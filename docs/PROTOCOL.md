# EnhancedIRL OBS Switcher — agent protocol (v1)

This document describes everything the plugin sends to and receives from the EnhancedIRL
backend. It is part of the open-source plugin on purpose: anyone can check that the plugin
**never sends your stream key** and that **the backend cannot change where you stream to**.

Design rules that the protocol enforces:

1. **The stream key never leaves OBS.** The plugin only sends a keyed fingerprint of it
   (HMAC-SHA256 with a per-account salt) so the panel can tell you when two OBS are set up
   for different channels.
2. **The backend has no command to change the server, the key or any other OBS setting.**
   The only commands are `prepare`, `cut`, `start`, `commit` and `rollback`, all about the
   main stream output. The other messages it can send (`welcome`, `handedOff`) only change
   what the plugin shows.
3. **The plugin trusts nothing it receives.** Unknown messages are ignored, every field is
   validated, and a command that does not make sense in the current state is rejected.
   Text shown in OBS (account, names) is shown as plain text, and URLs opened in the browser
   must be `https` and plain.
4. **A lost backend never leaves you off the air.** After a `cut`, the plugin arms a
   dead-man timer: if no `commit` arrives in time, it starts streaming again on its own.

## 1. Linking an OBS to your account (pairing)

Device-code flow, similar to RFC 8628. No password ever goes through OBS.

```
Plugin                                  Backend                         Browser (logged in)
  | POST /obs-agent/pairing ---------------> |                                   |
  | <------ { pairingId, code, pollSecret,   |                                   |
  |           verifyUrl, expiresIn, interval}|                                   |
  | opens verifyUrl in the system browser ---------------------------------------> |
  |                                          | <---- POST /obs-agents/link {code} |
  | POST /obs-agent/pairing/{id}/token ----> |                                   |
  | <------ 202 pending ... 200 { agentId, agentToken, fingerprintSalt }          |
```

`POST /obs-agent/pairing` — unauthenticated, rate-limited.

```json
{ "name": "Streaming PC", "os": "windows", "obsVersion": "32.0.1", "pluginVersion": "0.1.0" }
```

Response `201`:

```json
{
  "pairingId": "b7c1…",
  "code": "QF4K-9WTZ",
  "pollSecret": "…32 random bytes, base64url…",
  "verifyUrl": "https://app.enhancedirl.com/obs/link?pairing=QF4K-9WTZ",
  "expiresIn": 600,
  "interval": 3
}
```

`POST /obs-agent/pairing/{pairingId}/token` with `{ "pollSecret": "…" }`:

| Status | Meaning |
|---|---|
| `202` | Not confirmed yet. Poll again after `interval` seconds. |
| `200` | `{ "agentId", "agentToken", "fingerprintSalt" }`. Returned **once**. |
| `403` | Denied: the account reached its OBS limit or its email is not verified. Body: `{ "error": "limit_reached" \| "email_not_verified" }` |
| `410` | Expired or unknown. Start again. |

`POST /obs-agent/pairing` can also answer `429 { "error": "too_many_pending" }` (too many pending
links from the same network) or `503 { "error": "disabled" \| "busy" }`.

While it waits, the plugin shows the code with a countdown taken from `expiresIn`. Cancelling
in the plugin just stops polling: nothing is sent, and the pending link expires on its own.
If the confirmation arrives at the same moment as the cancel, the link is kept (the user did
confirm it in the browser).

Before confirming, the panel shows **which device is asking** (the name and OS the plugin sent,
and how long ago). Nobody should confirm a code they did not start on their own OBS: whoever
created the code gets an OBS linked to the confirming account.

The plugin stores `agentToken` in its own config file (user-only permissions). The token can
be revoked from the panel at any time; the plugin then gets close code `4401` and forgets it.

## 2. The live connection

One outgoing WebSocket per OBS (works behind any NAT):

```
GET wss://api.enhancedirl.com/obs-agent
Authorization: Bearer <agentToken>
Sec-WebSocket-Protocol: eirl-switcher.v1
```

Messages are JSON text frames: `{ "type": "...", ... }`. The backend sends WebSocket-level
pings every 20 s and drops a plugin that misses one; the plugin reconnects with backoff
(1 s → 30 s, with jitter) when it sees no traffic for 60 s.

`hello` must be the first message and is accepted once; anything sent before it is ignored.
More than 20 frames per second gets the connection closed with `1008`.

Close codes (the plugin does **not** retry on these):

| Code | Meaning |
|---|---|
| `4401` | Token revoked or unknown. The plugin forgets the token and shows "Not linked". |
| `4403` | The account can no longer use this OBS (plan limit). |
| `4409` | The same token connected from somewhere else. |
| `4426` | Plugin too old for the backend. The panel shows how to update. |

Retryable closes: `1012` (the backend is restarting), `1013` (the switcher is temporarily
disabled), `1008` (too many frames).

### 2.1 Plugin → backend

`hello` — first message after connecting.

```json
{ "type": "hello", "protocol": 1, "pluginVersion": "0.1.0", "obsVersion": "32.0.1",
  "os": "windows", "capabilities": ["cut", "start"] }
```

`status` — on every change and at least every 30 s.

```json
{
  "type": "status",
  "streaming": true,
  "streamingSince": 1791587032151,
  "reconnecting": false,
  "serviceReady": true,
  "platform": "twitch",
  "serverHost": "euw30.contribute.live-video.net",
  "keyFingerprint": "3f9a…(64 hex)",
  "channelHint": { "platform": "twitch", "broadcasterId": "12345678" },
  "video": { "width": 1920, "height": 1080, "fps": 60 },
  "encoder": { "codec": "h264", "bitrateKbps": 6000 }
}
```

