import { verifyIdToken, type VerifiedUser } from "./auth";

export interface Env {
  DB: D1Database;
  GOOGLE_CLIENT_ID: string;
  EDGE_RATE_LIMITER: {
    limit(input: { key: string }): Promise<{ success: boolean }>;
  };
}

const DAY_MS = 24 * 60 * 60 * 1000;
const MAX_BODY_BYTES = 8 * 1024;
const MAX_FRIENDS = 50;
const MAX_MATCHES = 50;
const SECURITY_HEADERS: Record<string, string> = {
  "Cache-Control": "no-store",
  "Content-Type": "application/json; charset=utf-8",
  "X-Content-Type-Options": "nosniff",
};

interface SessionRow {
  id: number;
  email: string;
  game: string;
  youtube_id: string;
  started_at: number;
  stopped_at: number | null;
  friends: string;
}

interface SessionMetadata {
  emails: string[];
  displayName: string;
  pictureUrl: string;
}

function json(data: unknown, status = 200, extra: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: { ...SECURITY_HEADERS, ...extra },
  });
}

function validEmail(value: string): boolean {
  return value.length <= 254 && /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(value);
}

function parseFriends(raw: unknown, ownEmail = ""): string[] {
  if (!Array.isArray(raw)) return [];
  const unique = new Set<string>();
  for (const item of raw) {
    if (typeof item !== "string") continue;
    const email = item.trim().toLowerCase();
    if (email !== ownEmail && validEmail(email)) unique.add(email);
    if (unique.size >= MAX_FRIENDS) break;
  }
  return [...unique];
}

function parseMetadata(raw: string): SessionMetadata {
  try {
    const value = JSON.parse(raw) as unknown;
    if (Array.isArray(value)) {
      return { emails: parseFriends(value), displayName: "", pictureUrl: "" };
    }
    if (value && typeof value === "object") {
      const record = value as Record<string, unknown>;
      return {
        emails: parseFriends(record.emails),
        displayName: typeof record.displayName === "string" ? record.displayName.slice(0, 100) : "",
        pictureUrl: safePictureUrl(record.pictureUrl),
      };
    }
  } catch {
    // Corrupt legacy metadata is treated as an empty friend list.
  }
  return { emails: [], displayName: "", pictureUrl: "" };
}

function safePictureUrl(raw: unknown): string {
  if (typeof raw !== "string" || raw.length > 2048) return "";
  try {
    const url = new URL(raw);
    if (url.protocol !== "https:") return "";
    const host = url.hostname.toLowerCase();
    return host === "googleusercontent.com" || host.endsWith(".googleusercontent.com")
      ? url.toString()
      : "";
  } catch {
    return "";
  }
}

