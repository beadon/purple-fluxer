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
- **DM history on login** — DM channels are not fetched/replayed on connect; unread DMs are invisible until a new message arrives. Should fetch last N messages for each DM channel in the READY payload's `private_channels` list, same as guild channel history on `fluxer_join_chat`

---

## Prior art / reference implementations

- https://github.com/EionRobb/purple-discord — closest structural reference
- https://github.com/hoehermann/purple-gowhatsapp — good libpurple 2.x patterns
- https://github.com/matrix-org/purple-matrix — another 2.x reference
