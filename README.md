# purple-fluxer

A **libpurple / Pidgin** protocol plugin for [Fluxer](https://fluxer.app) —
a free, open-source, self-hostable Discord-compatible instant messaging platform.

> **Alpha software.** The plugin is functional for day-to-day text messaging
> but lacks file attachments, MFA, and voice. Expect rough edges.

---

## What works (v0.1)

| Feature | Status |
|---|---|
| Email/password login | ✅ |
| Token-based login (bot or stored session) | ✅ |
| Gateway WebSocket connect + IDENTIFY | ✅ |
| Heartbeat keepalive (with op:1 server-request handling) | ✅ |
| Session RESUME on reconnect | ✅ |
| Receive DMs (`MESSAGE_CREATE`) | ✅ |
| Send DMs | ✅ |
| Receive guild channel messages | ✅ |
| Send guild channel messages | ✅ |
| Guild/category/channel buddy list hierarchy | ✅ (as "Guild / Category" groups) |
| Channel history on open (last 50 messages) | ✅ |
| `/more` command — paginate older history | ✅ |
| Message edit notifications (`MESSAGE_UPDATE`) | ✅ |
| Message delete notifications (`MESSAGE_DELETE`) | ✅ |
| Guild member list in chat room | ✅ (via REST `GET /guilds/{id}/members`) |
| Presence / status sync (`PRESENCE_UPDATE`) | ✅ |
| Set own presence (online/idle/dnd/invisible) | ✅ |
| Guild channel room list | ✅ |
| Self-hosted instance support | ✅ (set API base in Advanced) |
| Open DM from guild member list | ✅ |
| Personal notes channel (type 999) | ✅ |
| Correct sender name in guild chat | ✅ (`OPT_PROTO_UNIQUE_CHATNAME`) |
| Markdown rendering (bold/italic/code/strikethrough) | ❌ (TODO) |
| Mention resolution (`<@id>` → username, `<#id>` → channel) | ❌ (TODO) |
| Message reply context | ❌ (TODO) |
| File attachments (links + inline images) | ❌ (TODO) |
| Buddy avatars | ❌ (TODO) |
| Typing indicator send | ❌ (stub — needs channel lookup) |
| Typing indicator receive | ❌ (stub — needs user_id→name index) |
| `GUILD_MEMBER_ADD` / `GUILD_MEMBER_REMOVE` live updates | ❌ (TODO) |
| Reactions display | ❌ (TODO) |
| MFA / TOTP login | ❌ (TODO) |
| Voice/video | ❌ (out of scope for libpurple) |

---

## Build

### Dependencies

```
# Debian/Ubuntu
sudo apt install libpurple-dev libjson-glib-dev libglib2.0-dev

# Fedora
sudo dnf install libpurple-devel json-glib-devel glib2-devel

# Arch
sudo pacman -S libpurple json-glib glib2
```

### Compile and install

```bash
git clone https://github.com/beadon/purple-fluxer.git
cd purple-fluxer
make

# Install plugin for current user only (no sudo needed):
make install-user

# Or system-wide:
sudo make install
```

### Protocol icon (optional — silences log warnings)

Pidgin 2.x looks for protocol icons in the system pixmaps directory only;
there is no user-local path. A placeholder icon can be installed with:

```bash
make install-icons   # generates a PNG and installs it with sudo
```

---

## Setup in Pidgin

1. Restart Pidgin after installing the plugin.
2. **Accounts → Manage Accounts → Add**
3. Protocol: **Fluxer**
4. Username: your Fluxer **email address**
5. Password: your Fluxer password

### Token login (recommended for persistent sessions)

1. Open the **Advanced** tab when adding/editing the account.
2. Paste your token into the **Token** field.
3. Leave Username/Password blank (or fill them — token takes priority).

### Self-hosted instance

In the **Advanced** tab, change **API base URL** to your instance, e.g.:

```
https://api.myfluxer.example.com/v1
```

The gateway connects directly to `gateway.fluxer.app` (or the equivalent on
your self-hosted server).

---

## Usage notes

### Channel history

When you open a guild channel, the last 50 messages are loaded automatically.
To page further back, type `/more` in the channel. Each invocation loads the
next 50 messages before the current oldest. Because libpurple has no
insert-at-position API, history pages are appended below live messages with
a labelled date-range separator.

### Channels in the buddy list

Channels are grouped as **"Guild / Category"** in the Pidgin buddy list.
This is a workaround for libpurple 2.x supporting only one level of nesting
(the proper fix requires upstream Pidgin changes).

---

## Known limitations / upstream Pidgin work items

These require changes to libpurple/Pidgin itself — tracked as future
contribution targets:

1. **Nested buddy list groups** — libpurple only supports one level of
   grouping; guild → category → channel needs two.
2. **Scroll-to-top history trigger** — no plugin hook for "user scrolled to
   top"; workaround is the `/more` command.
3. **Insert message at timestamp** — `serv_got_chat_in` always appends to
   the bottom; history pages appear below live messages rather than above.
4. **Protocol UI hints** — no API for a plugin to suggest default window
   behaviours (e.g. hide the participant list by default for large guilds).
5. **Per-guild display names (server nicknames)** — Discord-compatible platforms
   support a per-server nickname distinct from the global username. libpurple
   buddies have a single global alias; `get_cb_alias` has no room/guild context
   parameter, so per-guild overrides cannot be implemented at the plugin level.

---

## Architecture

Fluxer's API is intentionally Discord-compatible. Key surface used:

| Endpoint | Purpose |
|---|---|
| `POST /v1/auth/login` | Email+password → bearer token |
| `POST /v1/users/@me/channels` | Open DM with a user |
| `POST /v1/channels/{id}/messages` | Send a message |
| `POST /v1/channels/{id}/typing` | Send typing indicator |
| `GET /v1/channels/{id}/messages` | Fetch message history |
| `wss://gateway.fluxer.app/?v=1&encoding=json` | Real-time event gateway |

Gateway opcodes match Discord's (HELLO=10, IDENTIFY=2, DISPATCH=0, etc.).
The plugin implements a full RFC 6455 WebSocket frame parser and masker
since libpurple 2.x has no native WebSocket support.

### Connection flow

```
fluxer_login()
  └─ stored token → fluxer_use_token()
  └─ else POST /auth/login → fluxer_got_login_response() → fluxer_use_token()
       └─ fluxer_ws_connect()  [SSL + WebSocket upgrade]
            └─ HELLO (op:10) → send heartbeat + IDENTIFY (op:2)
                 └─ READY dispatch → seed buddy list, request guild members
                      └─ GUILD_MEMBERS_CHUNK → populate chat room participant lists
```

### Key gateway events handled

| Event | Handler |
|---|---|
| `READY` | Seed user/guild/DM maps, start buddy list population |
| `GUILD_CREATE` | Build channel/category hierarchy, request member list |
| `GUILD_MEMBERS_CHUNK` | Populate chat room participant lists |
| `CHANNEL_CREATE` | Register new channel in maps |
| `MESSAGE_CREATE` | Route to DM or guild chat |
| `MESSAGE_UPDATE` | Show edit notice + re-post new content |
| `MESSAGE_DELETE` | Show deletion notice with attribution |
| `TYPING_START` | Stub (needs user_id→name lookup) |
| `PRESENCE_UPDATE` | Map Fluxer status to libpurple status |

---

## Contributing

PRs welcome. The most impactful near-term improvements:

- MFA (TOTP) login flow
- `TYPING_START` → `serv_got_typing()` (needs user_id→username index, already collected)
- Buddy avatar download and caching
- `GUILD_MEMBER_ADD` / `GUILD_MEMBER_REMOVE` events (live member list updates)
- Thread/reply rendering

See the "Known limitations" section above for upstream Pidgin contribution
opportunities.

---

## License

GPL-3.0-only — libpurple is GPL-2.0-or-later, which is compatible with GPLv3.

Fluxer itself is AGPL-3.0.
