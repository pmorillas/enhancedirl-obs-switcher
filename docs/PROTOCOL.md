# EnhancedIRL OBS Switcher — agent protocol (v1)

This document describes everything the plugin sends to and receives from the EnhancedIRL
backend. It is part of the open-source plugin on purpose: anyone can check that the plugin
**never sends your stream key**, that **switching cannot change where you stream to**, and
that **remote control only works if you turn it on, and only for the actions listed in §3.5**.

Design rules that the protocol enforces:

1. **The stream key never leaves OBS.** The plugin only sends a keyed fingerprint of it
   (HMAC-SHA256 with a per-account salt) so the panel can tell you when two OBS are set up
   for different channels.
2. **Switching never touches your key or your destination.** The switch commands are
   `prepare`, `cut`, `start`, `commit` and `rollback`, all about the main stream output, and
   none of them changes the server, the key or any other OBS setting. The other messages the
   backend can send outside remote control (`welcome`, `handedOff`) only change what the
   plugin shows.
3. **Remote control is off unless you turn it on.** It is a switch per OBS in the plugin
   window, off by default and off again after unlinking. While it is off, every `obs_request`
   is refused. While it is on, the plugin runs only the requests in the closed list of §3.5
   and forwards only the events in that list. None of them reads or changes the stream key,
   the server, the stream service, other settings, profiles, scene collections, sources'
   settings, hotkeys or files. The list is checked **inside the plugin**, so it holds even if
   the backend asks for something else: such a request is refused without running.
4. **The plugin trusts nothing it receives.** Unknown messages are ignored, every field is
   validated, and a command that does not make sense in the current state is rejected.
   Text shown in OBS (account, names) is shown as plain text, and URLs opened in the browser
   must be `https` and plain.
5. **A lost backend never leaves you off the air.** After a `cut`, the plugin arms a
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
More than 20 frames per second gets the connection closed with `1008` (remote control's
`obs_response` and `obs_event` frames have their own limits, see §3.4). Frames are at most
64 KiB, except `obs_response`, which can be up to 3 MB.

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
{ "type": "hello", "protocol": 1, "pluginVersion": "0.2.0", "obsVersion": "32.0.1",
  "os": "windows", "capabilities": ["cut", "start", "control"] }
```

- `capabilities`: `cut` and `start` are always there. `control` (0.2.0 and later) means this
  OBS can be remote controlled (§3): it is missing in a Cloud OBS and in an OBS without OBS's
  WebSocket plugin 5.5 or later (bundled with OBS 30.2 and later). It says the plugin *can*
  do it; whether the user allowed it comes in `status`.

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
  "encoder": { "codec": "h264", "bitrateKbps": 6000 },
  "control": { "enabled": false }
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
- `control`: only when `hello` had the `control` capability. `enabled` is the user's switch
  in the plugin window. When the user flips it, a `status` goes out right away.

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

## 3. Remote control

Optional, per OBS, and **off by default** (design rule 3). With it on, the backend can run a
closed list of obs-websocket requests on this OBS and receive a closed list of its events.
That is what the panel's OBS controls, a shared control link and automatic scene switching
use. The backend speaks obs-websocket v5 to those clients and turns their requests into the
messages below.

### 3.1 How the plugin runs them

The plugin runs each allowed request **inside OBS** through the plugin API of the WebSocket
plugin that comes with OBS (`obs_websocket_call_request`), and gets the events from the same
API. No WebSocket server, port or password is involved: it works with OBS's own WebSocket
server turned off, and the answers and errors are exactly obs-websocket's.

Requests run one at a time, in order, on a worker thread of the plugin, never on the
connection's thread. While one has run in the last 10 seconds, the plugin window shows
"Being controlled from EnhancedIRL".

### 3.2 Backend → plugin

`obs_request` — run one request.

```json
{ "type": "obs_request", "id": "5f0c…", "requestType": "SetCurrentProgramScene",
  "requestData": { "sceneName": "Gameplay" } }