- `streamingSince`: when this OBS went on air, in milliseconds since the Unix epoch (wall
  clock). Only present while `streaming` is `true`. The plugin takes it from OBS's "streaming
  started" event (or, if OBS was already streaming when the plugin started, from the moment it
  noticed) and drops it when the stream stops. It does **not** start again when OBS reconnects
  on its own (`reconnecting`), nor when this OBS goes back on air after a switch's `cut` —
  the dead-man restart or a `rollback` — because the platform's broadcast never ended.
- `serviceReady`: the stream service is the EnhancedIRL Switcher service and has a server and
  a key. Without it the OBS can be shown but cannot take part in a switch.
- `serverHost`: host only, never the path (some custom URLs carry secrets in the path).
- `keyFingerprint`: `hex(HMAC-SHA256(fingerprintSalt, streamKey))`, or `null` if there is no key.
- `channelHint`: only public identifiers. For Twitch, the numeric broadcaster id that every
  Twitch key starts with (`live_<id>_…`). It lets the backend confirm with Twitch's public API
  that the broadcast survived the switch.

### 2.2 Backend → plugin (no answer expected)

`welcome` — right after `hello`, and again whenever something in it changes (for example when
the OBS is renamed in My OBS). The plugin accepts any number of them and keeps the latest
values. It shows them in OBS, so the user can see which account this OBS is linked to and the
name it has in My OBS.

```json
{ "type": "welcome", "accountName": "p***@example.com", "agentName": "Streaming PC",
  "panelUrl": "https://app.enhancedirl.com/obs" }
```

- `panelUrl`: absolute URL of My OBS, opened by the plugin's "Open My OBS" buttons. Optional:
  while it has not arrived (not linked yet, or an older backend), the plugin derives it from
  the API base — a host starting with `api.` becomes the same host with `app.` and the path
  `/obs` (`https://api.enhancedirl.com` → `https://app.enhancedirl.com/obs`); anything else
  falls back to `https://app.enhancedirl.com/obs`. A `welcome` without it keeps the previous one.

`handedOff` — a switch asked for from My OBS moved the stream from this OBS to another one.
The plugin shows a notice (not modal) saying where the stream is now and when it was asked
for; a newer notice replaces an older one. The Cloud OBS build never shows it.

```json
{ "type": "handedOff", "toName": "Cloud OBS", "at": 1791587032151 }
```

- `toName`: name of the OBS that is on air now.
- `at`: when the switch was asked for, in milliseconds since the Unix epoch.

### 2.3 More plugin → backend messages

`event` — something happened locally. `at` is in milliseconds since the Unix epoch.

```json
{ "type": "event", "name": "streaming_started", "at": 1791587032151 }
```

Names: `streaming_started`, `streaming_stopped`, `streaming_reconnecting`, `deadman_restart`.

`result` — answer to a command (same `id`).

```json
{ "type": "result", "id": "c-17", "ok": false, "error": "not_streaming" }
```

### 2.4 Backend → plugin: commands

Every command carries an `id` (echoed in `result`) and the `switchId` it belongs to.

| Command | What the plugin does | Answers |
|---|---|---|
| `prepare` | Checks it could start streaming right now (service ready, not streaming). Changes nothing. | `ok`, or `not_ready` / `already_streaming` |
| `cut` `{ deadManMs? }` | Stops the main stream **without the RTMP goodbye** (no `FCUnpublish`/`deleteStream`), so the platform treats it as a dropped connection and keeps the broadcast open. With `deadManMs`, arms the dead-man timer. If a `start` is still in progress, it is cancelled the same way. | `ok` once the connection is closed, or `not_streaming` |
| `start` `{ ingest? }` | Starts the main stream with the current settings. On YouTube, `ingest: "primary" \| "backup"` picks one of its two ingests (`a.`/`b.` server, `?backup=1`): the switch puts the new OBS on the ingest the live one is not using, and YouTube moves over without a gap when the live one stops. On Kick the plugin adds an Amazon IVS takeover priority by itself. | `ok` when OBS reports the stream started, or `start_failed` (with OBS's message in `detail`) |
| `commit` | Disarms the dead-man timer. Idempotent: the backend may send it more than once. | `ok` |
| `rollback` | Starts streaming again right away (the switch failed on the other OBS). If it is already streaming, does nothing. | `ok` / `start_failed` |

### 2.5 What a switch looks like

```
backend → B  prepare            B → result ok
backend → A  cut (deadManMs)    A → result ok        (A is off the air, platform shows its backup screen)
backend → B  start              B → result ok        (B is live; same broadcast, same uptime)
backend → A  commit             A → result ok        (dead-man disarmed)
```

If B fails to start within ~25 s, the backend sends `cut` to B (in case it comes up late) and
`rollback` to A. If A's answer to `cut` is lost, the backend sends `rollback` to A. If the backend
itself disappears after the `cut`, A's dead-man timer restarts A's stream.

## 3. Versioning

`protocol` in `hello` is an integer. The backend accepts the current version and the previous
one; older plugins get close code `4426`. New optional fields can appear in any message at
any time: both sides must ignore fields they do not know.

New message types can appear too. The plugin ignores a message whose `type` it does not know
and keeps the connection open, without answering it: a newer backend can add notices (as it
did with `handedOff`) without breaking older plugins. A command the plugin does not know gets
no `result`, so the backend times it out as it would with a lost answer.
