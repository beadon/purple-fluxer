# purple-fluxer

libpurple 2.14.x protocol plugin for [Fluxer](https://fluxer.app) — a free,
open-source, self-hostable Discord-compatible instant messaging platform.

Fluxer's API is intentionally Discord-compatible. The WebSocket gateway
opcodes are identical, so this plugin is spiritually derived from
purple-discord but written fresh against Fluxer's documented API.

---

## Current state

The scaffolding in `purple-fluxer.c` is architecturally complete but does
**not compile cleanly** against libpurple 2.14.x yet. The logic, data
structures, WebSocket frame parser, heartbeat, identify, and dispatch
handlers are all correct — the errors are purely API binding issues.

---

## Known compile errors to fix

### 1. HTTP layer — wrong API generation

The code uses `PurpleHttpRequest` / `PurpleHttpResponse` / `PurpleHttpConnection`
which are **libpurple 3.x only**. libpurple 2.14.x uses:

```c
#include <util.h>

PurpleUtilFetchUrlData *purple_util_fetch_url_request_data_len_with_account(
    PurpleAccount *account,
    const char *url,
    gboolean full,
    const char *user_agent,
    gboolean http11,
    const char *request,   /* raw HTTP request string, or NULL for GET */
    gsize request_len,
    gboolean include_headers,
    gssize max_len,        /* -1 for unlimited */
    PurpleUtilFetchUrlCallback callback,
    void *user_data);

void purple_util_fetch_url_cancel(PurpleUtilFetchUrlData *url_data);

/* Callback signature: */
typedef void (*PurpleUtilFetchUrlCallback)(PurpleUtilFetchUrlData *url_data,
    const gchar *url, const gchar *webdata, gsize len,
    const gchar *error_message, gpointer user_data);
```

For POST requests, build the full HTTP request string manually and pass it
as the `request` parameter (with Content-Type and Authorization headers
embedded). For GET, pass `NULL` and use the `user_agent` param.

Replace `FluxerData.pending_http` (currently `GSList *`) with
`GSList *` of `PurpleUtilFetchUrlData *` and cancel via
`purple_util_fetch_url_cancel()`.

### 2. Misnamed receive functions

```c
// WRONG (libpurple 3.x names):
purple_serv_got_im(...)
purple_serv_got_chat_in(...)

// CORRECT (libpurple 2.x, in <server.h>):
serv_got_im(...)
serv_got_chat_in(...)
```

### 3. Missing include for PurpleAccountOption

```c
#include <accountopt.h>   // add this — provides PurpleAccountOption
```

### 4. fluxer_list_icon declared after prpl_info

`prpl_info` struct references `fluxer_list_icon` but the function is defined
after the struct. Either forward-declare it or move the definition above
`prpl_info`.

### 5. send_typing signature mismatch

```c
// prpl_info.send_typing expects:
unsigned int (*send_typing)(PurpleConnection *gc, const char *name,
                            PurpleTypingState state);

// Current implementation is:
static void fluxer_send_typing(PurpleConnection *gc, const gchar *name);
// Fix: change return type to unsigned int, add PurpleTypingState state param
```

### 6. PURPLE_INIT_PLUGIN macro issue

The macro invocation is wrong. Correct form:

```c
static PurplePluginInfo info = { ... };  // named 'info', not 'plugin_info'

PURPLE_INIT_PLUGIN(fluxer, plugin_init_func, info)
```

Where `plugin_init_func` is a function `void plugin_init_func(PurplePlugin *plugin)`.
The third argument must be the info struct identifier, not a variable named
`plugin_info` (conflicts with macro internals on some versions).

---

## Architecture reference

### Connection flow

```
fluxer_login()
  └─ if stored token → fluxer_use_token()
  └─ else POST /auth/login → fluxer_got_login_response() → fluxer_use_token()
       └─ GET /gateway/bot → fluxer_got_gateway_url()
            └─ fluxer_ws_connect()  [SSL + WebSocket upgrade]
                 └─ fluxer_ssl_connected_cb()  [sends HTTP 101 upgrade]
                      └─ fluxer_ssl_recv_cb()  [reads frames]
                           └─ ws_process_recv_buf()  [parses WS frames]
                                └─ handles opcodes → fluxer_handle_dispatch()
```

### Key gateway events handled

| Event | Handler |
|---|---|
| `READY` | `handle_ready()` — sets self user_id, session_id, seeds DM channels |
| `MESSAGE_CREATE` | `handle_message_create()` — routes to DM or guild chat |
| `TYPING_START` | `handle_typing_start()` — stub, needs user_id→username index |
| `PRESENCE_UPDATE` | `handle_presence_update()` — maps status to libpurple status |
| `CHANNEL_CREATE` | `handle_channel_create()` — populates channel/guild maps |
| `GUILD_CREATE` | `handle_guild_create()` — populates channel names for guild |

### Data structures in FluxerData

- `channel_to_guild`: `channel_id → guild_id` (NULL value = DM)
- `channel_names`: `channel_id → channel_name`
- `dm_channels`: `user_id → channel_id`
- `chat_id_map`: `channel_id → int` (libpurple chat ID for guild channels)

---

## API reference (Fluxer)

- Docs: https://docs.fluxer.app
- Gateway opcodes: https://docs.fluxer.app/gateway/opcodes
- Gateway events: https://docs.fluxer.app/gateway/events
- Auth resource: https://docs.fluxer.app/resources/auth
- Base URL: `https://api.fluxer.app/v1`
- Gateway: `wss://gateway.fluxer.app/?v=1&encoding=json`

Key REST endpoints used:
- `POST /v1/auth/login` — email+password → token
- `GET /v1/gateway/bot` — discover WebSocket URL
- `POST /v1/users/@me/channels` — open DM channel
- `POST /v1/channels/{id}/messages` — send message
- `POST /v1/channels/{id}/typing` — typing indicator

---

## Prior art / reference implementations

- https://github.com/EionRobb/purple-discord — closest structural reference
- https://github.com/hoehermann/purple-gowhatsapp — good libpurple 2.x patterns
- https://github.com/matrix-org/purple-matrix — another 2.x reference

---

## Build

```bash
sudo apt install libpurple-dev libjson-glib-dev libglib2.0-dev
make
make install-user   # installs to ~/.purple/plugins/
```