async function hashKey(value: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function takeRateLimit(
  env: Env,
  key: string,
  limit: number,
  windowMs: number,
): Promise<{ allowed: boolean; retryAfter: number }> {
  const now = Date.now();
  const bucket = Math.floor(now / windowMs);
  const row = await env.DB.prepare(
    "INSERT INTO request_limits(key, bucket, count, expires_at) VALUES (?, ?, 1, ?) " +
      "ON CONFLICT(key, bucket) DO UPDATE SET count = count + 1 RETURNING count",
  )
    .bind(key, bucket, (bucket + 1) * windowMs)
    .first<{ count: number }>();
  const retryAfter = Math.max(1, Math.ceil(((bucket + 1) * windowMs - now) / 1000));
  return { allowed: (row?.count ?? limit + 1) <= limit, retryAfter };
}

function limited(retryAfter: number): Response {
  return json({ error: "Too many requests" }, 429, { "Retry-After": String(retryAfter) });
}

async function readObject(request: Request): Promise<Record<string, unknown> | null> {
  const declared = Number(request.headers.get("content-length") ?? "0");
  if (declared > MAX_BODY_BYTES) return null;
  const text = await request.text();
  if (new TextEncoder().encode(text).byteLength > MAX_BODY_BYTES) return null;
  try {
    const value = JSON.parse(text) as unknown;
    return value && typeof value === "object" && !Array.isArray(value)
      ? (value as Record<string, unknown>)
      : null;
  } catch {
    return null;
  }
}

async function handleStart(request: Request, env: Env, user: VerifiedUser): Promise<Response> {
  const body = await readObject(request);
  if (!body) return json({ error: "Invalid or oversized JSON body" }, 400);

  const game = typeof body.game === "string" ? body.game.trim() : "";
  const youtubeId = typeof body.youtubeId === "string" ? body.youtubeId.trim() : "";
  const startedAt = typeof body.startedAt === "number" ? body.startedAt : Number.NaN;
  const now = Date.now();
  if (!/^[\p{L}\p{N} .:'_+()\-]{1,80}$/u.test(game)
      || !/^[A-Za-z0-9_-]{11}$/.test(youtubeId)
      || !Number.isFinite(startedAt)
      || Math.abs(startedAt - now) > 10 * 60 * 1000) {
    return json({ error: "Invalid session data" }, 400);
  }

  const metadata: SessionMetadata = {
    emails: parseFriends(body.friends, user.email),
    displayName: user.name.slice(0, 100),
    pictureUrl: safePictureUrl(user.picture),
  };

  const existing = await env.DB.prepare(
    "SELECT email FROM sessions WHERE youtube_id = ? LIMIT 1",
  ).bind(youtubeId).first<{ email: string }>();
  if (existing) {
    return existing.email === user.email
      ? json({ ok: true })
      : json({ error: "Invalid session data" }, 400);
  }

  // Defensive invariant: an account can own only one open session. Closing an
  // old row also makes retried /start calls safe instead of multiplying state.
  await env.DB.batch([
    env.DB.prepare(
      "DELETE FROM sessions WHERE (stopped_at IS NOT NULL AND stopped_at < ?) " +
        "OR (stopped_at IS NULL AND started_at < ?)",
    ).bind(now - DAY_MS, now - DAY_MS),
    env.DB.prepare("DELETE FROM request_limits WHERE expires_at < ?").bind(now),
    env.DB.prepare("UPDATE sessions SET stopped_at = ? WHERE email = ? AND stopped_at IS NULL")
      .bind(now, user.email),
    env.DB.prepare(
      "INSERT INTO sessions(email, game, youtube_id, started_at, stopped_at, friends) " +
        "VALUES (?, ?, ?, ?, NULL, ?)",
    ).bind(user.email, game, youtubeId, startedAt, JSON.stringify(metadata)),
  ]);
  return json({ ok: true });
}

async function handleStop(env: Env, email: string): Promise<Response> {
  const now = Date.now();
  const mine = await env.DB.prepare(
    "SELECT * FROM sessions WHERE email = ? AND stopped_at IS NULL " +
      "ORDER BY started_at DESC LIMIT 1",
  ).bind(email).first<SessionRow>();

  const gc = env.DB.prepare(
    "DELETE FROM sessions WHERE (stopped_at IS NOT NULL AND stopped_at < ?) " +
      "OR (stopped_at IS NULL AND started_at < ?)",
  ).bind(now - DAY_MS, now - DAY_MS);
  const limitGc = env.DB.prepare("DELETE FROM request_limits WHERE expires_at < ?").bind(now);

  if (!mine) {
    await env.DB.batch([gc, limitGc]);
    return json({ vods: [] });
  }

  await env.DB.prepare("UPDATE sessions SET stopped_at = ? WHERE id = ?")
    .bind(now, mine.id).run();

  const myFriends = new Set(parseMetadata(mine.friends).emails);
  const candidates = await env.DB.prepare(
    "SELECT * FROM sessions WHERE game = ? AND email != ? " +
      "AND started_at <= ? AND (stopped_at IS NULL OR stopped_at >= ?) " +
      "ORDER BY started_at DESC LIMIT 200",
  ).bind(mine.game, email, now, mine.started_at).all<SessionRow>();

  const seen = new Set<string>();
  const vods: Array<Record<string, unknown>> = [];
  for (const row of candidates.results ?? []) {
    if (!myFriends.has(row.email.toLowerCase())) continue;
    const profile = parseMetadata(row.friends);
    if (!profile.emails.includes(email) || seen.has(row.youtube_id)) continue;
    seen.add(row.youtube_id);
    vods.push({
      email: row.email,
      name: profile.displayName,
      picture: profile.pictureUrl,
      youtubeId: row.youtube_id,
      game: row.game,
      startedAt: row.started_at,
      stoppedAt: row.stopped_at,
    });
    if (vods.length >= MAX_MATCHES) break;
  }
  await env.DB.batch([gc, limitGc]);
  return json({ vods });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const isStart = url.pathname === "/start" && request.method === "POST";
    const isStop = url.pathname === "/stop" && request.method === "GET";
    if (!isStart && !isStop) return json({ error: "Not found" }, 404);

    try {
      const ip = request.headers.get("CF-Connecting-IP") ?? "unknown";
      const ipKey = await hashKey(`ip:${ip}`);
      const edgeLimit = await env.EDGE_RATE_LIMITER.limit({ key: ipKey });
      if (!edgeLimit.success) return limited(60);

      const user = await verifyIdToken(request.headers.get("Authorization"), env.GOOGLE_CLIENT_ID);
      if (!user) return json({ error: "Unauthorized" }, 401);

      const route = isStart ? "start" : "stop";
      const accountKey = await hashKey(`${route}:${user.email}`);
      const accountLimit = await takeRateLimit(env, accountKey, isStart ? 3 : 10, 15 * 60 * 1000);
      if (!accountLimit.allowed) return limited(accountLimit.retryAfter);

      return isStart ? await handleStart(request, env, user) : await handleStop(env, user.email);
    } catch (error) {
      console.error("request failed", error instanceof Error ? error.message : "unknown");
      return json({ error: "Internal error" }, 500);
    }
  },
};
