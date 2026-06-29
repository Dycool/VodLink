# VodLink session matchmaker (Cloudflare Worker)

This Worker is deliberately tiny. It keeps **no permanent state** and exposes
**two routes**, costing exactly **two requests per shared play session**:

| Method | Path     | Auth            | Purpose |
|--------|----------|-----------------|---------|
| `POST` | `/start` | Google ID token | Record that I started streaming a game (with my YouTube id and my friend emails). |
| `GET`  | `/stop`  | Google ID token | Close my open session and return the YouTube links of **mutual friends** who streamed the **same game** during an **overlapping** window. |

The game list is hard-coded in the desktop app, so there is no catalog endpoint.

## How matching works

- Identity is the verified `email` claim of a Google **ID token** (RS256 JWT),
  validated against Google's public keys in [`src/auth.ts`](src/auth.ts). No
  passwords, no per-request upstream calls once the keys are cached.
- Friend lists live only in the desktop app and are sent in `POST /start`.
- A friend's link is returned from `/stop` only when **both sides listed each
  other** (mutual) and their `[start, stop]` windows overlap for the same game.
- Rows are garbage-collected (anything older than 24h) on every `/stop`.

## Free-tier friendly

Two requests per session and a single, self-cleaning D1 table. A toggle in the
app gates whether the app calls the Worker at all — with sharing off, zero
requests are made.

## Deploy

Prereqs: a Cloudflare account and Node 18+.

```bash
cd worker
npm install

# 1. Create the D1 database and copy the printed database_id into wrangler.toml
npx wrangler d1 create vodlink

# 2. Set GOOGLE_CLIENT_ID in wrangler.toml to the OAuth client id the app uses
#    (the Worker only accepts ID tokens whose `aud` matches it).

# 3. Apply the schema to the remote database
npm run db:init:remote

# 4. Deploy
npm run deploy
```

Local development:

```bash
npm run db:init      # applies schema.sql to the local D1
npm run dev          # http://127.0.0.1:8787
npm run typecheck    # tsc --noEmit
```

### CI deploy

`.github/workflows/deploy-worker.yml` deploys on pushes to `main` that touch
`worker/**`. Add a repository secret **`CLOUDFLARE_API_TOKEN`** (an API token
with the *Edit Cloudflare Workers* template, including D1) for it to work.

## Manual smoke test

```bash
TOKEN="<a real Google ID token whose aud == GOOGLE_CLIENT_ID>"
BASE="http://127.0.0.1:8787"

curl -s -X POST "$BASE/start" -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"game":"Minecraft","youtubeId":"abc123","startedAt":'"$(date +%s%3N)"',"friends":["friend@example.com"]}'

curl -s "$BASE/stop" -H "Authorization: Bearer $TOKEN"   # -> {"vods":[...]}
```

`/stop` returns a friend's VOD only when that friend also ran `POST /start` for
the same game during the window **and** listed your email (and you listed theirs).
