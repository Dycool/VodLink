# VodLink Privacy Policy

Effective date: 2026-06-30

VodLink is an open-source desktop app that records game sessions by creating YouTube Live broadcasts and streaming directly from your computer to your own YouTube channel. This policy explains what data VodLink handles, where it goes, and how you can delete it.

This policy describes the official VodLink app and the optional VodLink Cloudflare Worker included in this repository. If you fork, modify, self-host, or redistribute VodLink, your build and infrastructure may handle data differently.

## Summary

- VodLink streams video and audio from your computer directly to YouTube. VodLink does not run a server that stores your stream content.
- Your Google/YouTube account is used to create, manage, embed, and sync your own YouTube broadcasts, VODs, and clips.
- Most app data stays on your computer, including settings, cached VOD metadata, friends, detected games, clips, and sign-in refresh tokens.
- The optional Worker is used only for friend/session matching. It stores short-lived session metadata and deletes old rows automatically.
- VodLink does not sell personal data and does not include third-party ads or analytics.

## Data VodLink handles locally

VodLink may store the following on your computer:

- Google OAuth refresh token used to keep you signed in.
- Your Google account email, display name, and profile picture URL, when returned by Google sign-in.
- App settings such as stream resolution, FPS, bitrate, capture mode, encoder choice, worker URL, sharing toggle, and UI preferences.
- Local VOD and clip library entries, including YouTube video IDs, clip IDs, titles, descriptions, timestamps, games, owners, and thumbnail cache metadata.
- Your local friend list used for optional VOD matching.
- Detected or manually added games and executable names.
- App cache and private OBS runtime files extracted or used by the app.
- Debug logs, only when you intentionally run the app with debug logging enabled.

VodLink stores its app data inside its own local data folder. The app's reset option is intended to close VodLink and delete local VodLink data from this computer, including sign-in tokens, settings, friends, cached VODs, clips, games, and cache files.

## Google and YouTube data

When you sign in with Google, VodLink uses OAuth access granted by you to perform YouTube actions for your account. Depending on the app flow and the permissions you grant, VodLink may:

- Create YouTube Live broadcasts and live streams.
- Bind a broadcast to a stream key/ingest endpoint.
- Update broadcast/video metadata such as title, description, privacy status, embeddability, DVR/archive settings, recording status, and public stats visibility where supported by YouTube.
- Start, stop, complete, and sync broadcasts/VODs.
- Read your VodLink-created or channel-associated VOD metadata so the local library can stay in sync.
- Create and track YouTube Clips when supported by your account and YouTube.
- Embed YouTube videos and thumbnails inside the app for playback and POV switching.

Your live video/audio content is sent from your computer to YouTube using YouTube RTMP/RTMPS ingest. YouTube processes, stores, publishes, deletes, and retains that content according to your YouTube account settings and Google's policies.

VodLink does not ask for your Google password. Sign-in happens through Google's OAuth flow. You can revoke VodLink's Google access from your Google Account security settings.

## Screen, game, desktop, and audio capture

VodLink captures only according to the mode you select:

- Game only: captures desktop video through OBS monitor/display capture, but blacks out the image when the detected game executable is not the foreground window. Audio is intended to be game/process audio only.
- Game and external audio: uses the same focus-gated desktop video, with system output audio and the default microphone.
- Desktop: captures the desktop visually with game-only audio.
- Desktop with external audio: captures desktop video, system output audio, and the default microphone.

Because desktop-style capture can include sensitive on-screen information when visible, you should verify the selected mode before streaming.

## Optional friend/session matching Worker

VodLink can optionally call the Cloudflare Worker in this repository to find mutual friends who streamed the same game during an overlapping time window. If sharing is off, the app does not need to call the Worker for matching.

When sharing is enabled, the Worker may receive and temporarily store:

- Your verified Google email address.
- Your Google display name and profile picture URL, if present in the verified Google ID token.
- The selected game name.
- Your YouTube video ID.
- Session start and stop timestamps.
- The friend email addresses you configured in VodLink.

The Worker returns a friend's VOD only when both users listed each other and streamed the same game during an overlapping window.

The Worker is designed to keep no permanent session state. It deletes sessions older than 24 hours during normal `/stop` cleanup. For rate limiting, it stores hashed IP/account-scope keys and fixed-window counters; it does not persist raw IP addresses or bearer tokens in the D1 tables.

## Data shared with other users

When VOD sharing is enabled and a mutual match is found, VodLink may show matched friends:

- Your display name and profile picture, if available.
- Your email address.
- Your YouTube video ID/link.
- The game name.
- Session start/stop timestamps used for real-time POV sync.

Do not enable sharing or add someone as a friend if you do not want that information to be available to matched mutual friends.

## Third-party services

VodLink may interact with:

- Google Sign-In / OAuth, for authentication and authorization.
- YouTube Data API, YouTube Live Streaming API, YouTube embeds, thumbnails, and RTMP/RTMPS ingest.
- Cloudflare Workers and D1, if you use the optional matching Worker.
- Operating-system APIs for foreground window detection, display capture, audio capture, game detection, GPU preference, and local storage.

Those services may process data under their own terms and privacy policies.

## What VodLink does not do

VodLink does not intentionally:

- Sell your personal data.
- Include third-party advertising SDKs.
- Include analytics or tracking SDKs.
- Upload your local friend list to any service except the optional matching Worker when sharing is enabled.
- Store your stream video/audio on a VodLink-operated backend.
- Read your installed OBS Studio profile, scenes, plugins, logs, cache, or configuration.

## Data deletion and control

You can control or delete data in several places:

- In VodLink, use the reset option to delete local VodLink data on this computer.
- In YouTube Studio, delete or edit videos, live broadcasts, clips, titles, descriptions, visibility, and related metadata.
- In your Google Account, revoke VodLink's OAuth access.
- In your self-hosted Worker/D1 database, delete rows or drop the database if you operate your own backend.

Deleting local VodLink data does not automatically delete videos already stored on YouTube. Deleting a YouTube video may leave a stale local entry until VodLink syncs or you remove the local entry.

## Security

VodLink uses Google's OAuth flow rather than collecting Google passwords. The Worker verifies Google ID tokens before accepting `/start` or `/stop` requests and uses rate limiting to reduce abuse. Network transport should be HTTPS/WSS in production.

No app can guarantee perfect security. Avoid streaming sensitive information and keep your operating system, drivers, browser/WebView runtime, and VodLink build up to date.

## Children

VodLink is intended for users who are old enough to use Google/YouTube and live streaming features under the rules that apply to their account and location. VodLink is not designed for children.

## Changes to this policy

This policy may change as VodLink changes. Material changes should be committed to this repository with an updated effective date.

## Contact

For privacy questions, open an issue in the VodLink repository or contact the repository maintainer through the contact method published on the repository profile.