```

- `id`: 1–64 characters, echoed in the answer. A request without a usable `id` is ignored.
- `requestType`: obs-websocket's request name. It must be in the closed list (§3.5).
- `requestData`: an object or `null`. Only the fields listed for that request are kept, with
  the types obs-websocket documents; anything else is dropped before the request runs.

`obs_subscribe` — the events the backend wants from now on: the union of what its open
sessions for this OBS ask for. It replaces the previous subscription; an empty list means no
events. Names outside the event list (§3.5) are ignored. The subscription ends with the
connection: after reconnecting, the backend sends it again.

```json
{ "type": "obs_subscribe", "events": ["CurrentProgramSceneChanged", "InputMuteStateChanged"] }
```

### 3.3 Plugin → backend

`obs_response` — the answer to an `obs_request` (same `id`), with obs-websocket's
`RequestStatus` code and comment, and its `responseData` as is.

```json
{ "type": "obs_response", "id": "5f0c…", "ok": true, "code": 100, "comment": null,
  "responseData": null }
```

Answers the plugin gives without running the request:

| `code` | `comment` | When |
|---|---|---|
| `204` | `not allowed by EnhancedIRL Switcher` | Remote control is off (also for requests already queued when it was turned off), the request is not in the list, or this OBS has no remote control at all. |
| `205` | `Too many requests are waiting…` / `The request expired…` / `OBS is closing.` | More than 64 requests waiting; a request that waited more than 10 s (the backend has already given up on it); OBS is closing. |
| `207` | `OBS is not ready yet.` | OBS has not finished loading. |
| `400` | ``The field `x` is too long.`` / ``The field `imageFormat` must be jpg or png.`` | A text field over 512 bytes; a screenshot in another format. |
| `401` | ``The field `x` must be a string.`` (or a boolean, a number, an integer, an object) | A field with the wrong type, or `requestData` that is not an object. |
| `402` | ``The field `imageWidth` is out of range.`` | A screenshot side outside 1–16384. |
| `702` | `The response is larger than 3 MB…` | The answer would not fit in a frame (ask for a smaller or JPG image). |

`GetVersion` is answered by OBS, except that `availableRequests` lists only the closed list
and `supportedImageFormats` only `jpeg`, `jpg` and `png`.

`obs_event` — an event from the list that the backend subscribed to, with obs-websocket's
`eventType`, `eventIntent` (its subscription bit) and `eventData`. Changes made by anyone
(in OBS itself, or through remote control) are reported the same way.

```json
{ "type": "obs_event", "eventType": "CurrentProgramSceneChanged", "eventIntent": 4,
  "eventData": { "sceneName": "Gameplay", "sceneUuid": "64bb…" } }
