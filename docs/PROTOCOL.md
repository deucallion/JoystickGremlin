# Wire protocol

The Mumble plugin and the overlay communicate one-way over **localhost UDP**.
The plugin sends, the overlay listens. The default endpoint is
`127.0.0.1:27812`, overridable on both sides:

| Side    | How to override                                              |
| ------- | ------------------------------------------------------------ |
| Plugin  | env `MUMBLE_VOICE_OVERLAY_HOST` / `MUMBLE_VOICE_OVERLAY_PORT` |
| Overlay | `host` / `port` in `config.json`, or the same env vars       |

UDP is used because it can never block Mumble's main thread and localhost
delivery is effectively lossless. Because it is connectionless, the overlay and
the plugin can start in any order and reconnect freely.

## Framing

Each datagram contains one or more **newline-delimited JSON objects** (UTF-8).
Every object has a `t` (type) field. Unknown types and malformed lines are
ignored by the overlay, so the protocol can grow without breaking older
overlays.

## Messages (plugin → overlay)

### `meta`
Plugin lifecycle.
```json
{"t":"meta","event":"hello","protocol":1,"plugin":"Mumble Voice Overlay (Star Citizen)"}
{"t":"meta","event":"bye"}
```
`event` is `hello` (sent on load) or `bye` (sent on unload, clears the overlay).

### `server`
Server connection lifecycle.
```json
{"t":"server","event":"synchronized"}
{"t":"server","event":"disconnected"}
```
`disconnected` clears all speakers.

### `self`
The local user's own audio status.
```json
{"t":"self","id":42,"muted":false,"deafened":false,"mode":"push-to-talk"}
```
`mode` is one of `continuous`, `voice-activation`, `push-to-talk`, `unknown`.

### `user`
Everything Mumble exposes about a user. Sent for everyone in your channel on
sync, when someone joins your channel, and (defensively) the first time a user
talks.
```json
{
  "t":"user","id":1,"name":"Reaver",
  "channel":"Alpha Squad","channelId":10,
  "comment":"Gunner. Comms on tac-1.",
  "hash":"a1b2c3d4...",
  "locallyMuted":false,
  "self":false
}
```
`comment` is the raw Mumble comment (HTML); the overlay strips it to plain text.
`hash` is Mumble's stable per-user certificate hash (identity across restarts).

### `talk`
A talking-state transition — the high-frequency message.
```json
{"t":"talk","id":1,"state":"talking"}
```
`state` is one of:

| state        | meaning                                  | shown? |
| ------------ | ---------------------------------------- | ------ |
| `talking`    | normal channel speech                    | yes    |
| `whispering` | targeted whisper                         | yes    |
| `shouting`   | shout (e.g. to a linked/parent channel)  | yes    |
| `muted`      | trying to talk while muted               | yes    |
| `passive`    | stopped talking                          | no     |
| `invalid`    | unknown                                  | no     |

### `leave`
A user left your channel (or disconnected).
```json
{"t":"leave","id":1}
```

## Overlay behaviour

The overlay keeps a table of users keyed by `id`. A card appears while a user is
in an active `state` and lingers `linger_ms` (default 1200 ms) after they go
`passive`, so brief gaps don't make cards flicker.
