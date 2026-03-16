# purple-fluxer

libpurple 2.14.x protocol plugin for [Fluxer](https://fluxer.app) — a free,
open-source, self-hostable Discord-compatible instant messaging platform.

Fluxer's API is intentionally Discord-compatible. The WebSocket gateway
opcodes are identical, so this plugin is spiritually derived from
purple-discord but written fresh against Fluxer's documented API.

---

## Current state

The plugin compiles cleanly against libpurple 2.14.x and is functional for
day-to-day text messaging. All earlier API binding issues have been resolved.

Working: login (email+password or token), CAPTCHA challenge/response dialog
(opens browser + token paste), WebSocket gateway, heartbeat, guild/channel
buddy list with category nesting, channel history on open, `/more` pagination,
message send/receive, edit/delete notifications, guild member list population
via `REQUEST_GUILD_MEMBERS`, presence sync, room list, DMs, unread channel
indicators (`unseen-count` node data + notice on open), typing indicators
(send + receive for DMs), Discord markdown rendering (incoming + outgoing
round-trip), mention resolution (`<@id>` / `<#id>`), `@mention` tab highlight
(`PURPLE_MESSAGE_NICK`).

---

## Build

```bash
sudo apt install libpurple-dev libjson-glib-dev libglib2.0-dev
make
make install-user     # installs to ~/.purple/plugins/ (no sudo)
sudo make install     # system-wide install, includes icons automatically
```

`make install` depends on `install-icons` — icons are always co-installed.
`make install-user` does not install icons (no user-local icon path in Pidgin 2.x).

---

## Architecture reference

### Connection flow

```
fluxer_login()
  └─ stored token → fluxer_use_token()
  └─ else POST /auth/login → fluxer_got_login_response() → fluxer_use_token()
       └─ fluxer_ws_connect()  [SSL + WebSocket upgrade]
            └─ fluxer_ssl_connected_cb()  [sends HTTP 101 Upgrade request]
                 └─ fluxer_ssl_recv_cb()  [reads frames]
                      └─ ws_process_recv_buf()  [parses RFC 6455 WS frames]
                           └─ handles opcodes:
                                OP_HELLO (10)         → heartbeat + IDENTIFY
                                OP_HEARTBEAT_ACK (11) → set hb_ack_received=TRUE
                                OP_HEARTBEAT (1)      → fluxer_ws_send_heartbeat_payload()
                                                        (does NOT reset ACK state —
                                                         avoids false zombie detection)
                                OP_DISPATCH (0)       → fluxer_handle_dispatch()
```

### Session expiry and recovery flow

```
Normal session loss (web client logout or server kick):

  Server sends op:9 (OP_INVALID_SESSION, resumable=false)
    └─ ws_process_recv_buf: clear fd->session_id
       call purple_connection_error_reason(NETWORK_ERROR)
       return  ← fd may be freed after this
         └─ Pidgin auto-reconnect fires
              └─ fluxer_login(): stored token still present
                   └─ fluxer_use_token() → IDENTIFY with old token
                        └─ Server sends WS CLOSE(4004) — token was invalidated
                             └─ ws_process_recv_buf WS_OP_CLOSE handler:
                                  ws_closing = TRUE  (prevents double-CLOSE)
                                  purple_account_set_string("token", "")
                                  if (password stored):
                                    error_reason(NETWORK_ERROR) → auto-reconnect
                                  else:
                                    error_reason(AUTH_FAILED) → user must reconnect
                                      └─ fluxer_login(): no token, has password
                                           └─ POST /auth/login
                                                └─ CAPTCHA_REQUIRED response
                                                     └─ fluxer_got_login_response:
                                                          xdg-open https://fluxer.app
                                                          purple_request_input dialog
                                                          user pastes token → fluxer_captcha_got_token
                                                          token stored → fluxer_use_token() → connected

WS CLOSE codes handled:
  4004 = auth_failed   (token rejected — clear token, fall back to email+password)
  4006 = session_invalid (same treatment as 4004)
  other = generic network error → error_reason(NETWORK_ERROR) → auto-reconnect with same token

Double-CLOSE prevention:
  fd->ws_closing flag — set on first CLOSE frame.
  fluxer_ssl_recv_cb checks it at entry and returns immediately if set,
  preventing a second purple_connection_error_reason from the SSL error path.

RESUME: session_id and resume_gateway_url are stored from READY but RESUME
  (op:6) is not yet sent. op:7 (RECONNECT) triggers a fresh IDENTIFY on
  reconnect. Implementing RESUME is a TODO.
```

### Key gateway events handled

| Event | Handler |
|---|---|
| `READY` | `handle_ready()` — self user_id, session_id, seeds user/DM/guild maps |
| `GUILD_CREATE` | `handle_guild_create()` — builds channel hierarchy, sends REQUEST_GUILD_MEMBERS |
| `GUILD_MEMBERS_CHUNK` | `handle_guild_members_chunk()` — fills participant lists in open chats |
| `CHANNEL_CREATE` | `handle_channel_create()` — registers channel in maps |
| `MESSAGE_CREATE` | `handle_message_create()` — routes to DM or guild chat |
| `MESSAGE_UPDATE` | `handle_message_update()` — edit notice + re-posts new content |
| `MESSAGE_DELETE` | `handle_message_delete()` — deletion notice with attribution |
| `TYPING_START` | `handle_typing_start()` — DM typing via `serv_got_typing`; guild channels silently dropped (libpurple limitation) |
| `PRESENCE_UPDATE` | `handle_presence_update()` — maps status to libpurple status |

### Data structures in FluxerData

- `channel_to_guild`: `channel_id → guild_id` (NULL value = DM)
- `channel_names`: `channel_id → channel_name`
- `guild_names`: `guild_id → guild_name`
- `dm_channels`: `user_id → channel_id`
- `chat_id_map`: `channel_id → int` (libpurple chat ID for guild channels)
- `seeded_channels`: `channel_id → 1` (session-level dedup for buddy list)
- `user_names`: `user_id → username` (seeded from READY, MESSAGE_CREATE, member chunks)
- `oldest_msg_id`: `channel_id → snowflake` (cursor for `/more` pagination)
- `read_states`: `channel_id → last_read_msg_id` (from READY payload; used for unread detection)
- `channel_last_msg`: `channel_id → last_message_id` (newest known message per channel; from GUILD_CREATE channel objects)
- `guild_members`: `guild_id → GList* of usernames` (for chat room population)

### Guild/channel buddy list layout

Channels appear as chats under Pidgin groups named **"Guild / Category"**.
`handle_guild_create` uses a two-pass approach: pass 1 collects type=4
category channels; pass 2 seeds text/announcement channels under the
compound group name.

Duplicate prevention:
1. `seeded_channels` — session-level (prevents double-seed when GUILD_CREATE
   fires after READY for the same guild within a session)
2. `fluxer_chat_exists()` — direct blist scan — cross-session guard
   (`purple_blist_find_chat` is unreliable during the connecting phase)

### Channel history and `/more`

`fluxer_join_chat` fetches the last 50 messages on channel open via
`GET /channels/{id}/messages?limit=50`. The API returns newest-first; the
callback (`fluxer_got_history_cb`) reverses order before display. The oldest
message ID (array index n-1) is stored in `oldest_msg_id` as a pagination
cursor.

The `/more` slash command uses `?before={oldest_id}` to page further back.
History blocks loaded by `/more` are wrapped with labelled separator notices
(header: date range; footer: "live messages follow") because `serv_got_chat_in`
always appends — there is no insert-at-position API in libpurple 2.x.

`HistoryFetchData.show_separator` controls this: `FALSE` for the silent
initial load on join, `TRUE` for explicit `/more` fetches.

### Guild member loading

Fluxer sends only the requesting user's own member record in the initial
`GUILD_CREATE` payload (`members` array has 1 entry). The full list is
fetched via `OP_REQUEST_MEMBERS` (op 8) sent immediately after
`handle_guild_create`. The server responds with one or more
`GUILD_MEMBERS_CHUNK` dispatch events. Each chunk:
- updates `user_names`
- appends to `guild_members[guild_id]` (using `g_hash_table_steal` to avoid
  double-free when replacing the existing GList)
- calls `purple_conv_chat_add_users` on any already-open chat windows for
  that guild

### Heartbeat / zombie detection

`fluxer_gateway_send_heartbeat()` resets `hb_ack_received = FALSE` then
sends via `fluxer_ws_send_heartbeat_payload()`. The timer callback checks
the flag before each subsequent send.

Server-initiated heartbeat requests (`op:1`) are answered by calling
`fluxer_ws_send_heartbeat_payload()` directly — this bypasses the ACK
reset. Without this separation, a server op:1 arriving concurrently with
the regular timer caused false zombie disconnects.

### HTTP layer

All HTTP uses `purple_util_fetch_url_request_data_len_with_account`
(libpurple 2.x API). Every request builds a raw HTTP/1.0 request string
with Authorization, Origin, and optionally Content-Type headers embedded.
The `Origin: https://web.fluxer.app` header is required by Fluxer's CORS
check and is derived from the scheme+host of the request URL.

### Protocol icon

Pidgin 2.x constructs the icon path as
`/usr/share/pixmaps/pidgin/protocols/<size>/<prpl_name>.png` with no
user-local override. `make install-icons` generates a placeholder PNG and
installs it system-wide with `sudo`.

---

## API reference (Fluxer)

- Docs: https://docs.fluxer.app
- Gateway opcodes: https://docs.fluxer.app/gateway/opcodes
- Gateway events: https://docs.fluxer.app/gateway/events
- Auth resource: https://docs.fluxer.app/resources/auth
- OAuth2: https://docs.fluxer.app/topics/oauth2
- Base URL: `https://web.fluxer.app/api/v1`
- Gateway: `wss://gateway.fluxer.app/?v=1&encoding=json`

Key REST endpoints used:
- `POST /v1/auth/login` — email+password → token
- `POST /v1/users/@me/channels` — open DM channel
- `POST /v1/channels/{id}/messages` — send message
- `POST /v1/channels/{id}/typing` — typing indicator
- `GET /v1/channels/{id}/messages?limit=50[&before={id}]` — message history

---

## Upstream Pidgin/libpurple work items

Features that require changes to libpurple itself, identified through
purple-fluxer development:

1. **Nested buddy list groups** — single-level model (PurpleGroup → chats)
   prevents proper Guild → Category → Channel display.
2. **Scroll-to-top history hook** — no GTK scroll event exposed to prpl
   plugins; workaround is the `/more` command.
3. **Insert-at-timestamp conversation API** — `serv_got_chat_in` always
   appends; historical messages land below live messages.
4. **Protocol UI hints** — no mechanism for a plugin to suggest default
   window behaviour (e.g. hide participant list for large-guild channels).
5. **Blist entry bolding for unread closed channels** — `unseen-count` node
   data is written and a notice appears on open, but Pidgin only bolds blist
   chat entries when a `PurpleConversation` is open. Requires a new
   "unseen chat" signal or node-data hook in `gtkblist.c`.
6. **Typing indicators in guild chat rooms** — `serv_got_typing` /
   `send_typing` are IM-only APIs; no equivalent for `PurpleConvChat`.
   Proposed API: `serv_got_chat_typing(gc, chat_id, username, state)` and a
   `chat_send_typing` prpl op alongside `send_typing`.
7. **Per-guild display names (server nicknames)** — libpurple buddies have a
   single global alias. `get_cb_alias` has no room/guild context parameter so
   per-guild nickname overrides cannot be implemented at the plugin level.

---

## Pending plugin work

Features not yet implemented (contributions welcome):

- **Send images / file attachments** — set `OPT_PROTO_IM_IMAGE` to unlock Pidgin's Insert Image UI; implement multipart/form-data upload to `POST /channels/{id}/messages`; extract outgoing `<img id="N">` from message body via `purple_imgstore_get_by_id`
- **Receive non-image file attachments** — currently shown as `<a href>` links (functional); no inline preview
- **Buddy avatars** — download and cache per-user avatar URLs
- **`GUILD_MEMBER_ADD` / `GUILD_MEMBER_REMOVE`** — live member list updates
- **Message reply context** — quote preview above the reply body
- **Reactions display** — show emoji reaction counts on messages
- **MFA / TOTP login** — 6-digit TOTP code entry after email+password
- **`@everyone` / `@here` highlight** — add `PURPLE_MESSAGE_NICK` when message contains these strings
- **DM history on login** — implemented: user-configurable dropdown (`dm_history_mode`: auto/open/off). `auto` fetches history when IM window opens via `conversation-created` signal; `open` fetches all unread DMs at READY time. Deduped via `dm_history_fetched` hash.
- **User discriminator display** — Fluxer users have a `#NNNN` discriminator suffix (e.g. `beadon#1568`) visible in the web client. The `user` object in READY and member chunks includes a `discriminator` field. This should be stored in `user_names` (or a parallel `user_discriminators` map) and surfaced in Pidgin: at minimum shown in the chat participant tooltip / `get_cb_real_name`, ideally also in the account display name set via `purple_connection_set_display_name` so the user can see their own tag.
- **Friend requests** — READY payload `relationships` array contains pending requests (type values map to friend/pending/blocked). Need incoming request notification + accept/deny UI (likely via `purple_request_action`)
- **Open DMs persistence** — `private_channels` in READY represents the user's persistent DM list. Should restore open DM conversations across sessions and show unread indicators on buddy entries
- **History fetch max_len for JSON** — `fluxer_http_request` passes `-1` (defaults to 512 KB) for all fetches including message history. Fine for typical history payloads but will silently truncate if 50 messages contain large embeds/content. Consider passing an explicit limit (e.g. 4 MB) for history fetches the same way image fetches use 10 MB.
- **Handoff login flow (replaces CAPTCHA browser workaround)** — Fluxer implements an `AUTH_HANDOFF` API that allows a native client to get a token from an already-authenticated web session, bypassing CAPTCHA entirely. All three plugin-facing endpoints are `skipAuth: true` (no Authorization header needed).
  **Flow:**
  1. Plugin: `POST /auth/handoff/initiate` (no auth, no body) → `{code: "<uuid-string>"}` — strip dashes, display first 12 hex chars as two groups of 6: e.g. `A1B2C3 D4E5F6`
  2. Plugin shows a Pidgin dialog: "Your code is ready! Enter this code in the Fluxer web client: **A1B2C3 D4E5F6**"
  3. User opens `https://web.fluxer.app/login?desktop_handoff=1` in their browser → enters the code → web client calls `POST /auth/handoff/complete` with `{code, token, user_id}`
  4. Plugin polls `GET /auth/handoff/{code}/status` (e.g. every 2s) → response: `{status: "completed"|"pending"|"expired", token: "flx_...", user_id: "..."}`
  5. On `status == "completed"`: store token → connect gateway. On `status == "expired"` or timeout: fall back to existing CAPTCHA browser flow.
  **When to use:** Try handoff first on any fresh login (before email+password, which always triggers CAPTCHA). The existing CAPTCHA dialog remains the fallback. This eliminates the developer-tools token-copy requirement entirely.
  **Source confirmed from:** `ed64a32baf3d7010.js` (CAPTCHA bundle) and `4ad3f9a6a67c9e74.js` (main bundle) — functions `R()`, `L()`, `B()`, `U()` in the auth/handoff module. Status body shape confirmed: `{status, token, user_id}` from the desktop app's `consumeDesktopHandoffCode` bootstrap path.
- **Token expiry — automatic re-login on 4004/4006** — when the gateway sends WS CLOSE with code 4004 (auth failed) or 4006 (session invalidated), the plugin should automatically re-POST to `POST /auth/login` using the stored email+password (already in Pidgin account settings). On success, store the new token and reconnect — user sees nothing. If CAPTCHA is required (server returns `captcha_key`), fall through to the existing CAPTCHA browser flow. If only a token is stored (no password), show a clear error: "Session expired — re-enter your password in Account Settings → Password".
- **Token expiry — OAuth2 refresh token (future)** — Fluxer's `/oauth2/token` endpoint supports `grant_type=refresh_token` which would allow silent token renewal without re-entering credentials. However, the Fluxer Applications developer portal (Developer → Applications) currently cannot reveal or regenerate the client_secret or bot token (broken feature as of 2026-03-15). Revisit once Fluxer fixes application credential management. Implementation would add `Client ID` + `Client Secret` fields to Advanced settings, use the OOB authorization code flow on first connect, then silently refresh on 4004/4006.
- **Slash commands** — extend `fluxer_handle_slash_cmd` with the following guild/channel operations:
  - `/nick <new_nickname>` — PATCH `/guilds/{guild_id}/members/@me` with `nick` field
  - `/kick <username>` — DELETE `/guilds/{guild_id}/members/{user_id}`
  - `/ban <username>` — PUT `/guilds/{guild_id}/bans/{user_id}`
  - `/leave` / `/part` — DELETE `/users/@me/guilds/{guild_id}`
  - `/pinned` — GET `/channels/{id}/pins`, display each pinned message as a notice
  - `/roles` — GET `/guilds/{guild_id}/roles`, list role names as notices
  - `/threads` — GET `/channels/{id}/threads/active`, list thread names/IDs
  - `/thread <timestamp> <message>` — POST to thread channel by timestamp lookup
  - `/react <timestamp> <emoji>` — PUT `/channels/{id}/reactions/{emoji}/{message_id}/@me`
  - `/unreact <timestamp> <emoji>` — DELETE `/channels/{id}/reactions/{emoji}/{message_id}/@me`
  - `/reply <timestamp> <message>` — POST message with `message_reference.message_id` set
  - `/threadhistory` / `/thist <timestamp>` — fetch thread message history
  - `/grabhistory` / `/hist` — fetch full channel history (paginate until exhausted; warn on large channels)
  - `/servername` — display current guild name from `guild_names` map
  - `/joinserver <invite_code_or_url>` — POST `/invites/{code}` to join a guild

---

## Prior art / reference implementations

- https://github.com/EionRobb/purple-discord — closest structural reference
- https://github.com/hoehermann/purple-gowhatsapp — good libpurple 2.x patterns
- https://github.com/matrix-org/purple-matrix — another 2.x reference
