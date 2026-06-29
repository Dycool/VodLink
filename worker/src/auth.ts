// Verifies Google OpenID Connect ID tokens (RS256 JWTs) with zero per-request
// upstream calls once Google's signing keys are cached. The verified `email`
// claim is the only identity the Worker trusts.

export interface VerifiedUser {
  email: string;
  name: string;
  picture: string;
}

interface GoogleJwk {
  kid: string;
  n: string;
  e: string;
  alg?: string;
  kty: string;
}

// In-memory JWK cache, shared across requests on the same isolate. Refreshed
// when Google's Cache-Control max-age elapses.
let jwkCache: { keys: GoogleJwk[]; expiresAt: number } | null = null;
let jwkRefresh: Promise<GoogleJwk[]> | null = null;

const GOOGLE_CERTS_URL = "https://www.googleapis.com/oauth2/v3/certs";
const VALID_ISSUERS = new Set([
  "https://accounts.google.com",
  "accounts.google.com",
]);

function base64UrlToBytes(input: string): Uint8Array {
  const padded = input.replace(/-/g, "+").replace(/_/g, "/");
  const binary = atob(padded.padEnd(Math.ceil(padded.length / 4) * 4, "="));
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function decodeJson(segment: string): Record<string, unknown> {
  return JSON.parse(new TextDecoder().decode(base64UrlToBytes(segment)));
}

async function googleKeys(): Promise<GoogleJwk[]> {
  const now = Date.now();
  if (jwkCache && jwkCache.expiresAt > now) return jwkCache.keys;

  if (jwkRefresh) return jwkRefresh;
  jwkRefresh = refreshGoogleKeys(now);
  try {
    return await jwkRefresh;
  } finally {
    jwkRefresh = null;
  }
}

async function refreshGoogleKeys(now: number): Promise<GoogleJwk[]> {
  const response = await fetch(GOOGLE_CERTS_URL, {
    headers: { Accept: "application/json" },
  });
  if (!response.ok) {
    throw new Error(`Failed to fetch Google certs: HTTP ${response.status}`);
  }
  const body = (await response.json()) as { keys: GoogleJwk[] };

  // Honour Google's cache lifetime so verification stays offline between refreshes.
  let maxAge = 3600;
  const cacheControl = response.headers.get("cache-control");
  const match = cacheControl?.match(/max-age=(\d+)/);
  if (match) maxAge = parseInt(match[1], 10);

  jwkCache = { keys: body.keys, expiresAt: now + maxAge * 1000 };
  return body.keys;
}

async function importKey(jwk: GoogleJwk): Promise<CryptoKey> {
  return crypto.subtle.importKey(
    "jwk",
    { kty: jwk.kty, n: jwk.n, e: jwk.e, alg: "RS256", ext: true },
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    false,
    ["verify"],
  );
}

/**
 * Verifies a Google ID token and returns the verified user, or null if the
 * token is missing, malformed, expired, signed by an unknown key, or addressed
 * to a different client.
 */
export async function verifyIdToken(
  authorization: string | null,
  clientId: string,
): Promise<VerifiedUser | null> {
  if (!authorization) return null;
  const token = authorization.replace(/^Bearer\s+/i, "").trim();
  if (token.length === 0 || token.length > 8192) return null;
  const parts = token.split(".");
  if (parts.length !== 3) return null;
  if (parts.some((part) => part.length === 0 || part.length > 4096)) return null;

  let header: Record<string, unknown>;
  let payload: Record<string, unknown>;
  try {
    header = decodeJson(parts[0]);
    payload = decodeJson(parts[1]);
  } catch {
    return null;
  }

  if (header.alg !== "RS256" || typeof header.kid !== "string" || header.kid.length > 200) {
    return null;
  }

  let keys: GoogleJwk[];
  try {
    keys = await googleKeys();
  } catch {
    return null;
  }
  const jwk = keys.find((k) => k.kid === header.kid);
  if (!jwk) return null;

  let valid = false;
  try {
    const key = await importKey(jwk);
    const signedData = new TextEncoder().encode(`${parts[0]}.${parts[1]}`);
    valid = await crypto.subtle.verify(
      "RSASSA-PKCS1-v1_5",
      key,
      base64UrlToBytes(parts[2]),
      signedData,
    );
  } catch {
    return null;
  }
  if (!valid) return null;

  const now = Math.floor(Date.now() / 1000);
  if (typeof payload.exp !== "number" || payload.exp < now) return null;
  if (typeof payload.iat !== "number" || payload.iat > now + 300) return null;
  if (typeof payload.nbf === "number" && payload.nbf > now + 60) return null;
  if (typeof payload.iss !== "string" || !VALID_ISSUERS.has(payload.iss)) return null;
  if (payload.aud !== clientId) return null;
  if (payload.email_verified !== true) return null;
  if (typeof payload.email !== "string" || !payload.email) return null;

  const email = (payload.email as string).toLowerCase();
  if (email.length > 254) return null;
  return {
    email,
    name: typeof payload.name === "string" ? payload.name.slice(0, 100) : "",
    picture: typeof payload.picture === "string" ? payload.picture.slice(0, 2048) : "",
  };
}