```

### 3.4 Limits

- **Screenshots** (`GetSourceScreenshot`): JPG or PNG, at most 1280 pixels wide. A wider
  request — or none, or only a height that would make it wider — is scaled down to 1280 wide
  keeping the aspect ratio.
- **Answers**: an `obs_response` is at most 3,000,000 bytes; every other frame stays under
  64 KiB (a bigger event is dropped).
- **Events**: their own bucket of 50 per second with bursts of 200, apart from the 20 frames
  per second of the rest of the protocol. When it runs low, `InputVolumeChanged` is dropped
  first: the last 50 tokens are kept for the other events (scenes, stream, recording, mute).
- **Queue**: at most 64 requests waiting; the backend times a request out after 10 s.

### 3.5 The closed list

The same list is in the plugin's code (`src/agent/remote-control.cpp`) and here. Request and
event names and fields are obs-websocket v5's.

| Request | What it does | Fields kept |
|---|---|---|
| `GetVersion` | OBS and plugin versions, what can be requested | — |
| `GetStats` | CPU, memory, FPS, dropped frames | — |
| `GetSceneList` | The scenes and the current ones | — |
| `GetCurrentProgramScene` | The scene on air | — |
| `SetCurrentProgramScene` | Switches the scene on air | `sceneName`, `sceneUuid` |
| `GetCurrentPreviewScene` | The Studio Mode preview scene | — |
| `SetCurrentPreviewScene` | Changes the Studio Mode preview scene | `sceneName`, `sceneUuid` |
| `GetStudioModeEnabled` | Whether Studio Mode is on | — |
| `SetStudioModeEnabled` | Turns Studio Mode on or off | `studioModeEnabled` |
| `TriggerStudioModeTransition` | Puts the preview scene on air | — |
| `GetCurrentSceneTransition` | The current transition | — |
| `GetCurrentSceneTransitionCursor` | How far the transition is | — |
| `GetStreamStatus` | Stream state, time, congestion, dropped frames | — |
| `StartStream` | Starts the stream with your current settings | — |
| `StopStream` | Stops the stream | — |
| `GetRecordStatus` | Recording state and time | — |
| `StartRecord` | Starts recording with your current settings | — |
| `StopRecord` | Stops recording | — |
| `PauseRecord` | Pauses the recording | — |
| `ResumeRecord` | Resumes the recording | — |
| `GetInputList` | Names and kinds of the sources (not their settings) | `inputKind` |
| `GetInputVolume` | A source's volume | `inputName`, `inputUuid` |
| `SetInputVolume` | Changes a source's volume | `inputName`, `inputUuid`, `inputVolumeMul`, `inputVolumeDb` |
| `GetInputMute` | Whether a source is muted | `inputName`, `inputUuid` |
| `SetInputMute` | Mutes or unmutes a source | `inputName`, `inputUuid`, `inputMuted` |
| `ToggleInputMute` | Flips a source's mute | `inputName`, `inputUuid` |
| `GetSourceScreenshot` | A preview image of a scene or source, returned in the answer (never saved) | `sourceName`, `sourceUuid`, `imageFormat`, `imageWidth`, `imageHeight`, `imageCompressionQuality` |

| Event | When |
|---|---|
| `CurrentProgramSceneChanged` | The scene on air changed |
| `CurrentPreviewSceneChanged` | The Studio Mode preview scene changed |
| `SceneListChanged` | Scenes were added, removed or reordered |
| `StudioModeStateChanged` | Studio Mode was turned on or off |
| `StreamStateChanged` | The stream started, stopped or is reconnecting |
| `RecordStateChanged` | The recording started, stopped or paused |
| `InputVolumeChanged` | A source's volume changed |
| `InputMuteStateChanged` | A source was muted or unmuted |

### 3.6 Left out on purpose

Everything that is not in §3.5 is refused. In particular, and on purpose:

| Left out | Why |
|---|---|
| `GetStreamServiceSettings`, `SetStreamServiceSettings` | They read or change your stream key and server. |
| `SaveSourceScreenshot` | It writes a file on your computer (the preview image of `GetSourceScreenshot` only travels in the answer). |
| Creating, removing or editing sources and their settings (`CreateInput`, `RemoveInput`, `GetInputSettings`, `SetInputSettings`…), scene items and filters | They change your scenes' content, and a source's settings can hold paths, URLs or keys. |
| Profiles and scene collections (`SetCurrentProfile`, `SetProfileParameter`, `SetCurrentSceneCollection`…) | A profile holds the stream settings; collections replace all your scenes. |
| Output, video and record settings (`SetVideoSettings`, `SetRecordDirectory`, `SetOutputSettings`…) | They change how and where OBS writes and streams. |
| `CallVendorRequest`, `BroadcastCustomEvent` | They would reach other plugins, including this one's own hooks. |
| Hotkeys (`TriggerHotkeyByName`, `TriggerHotkeyByKeySequence`) | A hotkey can do anything OBS or another plugin can. |
| Projectors, dialogs, media control, replay buffer, virtual camera, `Sleep` and batches | Not needed by EnhancedIRL; they stay out until there is a reason, and then they are added to this list first. |

### 3.7 Who sees what

- A **Cloud OBS** never advertises `control` and refuses every `obs_request`: it is controlled
  in another way.
- The switch is stored with the link in the plugin's config file. A new link always starts
  with remote control off; unlinking the OBS (or the backend revoking it, close code `4401`)
  turns it off.
- In builds without a window (no Qt), nobody could see the switch, so remote control stays off.

## 4. Versioning

`protocol` in `hello` is an integer. The backend accepts the current version and the previous
one; older plugins get close code `4426`. New optional fields can appear in any message at
any time: both sides must ignore fields they do not know.

New message types can appear too. The plugin ignores a message whose `type` it does not know
and keeps the connection open, without answering it: a newer backend can add notices (as it
did with `handedOff`) without breaking older plugins. A command the plugin does not know gets
no `result`, so the backend times it out as it would with a lost answer.
