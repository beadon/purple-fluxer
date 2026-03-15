# purple-fluxer

A **libpurple / Pidgin** protocol plugin for [Fluxer](https://fluxer.app) — 
a free, open-source, self-hostable Discord-compatible instant messaging platform.

> ⚠️ **Alpha software.** Fluxer itself is mid-refactor and its API docs are
> partially TBD. This plugin targets the stable surface that is documented.
> Expect rough edges.

---

## What works (v0.1)

| Feature | Status |
|---|---|
| Email/password login | ✅ |
| Token-based login (bot or stored session) | ✅ |
| Gateway WebSocket connect + IDENTIFY | ✅ |
| Heartbeat keepalive | ✅ |
| Session RESUME on reconnect | ✅ |
| Receive DMs (`MESSAGE_CREATE`) | ✅ |
| Send DMs | ✅ |
| Receive guild channel messages | ✅ |
| Send guild channel messages | ✅ |
| Presence / status sync (`PRESENCE_UPDATE`) | ✅ |
| Set own presence (online/idle/dnd/invisible) | ✅ |
| Guild channel room list | ✅ |
| Self-hosted instance support | ✅ (set API base in Advanced) |
| Buddy avatars | ❌ (TODO) |
| File attachments | ❌ (TODO) |
| Voice/video | ❌ (out of scope for libpurple) |
| MFA login | ❌ (TODO) |

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

### Compile

```bash
git clone https://github.com/beadon/purple-fluxer.git
cd purple-fluxer
make

# Install for current user only (no sudo needed):
make install-user

# Or system-wide:
sudo make install
```

---

## Setup in Pidgin

1. Restart Pidgin after installing the plugin.
2. **Accounts → Manage Accounts → Add**
3. Protocol: **Fluxer**
4. Username: your Fluxer **email address**
5. Password: your Fluxer password

### Token login (recommended for bots or persistent sessions)

1. Open **Advanced** tab when adding/editing the account.
2. Paste your token into the **Token** field.
3. Leave Username/Password blank (or fill them — token takes priority).

### Self-hosted instance

In the **Advanced** tab, change **API base URL** to your instance, e.g.:
```
https://api.myfluxer.example.com/v1
```

The gateway URL is auto-discovered from `GET /gateway/bot`.

---

## Architecture notes

Fluxer's API is intentionally Discord-compatible. Key surface used here:

| Endpoint | Purpose |
|---|---|
| `POST /v1/auth/login` | Email+password → bearer token |
| `GET /v1/gateway/bot` | Discover WebSocket gateway URL |
| `GET /v1/users/@me` | Fetch own user info |
| `POST /v1/users/@me/channels` | Open DM with a user |
| `POST /v1/channels/{id}/messages` | Send a message |
| `POST /v1/channels/{id}/typing` | Send typing indicator |
| `wss://gateway.fluxer.app/?v=1&encoding=json` | Real-time event gateway |

Gateway opcodes match Discord's (HELLO=10, IDENTIFY=2, DISPATCH=0, etc.).
The plugin implements the full WS frame parser and masking per RFC 6455 since
libpurple 2.x has no native WebSocket support.

---

## Contributing

PRs welcome. The most impactful near-term improvements:

- MFA (TOTP) login flow
- `TYPING_START` → `purple_serv_got_typing()` (needs user_id→username index)  
- Buddy avatar download and caching
- Message edit/delete event handling
- Proper thread/reply rendering

---

## License

GPL-3.0-only — libpurple is GPL-2.0-or-later, which is compatible with GPLv3.

Fluxer itself is AGPL-3.0.
