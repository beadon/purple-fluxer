/*
 * purple-fluxer — libpurple/Pidgin protocol plugin for Fluxer
 *
 * Fluxer is a free, open-source IM/VoIP platform (https://fluxer.app).
 * Its API is intentionally Discord-compatible; the WebSocket gateway
 * opcodes are identical, so this plugin borrows heavily from the
 * purple-discord mental model.
 *
 * API docs: https://docs.fluxer.app
 * Source:   https://github.com/beadon/purple-fluxer
 *
 * Build deps: libpurple-dev, libjson-glib-dev, libglib2.0-dev
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define PURPLE_PLUGINS

#include <glib.h>
#include <json-glib/json-glib.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>

/* libpurple */
#include <account.h>
#include <accountopt.h>
#include <connection.h>
#include <conversation.h>
#include <debug.h>
#include <notify.h>
#include <plugin.h>
#include <prpl.h>
#include <request.h>
#include <roomlist.h>
#include <server.h>
#include <sslconn.h>
#include <cmds.h>
#include <util.h>
#include <version.h>

/* ─── Plugin constants ────────────────────────────────────────────────── */

#define FLUXER_PLUGIN_ID      "prpl-fluxer"
#define FLUXER_PLUGIN_VERSION "0.1.0"
#define FLUXER_PRPL_PROTOCOL  "fluxer"

#define FLUXER_API_BASE       "https://web.fluxer.app/api/v1"
#define FLUXER_GATEWAY_HOST   "gateway.fluxer.app"
#define FLUXER_GATEWAY_PORT   443
#define FLUXER_GATEWAY_PATH   "/?v=1&encoding=json"
#define FLUXER_USER_AGENT     "purple-fluxer/" FLUXER_PLUGIN_VERSION " (libpurple)"

/* ─── Gateway opcodes (Discord-compatible) ────────────────────────────── */

#define OP_DISPATCH           0
#define OP_HEARTBEAT          1
#define OP_IDENTIFY           2
#define OP_PRESENCE_UPDATE    3
#define OP_VOICE_STATE_UPDATE 4
#define OP_RESUME             6
#define OP_RECONNECT          7
#define OP_REQUEST_MEMBERS    8
#define OP_INVALID_SESSION    9
#define OP_HELLO              10
#define OP_HEARTBEAT_ACK      11
#define OP_GATEWAY_ERROR      12
#define OP_LAZY_REQUEST       14

/* Gateway intents bitfield */
#define INTENT_GUILDS             (1 << 0)
#define INTENT_GUILD_MEMBERS      (1 << 1)
#define INTENT_GUILD_PRESENCES    (1 << 8)
#define INTENT_GUILD_MESSAGES     (1 << 9)
#define INTENT_GUILD_REACTIONS    (1 << 10)
#define INTENT_DIRECT_MESSAGES    (1 << 12)
#define INTENT_DM_REACTIONS       (1 << 13)
#define INTENT_MESSAGE_CONTENT    (1 << 15)

#define FLUXER_INTENTS ( \
    INTENT_GUILDS | INTENT_GUILD_MEMBERS | \
    INTENT_GUILD_MESSAGES | INTENT_DIRECT_MESSAGES | \
    INTENT_MESSAGE_CONTENT | INTENT_GUILD_PRESENCES )

/* ─── WebSocket frame constants ───────────────────────────────────────── */

#define WS_OP_CONTINUATION  0x0
#define WS_OP_TEXT          0x1
#define WS_OP_BINARY        0x2
#define WS_OP_CLOSE         0x8
#define WS_OP_PING          0x9
#define WS_OP_PONG          0xA

/* ─── Per-connection state ────────────────────────────────────────────── */

typedef struct {
    PurpleAccount    *account;
    PurpleConnection *gc;

    /* Auth */
    gchar *token;           /* bearer token (user or bot) */
    gchar *self_user_id;
    gchar *self_username;   /* display name shown in buddy list header */

    /* Gateway WebSocket */
    PurpleSslConnection *ssl;
    gchar   *ws_host;
    guint16  ws_port;
    gboolean ws_handshake_done;
    GString *ws_recv_buf;   /* raw bytes accumulator */
    GString *ws_frame_buf;  /* assembled payload for current frame */
    gboolean ws_in_frame;
    guint8   ws_frame_opcode;
    guint64  ws_frame_remaining;

    /* Heartbeat */
    guint    hb_interval_ms;
    guint    hb_timer;
    gboolean hb_ack_received;
    gint     sequence;      /* last s value from dispatch, -1 = none */

    /* Session (for RESUME) */
    gchar *session_id;
    gchar *resume_gateway_url;

    /* Channel/guild bookkeeping */
    GHashTable *channel_to_guild;  /* channel_id (str) -> guild_id (str), NULL for DMs */
    GHashTable *channel_names;     /* channel_id (str) -> name (str) */
    GHashTable *guild_names;       /* guild_id (str)   -> name (str) */
    GHashTable *dm_channels;       /* user_id (str)    -> channel_id (str) */
    GHashTable *chat_id_map;       /* channel_id (str) -> int chat_id */
    GHashTable *seeded_channels;   /* channel_id (str) -> 1: session dedup for blist */
    GHashTable *user_names;        /* user_id (str)    -> username (str) */
    GHashTable *oldest_msg_id;     /* channel_id (str) -> oldest message snowflake (str) */
    GHashTable *guild_members;     /* guild_id (str)   -> GList* of gchar* usernames */
    gint        next_chat_id;

    /* HTTP pending requests (PurpleHttpConnection*) */
    GSList *pending_http;

    /* Login request state (used when email+password flow is in flight) */
    gboolean login_in_progress;

    /* Configurable API base (from account Advanced settings) */
    gchar *api_base;
} FluxerData;

/* ─── Forward declarations ────────────────────────────────────────────── */

static void fluxer_ws_connect(FluxerData *fd);
static void fluxer_ws_send_json(FluxerData *fd, JsonObject *obj);
static void fluxer_ws_send_heartbeat_payload(FluxerData *fd);
static void fluxer_gateway_send_identify(FluxerData *fd);
static void fluxer_gateway_send_heartbeat(FluxerData *fd);
static void fluxer_handle_dispatch(FluxerData *fd, const gchar *event_name, JsonObject *d);
static const gchar *fluxer_list_icon(PurpleAccount *account, PurpleBuddy *buddy);
static void handle_guild_create(FluxerData *fd, JsonObject *d);
static void fluxer_request_guild_members(FluxerData *fd, const gchar *guild_id);

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static void
free_string_list(gpointer data)
{
    g_list_free_full((GList *)data, g_free);
}

static FluxerData *
fluxer_data_new(PurpleConnection *gc)
{
    FluxerData *fd = g_new0(FluxerData, 1);
    fd->gc              = gc;
    fd->account         = purple_connection_get_account(gc);
    fd->sequence        = -1;
    fd->ws_recv_buf     = g_string_new(NULL);
    fd->ws_frame_buf    = g_string_new(NULL);
    fd->channel_to_guild = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->channel_names    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->guild_names      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->dm_channels      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->chat_id_map      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->seeded_channels  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    fd->user_names       = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->oldest_msg_id    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    fd->guild_members    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                               free_string_list);
    fd->next_chat_id     = 1;
    return fd;
}

static void
fluxer_data_free(FluxerData *fd)
{
    if (!fd) return;

    if (fd->hb_timer)
        purple_timeout_remove(fd->hb_timer);

    if (fd->ssl)
        purple_ssl_close(fd->ssl);

    /* Cancel any pending HTTP */
    g_slist_foreach(fd->pending_http,
                    (GFunc)purple_util_fetch_url_cancel, NULL);
    g_slist_free(fd->pending_http);

    g_string_free(fd->ws_recv_buf, TRUE);
    g_string_free(fd->ws_frame_buf, TRUE);

    g_hash_table_destroy(fd->channel_to_guild);
    g_hash_table_destroy(fd->channel_names);
    g_hash_table_destroy(fd->guild_names);
    g_hash_table_destroy(fd->dm_channels);
    g_hash_table_destroy(fd->chat_id_map);
    g_hash_table_destroy(fd->seeded_channels);
    g_hash_table_destroy(fd->user_names);
    g_hash_table_destroy(fd->oldest_msg_id);
    g_hash_table_destroy(fd->guild_members);

    g_free(fd->api_base);
    g_free(fd->token);
    g_free(fd->self_user_id);
    g_free(fd->self_username);
    g_free(fd->ws_host);
    g_free(fd->session_id);
    g_free(fd->resume_gateway_url);

    g_free(fd);
}

/* Build an Authorization header value.
 * User tokens go raw; bot tokens need "Bot " prefix.
 * Since this targets regular user accounts via Pidgin, we send the raw token.
 * Switch to "Bot " prefix if you're connecting a bot account. */
static const gchar *
fluxer_auth_header(FluxerData *fd)
{
    return fd->token;   /* raw token — matches Fluxer user session tokens */
}

/* ─── JSON helpers ────────────────────────────────────────────────────── */

static gchar *
json_object_to_string(JsonObject *obj)
{
    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, obj);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *str = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(root);
    return str;
}

static JsonObject *
string_to_json_object(const gchar *str)
{
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, str, -1, &err)) {
        purple_debug_error("fluxer", "JSON parse error: %s\n", err->message);
        g_error_free(err);
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    /* Increase ref so it survives parser destruction */
    json_object_ref(obj);
    g_object_unref(parser);
    return obj;
}

/* ─── WebSocket framing ───────────────────────────────────────────────── */

/*
 * Build a client→server WebSocket frame.
 * Client frames MUST be masked per RFC 6455.
 * Returns a newly allocated buffer; caller frees.
 */
static guint8 *
ws_build_frame(const guint8 *payload, gsize payload_len,
               guint8 opcode, gsize *out_len)
{
    gsize header_len = 2;
    if      (payload_len > 65535) header_len += 8;
    else if (payload_len > 125)   header_len += 2;

    header_len += 4; /* masking key */

    *out_len = header_len + payload_len;
    guint8 *frame = g_malloc(*out_len);

    frame[0] = 0x80 | (opcode & 0x0F);  /* FIN=1, opcode */
    frame[1] = 0x80;                      /* MASK=1 */

    gsize offset = 2;
    if (payload_len > 65535) {
        frame[1] |= 127;
        guint64 plen = GUINT64_TO_BE((guint64)payload_len);
        memcpy(frame + offset, &plen, 8);
        offset += 8;
    } else if (payload_len > 125) {
        frame[1] |= 126;
        guint16 plen = GUINT16_TO_BE((guint16)payload_len);
        memcpy(frame + offset, &plen, 2);
        offset += 2;
    } else {
        frame[1] |= (guint8)payload_len;
    }

    /* Random masking key */
    guint8 mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (guint8)(g_random_int() & 0xFF);
    memcpy(frame + offset, mask, 4);
    offset += 4;

    for (gsize i = 0; i < payload_len; i++)
        frame[offset + i] = payload[i] ^ mask[i % 4];

    return frame;
}

static void
ws_send_text(FluxerData *fd, const gchar *text)
{
    gsize frame_len;
    guint8 *frame = ws_build_frame((const guint8 *)text, strlen(text),
                                   WS_OP_TEXT, &frame_len);
    purple_ssl_write(fd->ssl, frame, frame_len);
    g_free(frame);
}

/* Parse incoming WS frames from fd->ws_recv_buf.
 * May produce zero or more complete messages. */
static void
ws_process_recv_buf(FluxerData *fd);

static void
fluxer_ssl_recv_cb(gpointer data, PurpleSslConnection *ssl,
                   PurpleInputCondition cond)
{
    FluxerData *fd = data;
    guint8 buf[4096];
    int len;

    while ((len = purple_ssl_read(ssl, buf, sizeof(buf))) > 0) {
        g_string_append_len(fd->ws_recv_buf, (gchar *)buf, len);
    }

    if (len < 0 && errno != EAGAIN) {
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
            "SSL read error");
        return;
    }

    if (!fd->ws_handshake_done) {
        /* Look for end of HTTP 101 response */
        const gchar *p = strstr(fd->ws_recv_buf->str, "\r\n\r\n");
        if (!p) return;

        /* Check for 101 Switching Protocols */
        if (!strstr(fd->ws_recv_buf->str, "101")) {
            purple_debug_error("fluxer",
                "WebSocket upgrade failed:\n%s\n",
                fd->ws_recv_buf->str);
            purple_connection_error_reason(fd->gc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "WebSocket handshake failed");
            return;
        }

        /* Strip HTTP headers from buffer */
        gsize consumed = (p - fd->ws_recv_buf->str) + 4;
        g_string_erase(fd->ws_recv_buf, 0, consumed);
        fd->ws_handshake_done = TRUE;
        purple_debug_info("fluxer", "WebSocket handshake complete\n");
    }

    ws_process_recv_buf(fd);
}

/*
 * Process accumulated bytes in fd->ws_recv_buf.
 * Reassembles fragmented frames, unmasks server→client payloads
 * (servers MUST NOT mask, but we handle it defensively).
 */
static void
ws_process_recv_buf(FluxerData *fd)
{
    GString *buf = fd->ws_recv_buf;

    while (buf->len >= 2) {
        guint8  byte0   = (guint8)buf->str[0];
        guint8  byte1   = (guint8)buf->str[1];
        gboolean fin    = (byte0 & 0x80) != 0;
        guint8   opcode = byte0 & 0x0F;
        gboolean masked = (byte1 & 0x80) != 0;
        guint64  payload_len = byte1 & 0x7F;

        gsize header_size = 2;
        if      (payload_len == 126) header_size += 2;
        else if (payload_len == 127) header_size += 8;
        if (masked) header_size += 4;

        if (buf->len < header_size) break;  /* need more data */

        gsize offset = 2;
        if (payload_len == 126) {
            guint16 plen;
            memcpy(&plen, buf->str + 2, 2);
            payload_len = GUINT16_FROM_BE(plen);
            offset += 2;
        } else if (payload_len == 127) {
            guint64 plen;
            memcpy(&plen, buf->str + 2, 8);
            payload_len = GUINT64_FROM_BE(plen);
            offset += 8;
        }

        guint8 mask_key[4] = {0};
        if (masked) {
            memcpy(mask_key, buf->str + offset, 4);
            offset += 4;
        }

        if (buf->len < header_size + payload_len) break; /* need more data */

        /* Unmask if needed (server→client frames are normally unmasked) */
        guint8 *payload = (guint8 *)(buf->str + offset);
        if (masked) {
            for (guint64 i = 0; i < payload_len; i++)
                payload[i] ^= mask_key[i % 4];
        }

        /* Handle control frames inline */
        if (opcode == WS_OP_CLOSE) {
            purple_debug_info("fluxer", "WebSocket CLOSE received\n");
            /* Attempt reconnect */
            purple_connection_error_reason(fd->gc,
                PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                "Gateway closed connection");
            g_string_erase(buf, 0, header_size + payload_len);
            return;
        }

        if (opcode == WS_OP_PING) {
            guint8 *pong = ws_build_frame(payload, payload_len,
                                          WS_OP_PONG, &header_size);
            purple_ssl_write(fd->ssl, pong, header_size);
            g_free(pong);
            g_string_erase(buf, 0, 2 + (offset - 2) + payload_len);
            continue;
        }

        /* Accumulate data frame payload */
        if (opcode != WS_OP_CONTINUATION) {
            g_string_truncate(fd->ws_frame_buf, 0);
            fd->ws_frame_opcode = opcode;
        }
        g_string_append_len(fd->ws_frame_buf,
                            (gchar *)payload, payload_len);

        /* Remove consumed bytes */
        g_string_erase(buf, 0, header_size + payload_len);

        if (!fin) continue;  /* wait for more fragments */

        /* Complete message assembled */
        if (fd->ws_frame_opcode == WS_OP_TEXT) {
            gchar *msg = fd->ws_frame_buf->str;
            purple_debug_misc("fluxer", "GW recv: %s\n", msg);

            JsonObject *root = string_to_json_object(msg);
            if (!root) continue;

            gint op = (gint)json_object_get_int_member(root, "op");
            gint s  = json_object_has_member(root, "s") &&
                      !json_node_is_null(json_object_get_member(root, "s"))
                      ? (gint)json_object_get_int_member(root, "s")
                      : -1;
            if (s > 0) fd->sequence = s;

            switch (op) {
            case OP_HELLO: {
                JsonObject *d = json_object_get_object_member(root, "d");
                fd->hb_interval_ms = (guint)
                    json_object_get_int_member(d, "heartbeat_interval");
                purple_debug_info("fluxer",
                    "HELLO: heartbeat every %ums\n", fd->hb_interval_ms);
                /* Jitter: send first heartbeat between 0 and interval */
                fluxer_gateway_send_heartbeat(fd);
                fluxer_gateway_send_identify(fd);
                break;
            }
            case OP_HEARTBEAT_ACK:
                fd->hb_ack_received = TRUE;
                purple_debug_misc("fluxer", "Heartbeat ACK\n");
                break;

            case OP_HEARTBEAT:
                /* Server requesting an immediate heartbeat (pure pong).
                 * Respond directly without touching hb_ack_received — the
                 * regular timer cycle manages ACK state independently. */
                fluxer_ws_send_heartbeat_payload(fd);
                break;

            case OP_RECONNECT:
                purple_debug_info("fluxer", "Gateway requested reconnect\n");
                purple_connection_error_reason(fd->gc,
                    PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                    "Gateway reconnect requested");
                break;

            case OP_INVALID_SESSION: {
                gboolean resumable = json_object_get_boolean_member(root, "d");
                if (!resumable) {
                    g_free(fd->session_id);
                    fd->session_id = NULL;
                }
                purple_debug_warning("fluxer",
                    "Invalid session (resumable=%d)\n", resumable);
                /* Re-identify after brief pause */
                g_usleep(1000000);
                fluxer_gateway_send_identify(fd);
                break;
            }

            case OP_DISPATCH: {
                const gchar *t = json_object_get_string_member(root, "t");
                JsonObject  *d = json_object_get_object_member(root, "d");
                fluxer_handle_dispatch(fd, t, d);
                break;
            }

            case OP_GATEWAY_ERROR:
                purple_debug_error("fluxer", "Gateway error from server\n");
                break;

            default:
                purple_debug_warning("fluxer",
                    "Unknown opcode %d\n", op);
                break;
            }

            json_object_unref(root);
        }
    }
}

/* ─── Gateway outbound payloads ───────────────────────────────────────── */

static void
fluxer_ws_send_json(FluxerData *fd, JsonObject *obj)
{
    gchar *str = json_object_to_string(obj);
    purple_debug_misc("fluxer", "GW send: %s\n", str);
    ws_send_text(fd, str);
    g_free(str);
}

static gboolean
fluxer_heartbeat_cb(gpointer data)
{
    FluxerData *fd = data;

    if (!fd->hb_ack_received) {
        purple_debug_warning("fluxer",
            "No heartbeat ACK — zombie connection?\n");
        /* Force reconnect */
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
            "Heartbeat timeout");
        fd->hb_timer = 0;
        return FALSE;
    }

    fluxer_gateway_send_heartbeat(fd);
    return TRUE;  /* keep firing */
}

/* Send a raw heartbeat payload without touching hb_ack_received.
 * Used both by the regular timer cycle and to respond to server op:1 requests. */
static void
fluxer_ws_send_heartbeat_payload(FluxerData *fd)
{
    JsonObject *payload = json_object_new();
    json_object_set_int_member(payload, "op", OP_HEARTBEAT);
    if (fd->sequence >= 0)
        json_object_set_int_member(payload, "d", fd->sequence);
    else
        json_object_set_null_member(payload, "d");

    fluxer_ws_send_json(fd, payload);
    json_object_unref(payload);
}

static void
fluxer_gateway_send_heartbeat(FluxerData *fd)
{
    fd->hb_ack_received = FALSE;
    fluxer_ws_send_heartbeat_payload(fd);

    /* Schedule the repeating heartbeat timer after first send */
    if (!fd->hb_timer && fd->hb_interval_ms > 0) {
        fd->hb_timer = purple_timeout_add(fd->hb_interval_ms,
                                          fluxer_heartbeat_cb, fd);
    }
}

static void
fluxer_gateway_send_identify(FluxerData *fd)
{
    /* IDENTIFY payload */
    JsonObject *props = json_object_new();
    json_object_set_string_member(props, "os",      "Linux");
    json_object_set_string_member(props, "browser", "purple-fluxer");
    json_object_set_string_member(props, "device",  "purple-fluxer");

    JsonObject *d = json_object_new();
    json_object_set_string_member(d, "token", fd->token);
    json_object_set_int_member   (d, "intents", FLUXER_INTENTS);
    json_object_set_object_member(d, "properties", props);

    /* Presence: online */
    JsonObject *presence = json_object_new();
    json_object_set_string_member(presence, "status", "online");
    json_object_set_boolean_member(presence, "afk", FALSE);
    json_object_set_null_member(presence, "since");
    json_object_set_array_member(presence, "activities", json_array_new());
    json_object_set_object_member(d, "presence", presence);

    JsonObject *payload = json_object_new();
    json_object_set_int_member   (payload, "op", OP_IDENTIFY);
    json_object_set_object_member(payload, "d",  d);

    fluxer_ws_send_json(fd, payload);
    json_object_unref(payload);

    purple_debug_info("fluxer", "Sent IDENTIFY\n");
}

static void
fluxer_gateway_send_resume(FluxerData *fd)
{
    JsonObject *d = json_object_new();
    json_object_set_string_member(d, "token",      fd->token);
    json_object_set_string_member(d, "session_id", fd->session_id);
    json_object_set_int_member   (d, "seq",        fd->sequence);

    JsonObject *payload = json_object_new();
    json_object_set_int_member   (payload, "op", OP_RESUME);
    json_object_set_object_member(payload, "d",  d);

    fluxer_ws_send_json(fd, payload);
    json_object_unref(payload);

    purple_debug_info("fluxer", "Sent RESUME (seq=%d)\n", fd->sequence);
}

/* ─── Dispatch event handlers ─────────────────────────────────────────── */

static void
handle_ready(FluxerData *fd, JsonObject *d)
{
    JsonObject *user = json_object_get_object_member(d, "user");
    const gchar *id  = json_object_get_string_member(user, "id");
    const gchar *uname = json_object_get_string_member(user, "username");

    g_free(fd->self_user_id);
    g_free(fd->self_username);
    fd->self_user_id  = g_strdup(id);
    fd->self_username = g_strdup(uname);

    g_free(fd->session_id);
    fd->session_id = g_strdup(
        json_object_get_string_member(d, "session_id"));

    if (json_object_has_member(d, "resume_gateway_url")) {
        g_free(fd->resume_gateway_url);
        fd->resume_gateway_url = g_strdup(
            json_object_get_string_member(d, "resume_gateway_url"));
    }

    purple_debug_info("fluxer",
        "READY: logged in as %s (%s), session=%s\n",
        uname, id, fd->session_id);

    /* Seed user_id → username map from the users array in READY */
    if (json_object_has_member(d, "users")) {
        JsonArray *users = json_object_get_array_member(d, "users");
        for (guint i = 0; i < json_array_get_length(users); i++) {
            JsonObject *u   = json_array_get_object_element(users, i);
            const gchar *uid  = json_object_get_string_member(u, "id");
            const gchar *un   = json_object_get_string_member(u, "username");
            if (uid && un)
                g_hash_table_insert(fd->user_names, g_strdup(uid), g_strdup(un));
        }
    }

    /* Walk DM channels sent in READY */
    if (json_object_has_member(d, "private_channels")) {
        JsonArray *dms = json_object_get_array_member(d, "private_channels");
        guint n = json_array_get_length(dms);
        for (guint i = 0; i < n; i++) {
            JsonObject *ch = json_array_get_object_element(dms, i);
            const gchar *ch_id = json_object_get_string_member(ch, "id");

            /* For 1-on-1 DMs the recipients array has one entry */
            if (json_object_has_member(ch, "recipients")) {
                JsonArray *recs = json_object_get_array_member(ch, "recipients");
                if (json_array_get_length(recs) > 0) {
                    JsonObject *r = json_array_get_object_element(recs, 0);
                    const gchar *uid = json_object_get_string_member(r, "id");
                    const gchar *un  = json_object_get_string_member(r, "username");
                    /* Map user_id -> channel_id */
                    g_hash_table_insert(fd->dm_channels,
                                        g_strdup(uid), g_strdup(ch_id));
                    /* Add to buddy list */
                    PurpleBuddy *buddy =
                        purple_find_buddy(fd->account, un);
                    if (!buddy) {
                        buddy = purple_buddy_new(fd->account, un, un);
                        purple_blist_add_buddy(buddy, NULL, NULL, NULL);
                    }
                    purple_prpl_got_user_status(fd->account, un,
                        "online", NULL);
                }
            }
        }
    }

    /* Walk guilds included in READY — seeds buddy list groups */
    if (json_object_has_member(d, "guilds")) {
        JsonArray *guilds = json_object_get_array_member(d, "guilds");
        guint ng = json_array_get_length(guilds);
        for (guint i = 0; i < ng; i++) {
            JsonObject *guild = json_array_get_object_element(guilds, i);
            if (json_object_has_member(guild, "channels"))
                handle_guild_create(fd, guild);
        }
        purple_debug_info("fluxer", "READY: processed %u guilds\n", ng);
    }

    purple_connection_set_state(fd->gc, PURPLE_CONNECTED);
    purple_connection_update_progress(fd->gc, "Connected", 3, 3);
}

static void
handle_message_create(FluxerData *fd, JsonObject *d)
{
    const gchar *channel_id = json_object_get_string_member(d, "channel_id");
    const gchar *content    = json_object_get_string_member(d, "content");

    if (!content || *content == '\0') return;  /* attachment-only, skip for now */

    JsonObject *author  = json_object_get_object_member(d, "author");
    const gchar *author_id = json_object_get_string_member(author, "id");
    const gchar *username  = json_object_get_string_member(author, "username");

    /* Cache user_id → username as we see messages */
    g_hash_table_insert(fd->user_names, g_strdup(author_id), g_strdup(username));

    /* Ignore our own messages */
    if (g_strcmp0(author_id, fd->self_user_id) == 0) return;

    gchar *guild_id = g_hash_table_lookup(fd->channel_to_guild, channel_id);
    time_t ts = time(NULL);

    if (guild_id == NULL) {
        /* DM — deliver as IM */
        serv_got_im(fd->gc, username, content,
                    PURPLE_MESSAGE_RECV, ts);
    } else {
        /* Guild channel — deliver to chat */
        gpointer chat_id_ptr =
            g_hash_table_lookup(fd->chat_id_map, channel_id);
        if (chat_id_ptr) {
            gint chat_id = GPOINTER_TO_INT(chat_id_ptr);
            serv_got_chat_in(fd->gc, chat_id, username,
                             PURPLE_MESSAGE_RECV, content, ts);
        }
    }
}

/* Extract HH:MM:SS from an ISO 8601 string like "2026-03-15T08:23:14.425Z".
 * Returns a pointer into a static buffer — copy before calling again. */
static const gchar *
iso_time(const gchar *iso)
{
    static gchar buf[9];
    if (!iso) return "?";
    const gchar *t = strchr(iso, 'T');
    if (!t) return "?";
    g_strlcpy(buf, t + 1, sizeof(buf));   /* copies up to 8 chars + NUL */
    return buf;
}

/* Write a system notice into whichever conversation owns channel_id.
 * Used for delete/edit notifications. */
static void
fluxer_conv_notice(FluxerData *fd, const gchar *channel_id, const gchar *text)
{
    time_t ts = time(NULL);
    gchar *guild_id = g_hash_table_lookup(fd->channel_to_guild, channel_id);

    if (guild_id == NULL) {
        /* DM — find conversation by the buddy's name if open */
        /* We don't have an easy reverse-lookup here; skip for now */
    } else {
        gpointer chat_id_ptr = g_hash_table_lookup(fd->chat_id_map, channel_id);
        if (!chat_id_ptr) return;
        PurpleConversation *conv =
            purple_find_chat(fd->gc, GPOINTER_TO_INT(chat_id_ptr));
        if (conv)
            purple_conversation_write(conv, NULL, text,
                                      PURPLE_MESSAGE_SYSTEM, ts);
    }
}

static void
handle_message_delete(FluxerData *fd, JsonObject *d)
{
    const gchar *channel_id = json_object_get_string_member(d, "channel_id");
    const gchar *content    = json_object_has_member(d, "content")
                            ? json_object_get_string_member(d, "content") : NULL;

    if (!content || *content == '\0') return;

    const gchar *author_id = json_object_has_member(d, "author_id")
                           ? json_object_get_string_member(d, "author_id") : NULL;
    const gchar *username  = author_id
                           ? g_hash_table_lookup(fd->user_names, author_id) : NULL;
    if (!username) username = author_id ? author_id : "unknown";

    gchar *notice = g_strdup_printf("* %s deleted: %s", username, content);
    fluxer_conv_notice(fd, channel_id, notice);
    g_free(notice);
}

static void
handle_message_update(FluxerData *fd, JsonObject *d)
{
    const gchar *channel_id = json_object_get_string_member(d, "channel_id");
    const gchar *content    = json_object_has_member(d, "content")
                            ? json_object_get_string_member(d, "content") : NULL;

    if (!content || *content == '\0') return;

    const gchar *author_id = NULL;
    const gchar *username  = NULL;
    if (json_object_has_member(d, "author")) {
        JsonObject *author = json_object_get_object_member(d, "author");
        author_id = json_object_get_string_member(author, "id");
        username  = json_object_get_string_member(author, "username");
        if (username)
            g_hash_table_insert(fd->user_names, g_strdup(author_id), g_strdup(username));
    }

    /* Timestamps: original send time and edit time */
    const gchar *orig_ts = json_object_has_member(d, "timestamp")
                         ? json_object_get_string_member(d, "timestamp") : NULL;
    const gchar *edit_ts = json_object_has_member(d, "edited_timestamp") &&
                           !json_node_is_null(json_object_get_member(d, "edited_timestamp"))
                         ? json_object_get_string_member(d, "edited_timestamp") : NULL;

    /* System notice: who edited and when */
    gchar *orig_str = g_strdup(iso_time(orig_ts));
    gchar *edit_str = g_strdup(iso_time(edit_ts));
    gchar *notice   = g_strdup_printf("* %s edited [%s → %s]",
                                      username ? username : "unknown",
                                      orig_str, edit_str);
    fluxer_conv_notice(fd, channel_id, notice);
    g_free(orig_str);
    g_free(edit_str);
    g_free(notice);

    /* Re-post the new content inline */
    if (!username) return;
    gchar *guild_id = g_hash_table_lookup(fd->channel_to_guild, channel_id);
    gchar *edited   = g_strdup_printf("[edited] %s", content);
    time_t ts       = time(NULL);

    if (guild_id == NULL) {
        serv_got_im(fd->gc, username, edited, PURPLE_MESSAGE_RECV, ts);
    } else {
        gpointer chat_id_ptr = g_hash_table_lookup(fd->chat_id_map, channel_id);
        if (chat_id_ptr)
            serv_got_chat_in(fd->gc, GPOINTER_TO_INT(chat_id_ptr), username,
                             PURPLE_MESSAGE_RECV, edited, ts);
    }
    g_free(edited);
}

static void
handle_typing_start(FluxerData *fd, JsonObject *d)
{
    const gchar *channel_id = json_object_get_string_member(d, "channel_id");
    const gchar *user_id    = json_object_get_string_member(d, "user_id");
    (void)channel_id; (void)user_id;
    /* TODO: call purple_serv_got_typing() once we resolve user_id→username */
}

static void
handle_presence_update(FluxerData *fd, JsonObject *d)
{
    JsonObject *user   = json_object_get_object_member(d, "user");
    const gchar *uname = json_object_get_string_member(user, "username");
    const gchar *status = json_object_get_string_member(d, "status");

    if (!uname || !status) return;

    const gchar *purple_status = "offline";
    if      (g_strcmp0(status, "online")    == 0) purple_status = "online";
    else if (g_strcmp0(status, "idle")      == 0) purple_status = "away";
    else if (g_strcmp0(status, "dnd")       == 0) purple_status = "unavailable";
    else if (g_strcmp0(status, "invisible") == 0) purple_status = "offline";

    purple_prpl_got_user_status(fd->account, uname, purple_status, NULL);
}

static void
handle_channel_create(FluxerData *fd, JsonObject *d)
{
    const gchar *ch_id   = json_object_get_string_member(d, "id");
    const gchar *ch_name = json_object_has_member(d, "name")
                         ? json_object_get_string_member(d, "name") : NULL;
    gint   ch_type = (gint)json_object_get_int_member(d, "type");
    const gchar *guild_id = json_object_has_member(d, "guild_id") &&
                            !json_node_is_null(
                                json_object_get_member(d, "guild_id"))
                          ? json_object_get_string_member(d, "guild_id")
                          : NULL;

    if (ch_type == 0 || ch_type == 5) {
        /* Text / announcement channel in a guild */
        g_hash_table_insert(fd->channel_to_guild,
                            g_strdup(ch_id),
                            g_strdup(guild_id ? guild_id : ""));
        if (ch_name)
            g_hash_table_insert(fd->channel_names,
                                g_strdup(ch_id), g_strdup(ch_name));
    } else if (ch_type == 1) {
        /* DM channel — map recipient → channel_id */
        if (json_object_has_member(d, "recipients")) {
            JsonArray *recs = json_object_get_array_member(d, "recipients");
            for (guint i = 0; i < json_array_get_length(recs); i++) {
                JsonObject *r = json_array_get_object_element(recs, i);
                const gchar *uid = json_object_get_string_member(r, "id");
                if (g_strcmp0(uid, fd->self_user_id) != 0) {
                    g_hash_table_insert(fd->dm_channels,
                                        g_strdup(uid), g_strdup(ch_id));
                }
            }
        }
        /* NULL guild_id marks it as a DM */
        g_hash_table_insert(fd->channel_to_guild,
                            g_strdup(ch_id), NULL);
    }
}

/* Walk the buddy list and check if a chat with this channel_id already exists
 * for our account.  purple_blist_find_chat() is unreliable during the
 * connecting phase, so we inspect components directly. */
static gboolean
fluxer_chat_exists(PurpleAccount *account, const gchar *channel_id)
{
    PurpleBlistNode *gnode, *node;
    for (gnode = purple_blist_get_root(); gnode; gnode = gnode->next) {
        if (!PURPLE_BLIST_NODE_IS_GROUP(gnode)) continue;
        for (node = gnode->child; node; node = node->next) {
            if (!PURPLE_BLIST_NODE_IS_CHAT(node)) continue;
            PurpleChat *chat = (PurpleChat *)node;
            if (purple_chat_get_account(chat) != account) continue;
            const gchar *stored =
                g_hash_table_lookup(purple_chat_get_components(chat), "channel_id");
            if (g_strcmp0(stored, channel_id) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/* Add a text channel to the Pidgin buddy list under a group named after
 * the guild.  Skips channels that are already present. */
static void
fluxer_blist_seed_channel(FluxerData *fd, const gchar *guild_name,
                          const gchar *channel_id, const gchar *channel_name)
{
    /* Session-level guard: GUILD_CREATE fires after READY for the same guild */
    if (g_hash_table_contains(fd->seeded_channels, channel_id))
        return;
    g_hash_table_insert(fd->seeded_channels, g_strdup(channel_id), GINT_TO_POINTER(1));

    /* Cross-session guard: channel may already exist in saved buddy list */
    if (fluxer_chat_exists(fd->account, channel_id))
        return;

    PurpleGroup *group = purple_find_group(guild_name);
    if (!group) {
        group = purple_group_new(guild_name);
        purple_blist_add_group(group, NULL);
    }

    GHashTable *components =
        g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(components, "channel_id", g_strdup(channel_id));

    PurpleChat *chat = purple_chat_new(fd->account, channel_name, components);
    purple_blist_add_chat(chat, group, NULL);
}

static void
handle_guild_create(FluxerData *fd, JsonObject *d)
{
    if (!json_object_has_member(d, "channels")) return;

    const gchar *guild_id = json_object_get_string_member(d, "id");

    /* Guild name: try top-level "name", then "properties"."name" */
    const gchar *guild_name = NULL;
    if (json_object_has_member(d, "name"))
        guild_name = json_object_get_string_member(d, "name");
    if (!guild_name && json_object_has_member(d, "properties")) {
        JsonObject *props = json_object_get_object_member(d, "properties");
        if (props && json_object_has_member(props, "name"))
            guild_name = json_object_get_string_member(props, "name");
    }
    if (!guild_name) guild_name = guild_id;

    g_hash_table_insert(fd->guild_names, g_strdup(guild_id), g_strdup(guild_name));

    JsonArray *channels = json_object_get_array_member(d, "channels");
    guint n = json_array_get_length(channels);

    /* Pass 1: collect category channels (type=4) → id→name */
    GHashTable *categories =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (guint i = 0; i < n; i++) {
        JsonObject *ch = json_array_get_object_element(channels, i);
        gint ch_type = (gint)json_object_get_int_member(ch, "type");
        if (ch_type == 4) {
            const gchar *cat_id   = json_object_get_string_member(ch, "id");
            const gchar *cat_name = json_object_has_member(ch, "name")
                                  ? json_object_get_string_member(ch, "name") : NULL;
            if (cat_id && cat_name)
                g_hash_table_insert(categories, g_strdup(cat_id), g_strdup(cat_name));
        }
    }

    /* Pass 2: seed text/announcement channels under "Guild / Category" groups */
    for (guint i = 0; i < n; i++) {
        JsonObject *ch = json_array_get_object_element(channels, i);
        const gchar *ch_id   = json_object_get_string_member(ch, "id");
        const gchar *ch_name = json_object_has_member(ch, "name")
                             ? json_object_get_string_member(ch, "name") : NULL;
        gint ch_type = (gint)json_object_get_int_member(ch, "type");

        if (ch_type == 0 || ch_type == 5) {  /* text / announcement */
            g_hash_table_insert(fd->channel_to_guild,
                                g_strdup(ch_id), g_strdup(guild_id));
            if (ch_name) {
                g_hash_table_insert(fd->channel_names,
                                    g_strdup(ch_id), g_strdup(ch_name));

                /* Build group name: "Guild / Category" or just "Guild" */
                gchar *group_name;
                const gchar *parent_id = json_object_has_member(ch, "parent_id") &&
                    !json_node_is_null(json_object_get_member(ch, "parent_id"))
                    ? json_object_get_string_member(ch, "parent_id") : NULL;
                const gchar *cat_name = parent_id
                    ? g_hash_table_lookup(categories, parent_id) : NULL;
                if (cat_name)
                    group_name = g_strdup_printf("%s / %s", guild_name, cat_name);
                else
                    group_name = g_strdup(guild_name);

                fluxer_blist_seed_channel(fd, group_name, ch_id, ch_name);
                g_free(group_name);
            }
        }
    }

    g_hash_table_destroy(categories);

    /* Collect guild member usernames for chat room population */
    if (json_object_has_member(d, "members")) {
        JsonArray *members = json_object_get_array_member(d, "members");
        guint nm = json_array_get_length(members);
        GList *member_list = NULL;
        for (guint i = 0; i < nm; i++) {
            JsonObject *m    = json_array_get_object_element(members, i);
            JsonObject *user = json_object_has_member(m, "user")
                             ? json_object_get_object_member(m, "user") : NULL;
            if (!user) continue;
            const gchar *uname = json_object_get_string_member(user, "username");
            const gchar *uid   = json_object_get_string_member(user, "id");
            if (uname && uid) {
                member_list = g_list_prepend(member_list, g_strdup(uname));
                g_hash_table_insert(fd->user_names, g_strdup(uid), g_strdup(uname));
            }
        }
        g_hash_table_insert(fd->guild_members, g_strdup(guild_id), member_list);
        purple_debug_info("fluxer",
            "GUILD_CREATE: guild \"%s\" (%s), %u channels, %u members\n",
            guild_name, guild_id, n, nm);
    } else {
        purple_debug_info("fluxer",
            "GUILD_CREATE: guild \"%s\" (%s), %u channels\n",
            guild_name, guild_id, n);
    }

    /* Request the full member list asynchronously */
    fluxer_request_guild_members(fd, guild_id);
}

static void
fluxer_request_guild_members(FluxerData *fd, const gchar *guild_id)
{
    JsonObject *d = json_object_new();
    json_object_set_string_member(d, "guild_id", guild_id);
    json_object_set_string_member(d, "query", "");
    json_object_set_int_member   (d, "limit", 0);

    JsonObject *payload = json_object_new();
    json_object_set_int_member   (payload, "op", OP_REQUEST_MEMBERS);
    json_object_set_object_member(payload, "d",  d);

    fluxer_ws_send_json(fd, payload);
    json_object_unref(payload);
    purple_debug_info("fluxer", "Sent REQUEST_GUILD_MEMBERS for %s\n", guild_id);
}

static void
handle_guild_members_chunk(FluxerData *fd, JsonObject *d)
{
    const gchar *guild_id = json_object_get_string_member(d, "guild_id");
    if (!guild_id || !json_object_has_member(d, "members")) return;

    gint chunk_index = json_object_has_member(d, "chunk_index")
        ? (gint)json_object_get_int_member(d, "chunk_index") : 0;
    gint chunk_count = json_object_has_member(d, "chunk_count")
        ? (gint)json_object_get_int_member(d, "chunk_count") : 1;

    JsonArray *members_arr = json_object_get_array_member(d, "members");
    guint n = json_array_get_length(members_arr);

    /* Build list of new usernames from this chunk */
    GList *new_names = NULL;
    for (guint i = 0; i < n; i++) {
        JsonObject *m    = json_array_get_object_element(members_arr, i);
        JsonObject *user = json_object_has_member(m, "user")
                         ? json_object_get_object_member(m, "user") : NULL;
        if (!user) continue;
        const gchar *uname = json_object_get_string_member(user, "username");
        const gchar *uid   = json_object_get_string_member(user, "id");
        if (uname && uid) {
            new_names = g_list_prepend(new_names, g_strdup(uname));
            g_hash_table_insert(fd->user_names, g_strdup(uid), g_strdup(uname));
        }
    }

    /* Add chunk members to any open chat windows for this guild */
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, fd->chat_id_map);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        const gchar *ch_id   = k;
        gint         chat_id = GPOINTER_TO_INT(v);
        const gchar *ch_guild = g_hash_table_lookup(fd->channel_to_guild, ch_id);
        if (g_strcmp0(ch_guild, guild_id) != 0) continue;

        PurpleConversation *conv = purple_find_chat(fd->gc, chat_id);
        if (!conv) continue;

        GList *flags = NULL;
        for (GList *l = new_names; l; l = l->next)
            flags = g_list_prepend(flags, GINT_TO_POINTER(PURPLE_CBFLAGS_NONE));
        purple_conv_chat_add_users(PURPLE_CONV_CHAT(conv),
                                   new_names, NULL, flags, FALSE);
        g_list_free(flags);
    }

    /* Merge chunk into guild_members: steal existing list to avoid double-free */
    GList *existing = g_hash_table_lookup(fd->guild_members, guild_id);
    if (existing)
        g_hash_table_steal(fd->guild_members, guild_id);
    GList *combined = g_list_concat(new_names, existing);
    g_hash_table_insert(fd->guild_members, g_strdup(guild_id), combined);

    purple_debug_info("fluxer",
        "GUILD_MEMBERS_CHUNK: guild %s chunk %d/%d, %u members\n",
        guild_id, chunk_index + 1, chunk_count, n);
}

static void
fluxer_handle_dispatch(FluxerData *fd, const gchar *event_name, JsonObject *d)
{
    purple_debug_info("fluxer", "DISPATCH: %s\n", event_name);

    if      (g_strcmp0(event_name, "READY")           == 0) handle_ready(fd, d);
    else if (g_strcmp0(event_name, "RESUMED")         == 0)
        purple_debug_info("fluxer", "Session resumed\n");
    else if (g_strcmp0(event_name, "MESSAGE_CREATE")  == 0)
        handle_message_create(fd, d);
    else if (g_strcmp0(event_name, "TYPING_START")    == 0)
        handle_typing_start(fd, d);
    else if (g_strcmp0(event_name, "PRESENCE_UPDATE") == 0)
        handle_presence_update(fd, d);
    else if (g_strcmp0(event_name, "CHANNEL_CREATE")  == 0)
        handle_channel_create(fd, d);
    else if (g_strcmp0(event_name, "GUILD_CREATE")         == 0)
        handle_guild_create(fd, d);
    else if (g_strcmp0(event_name, "GUILD_MEMBERS_CHUNK") == 0)
        handle_guild_members_chunk(fd, d);
    else if (g_strcmp0(event_name, "MESSAGE_DELETE")  == 0)
        handle_message_delete(fd, d);
    else if (g_strcmp0(event_name, "MESSAGE_UPDATE")  == 0)
        handle_message_update(fd, d);
    /* TODO: GUILD_DELETE, CHANNEL_DELETE, USER_UPDATE */
}

/* ─── WebSocket connection setup ──────────────────────────────────────── */

static void
fluxer_ssl_error_cb(PurpleSslConnection *ssl, PurpleSslErrorType error,
                    gpointer data)
{
    FluxerData *fd = data;
    purple_connection_ssl_error(fd->gc, error);
}

static void
fluxer_ssl_connected_cb(gpointer data, PurpleSslConnection *ssl,
                        PurpleInputCondition cond)
{
    FluxerData *fd = data;

    /* Send HTTP Upgrade request */
    gchar nonce_raw[16];
    for (int i = 0; i < 16; i++)
        nonce_raw[i] = (gchar)(g_random_int() & 0xFF);
    gchar *nonce_b64 = g_base64_encode((guchar *)nonce_raw, 16);

    gchar *handshake = g_strdup_printf(
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: %s\r\n"
        "\r\n",
        FLUXER_GATEWAY_PATH,
        fd->ws_host,
        nonce_b64,
        FLUXER_USER_AGENT);

    g_free(nonce_b64);

    purple_ssl_write(ssl, handshake, strlen(handshake));
    g_free(handshake);

    purple_ssl_input_add(ssl, fluxer_ssl_recv_cb, fd);
    purple_debug_info("fluxer", "WebSocket upgrade sent to %s\n", fd->ws_host);
    purple_connection_update_progress(fd->gc, "Authenticating with gateway",
                                      2, 3);
}

static void
fluxer_ws_connect(FluxerData *fd)
{
    purple_debug_info("fluxer", "Connecting to gateway: %s:%u\n",
                      fd->ws_host, fd->ws_port);

    fd->ws_handshake_done = FALSE;
    g_string_truncate(fd->ws_recv_buf, 0);
    g_string_truncate(fd->ws_frame_buf, 0);

    fd->ssl = purple_ssl_connect(fd->account, fd->ws_host, fd->ws_port,
                                 fluxer_ssl_connected_cb,
                                 fluxer_ssl_error_cb, fd);
    if (!fd->ssl) {
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
            "Failed to initiate SSL connection to gateway");
    }
}

/* ─── HTTP helpers ────────────────────────────────────────────────────── */

typedef struct {
    FluxerData *fd;
    gpointer    user_data;
    void       (*callback)(FluxerData *, const gchar *, gpointer);
} HttpCallbackData;

static void
fluxer_http_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data,
               const gchar *webdata, gsize len, const gchar *error_message)
{
    purple_debug_info("fluxer", "HTTP callback fired (len=%" G_GSIZE_FORMAT
                      " error=%s)\n", len,
                      error_message ? error_message : "(none)");

    HttpCallbackData *cbd = user_data;
    FluxerData *fd = cbd->fd;

    fd->pending_http = g_slist_remove(fd->pending_http, url_data);

    if (error_message) {
        purple_debug_error("fluxer", "HTTP error: %s\n", error_message);
    } else if (cbd->callback && webdata) {
        purple_debug_info("fluxer", "HTTP response body (first 200): %.200s\n",
                          webdata);
        cbd->callback(fd, webdata, cbd->user_data);
    } else {
        purple_debug_warning("fluxer", "HTTP callback: no body and no error\n");
    }

    g_free(cbd);
}

static void
fluxer_http_request(FluxerData *fd, const gchar *method, const gchar *url,
                    const gchar *body,
                    void (*callback)(FluxerData *, const gchar *, gpointer),
                    gpointer user_data)
{
    HttpCallbackData *cbd = g_new0(HttpCallbackData, 1);
    cbd->fd        = fd;
    cbd->callback  = callback;
    cbd->user_data = user_data;

    /* Always build a raw request so Authorization is included on every call */
    const gchar *scheme_end = url;
    const gchar *host_start = url;
    if      (g_str_has_prefix(url, "https://")) { scheme_end = url + 8; host_start = scheme_end; }
    else if (g_str_has_prefix(url, "http://"))  { scheme_end = url + 7; host_start = scheme_end; }

    const gchar *path_start = strchr(host_start, '/');
    gchar *host = path_start
        ? g_strndup(host_start, path_start - host_start)
        : g_strdup(host_start);
    const gchar *path = path_start ? path_start : "/";

    /* Origin = scheme + host (required by Fluxer's CORS origin check) */
    gchar *origin = g_strndup(url, (scheme_end - url) + strlen(host));

    gsize body_len = body ? strlen(body) : 0;

    /* Only include Content-Type / Content-Length when there is a body */
    gchar *headers;
    if (body) {
        headers = fd->token
            ? g_strdup_printf(
                "%s %s HTTP/1.0\r\n"
                "Connection: close\r\n"
                "Host: %s\r\n"
                "Origin: %s\r\n"
                "User-Agent: %s\r\n"
                "Content-Type: application/json\r\n"
                "Authorization: %s\r\n"
                "Content-Length: %" G_GSIZE_FORMAT "\r\n"
                "\r\n",
                method, path, host, origin, FLUXER_USER_AGENT,
                fluxer_auth_header(fd), body_len)
            : g_strdup_printf(
                "%s %s HTTP/1.0\r\n"
                "Connection: close\r\n"
                "Host: %s\r\n"
                "Origin: %s\r\n"
                "User-Agent: %s\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %" G_GSIZE_FORMAT "\r\n"
                "\r\n",
                method, path, host, origin, FLUXER_USER_AGENT, body_len);
    } else {
        headers = fd->token
            ? g_strdup_printf(
                "%s %s HTTP/1.0\r\n"
                "Connection: close\r\n"
                "Host: %s\r\n"
                "Origin: %s\r\n"
                "User-Agent: %s\r\n"
                "Authorization: %s\r\n"
                "\r\n",
                method, path, host, origin, FLUXER_USER_AGENT,
                fluxer_auth_header(fd))
            : g_strdup_printf(
                "%s %s HTTP/1.0\r\n"
                "Connection: close\r\n"
                "Host: %s\r\n"
                "Origin: %s\r\n"
                "User-Agent: %s\r\n"
                "\r\n",
                method, path, host, origin, FLUXER_USER_AGENT);
    }
    g_free(origin);

    gsize headers_len = strlen(headers);
    gsize request_len = headers_len + body_len;
    gchar *request = g_malloc(request_len + 1);
    memcpy(request, headers, headers_len);
    if (body) memcpy(request + headers_len, body, body_len);
    request[request_len] = '\0';

    g_free(headers);
    g_free(host);

    purple_debug_info("fluxer", "HTTP %s %s (body_len=%" G_GSIZE_FORMAT ")\n",
                      method, url, request_len);

    PurpleUtilFetchUrlData *fetch_data =
        purple_util_fetch_url_request_data_len_with_account(
            fd->account, url, TRUE, FLUXER_USER_AGENT, FALSE,
            request, request_len, FALSE, -1,
            fluxer_http_cb, cbd);

    g_free(request);

    if (fetch_data) {
        purple_debug_info("fluxer", "HTTP fetch started OK\n");
        fd->pending_http = g_slist_prepend(fd->pending_http, fetch_data);
    } else {
        purple_debug_error("fluxer", "HTTP fetch failed to start (returned NULL)\n");
        g_free(cbd);
    }
}

/* ─── Login flow ──────────────────────────────────────────────────────── */

/* Step 3: got gateway URL → open WebSocket */
static void
fluxer_got_gateway_url(FluxerData *fd, const gchar *body, gpointer user_data)
{
    (void)user_data;
    JsonObject *root = string_to_json_object(body);
    if (!root) {
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to parse gateway URL");
        return;
    }

    const gchar *ws_url = json_object_get_string_member(root, "url");
    if (!ws_url) {
        purple_debug_error("fluxer", "Gateway response missing 'url' field: %s\n",
                           body);
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Gateway URL response missing url field");
        json_object_unref(root);
        return;
    }
    purple_debug_info("fluxer", "Gateway URL: %s\n", ws_url);

    /* Parse "wss://hostname" — strip scheme, extract host */
    const gchar *host = ws_url;
    if (g_str_has_prefix(ws_url, "wss://"))
        host = ws_url + 6;
    else if (g_str_has_prefix(ws_url, "ws://"))
        host = ws_url + 5;

    g_free(fd->ws_host);
    fd->ws_host = g_strdup(host);
    /* Strip trailing slash or path if any */
    gchar *slash = strchr(fd->ws_host, '/');
    if (slash) *slash = '\0';

    fd->ws_port = FLUXER_GATEWAY_PORT;

    json_object_unref(root);

    fluxer_ws_connect(fd);
}

/* Step 2: logged in, got token → connect to gateway.
 * /gateway/bot is a bot-only endpoint; user accounts connect directly
 * to the well-known gateway host defined in FLUXER_GATEWAY_HOST. */
static void
fluxer_use_token(FluxerData *fd)
{
    purple_debug_info("fluxer", "Connecting to gateway %s:%d\n",
                      FLUXER_GATEWAY_HOST, FLUXER_GATEWAY_PORT);
    purple_connection_update_progress(fd->gc, "Connecting to gateway", 1, 3);
    fd->ws_host = g_strdup(FLUXER_GATEWAY_HOST);
    fd->ws_port = FLUXER_GATEWAY_PORT;
    fluxer_ws_connect(fd);
}

/* Step 1a: got login response */
static void
fluxer_got_login_response(FluxerData *fd, const gchar *body, gpointer user_data)
{
    (void)user_data;
    purple_debug_info("fluxer", "Got login response\n");
    JsonObject *root = string_to_json_object(body);
    if (!root) {
        purple_debug_error("fluxer", "Login response JSON parse failed\n");
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
            "Failed to parse login response");
        return;
    }

    if (!json_object_has_member(root, "token")) {
        const gchar *msg = json_object_has_member(root, "message")
            ? json_object_get_string_member(root, "message")
            : "Login failed — check email and password";
        purple_debug_error("fluxer", "Login failed: %s\n", msg);
        purple_connection_error_reason(fd->gc,
            PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, msg);
        json_object_unref(root);
        return;
    }

    g_free(fd->token);
    fd->token = g_strdup(json_object_get_string_member(root, "token"));
    purple_debug_info("fluxer", "Login successful, got token\n");

    /* Optionally persist token */
    purple_account_set_string(fd->account, "token", fd->token);

    json_object_unref(root);
    fluxer_use_token(fd);
}

/* ─── libpurple protocol ops ──────────────────────────────────────────── */

static void
fluxer_login(PurpleAccount *account)
{
    PurpleConnection *gc = purple_account_get_connection(account);
    FluxerData *fd = fluxer_data_new(gc);
    purple_connection_set_protocol_data(gc, fd);

    const gchar *api_base_setting =
        purple_account_get_string(account, "api_base", FLUXER_API_BASE);
    fd->api_base = g_strdup((api_base_setting && *api_base_setting)
                            ? api_base_setting : FLUXER_API_BASE);

    purple_connection_update_progress(gc, "Connecting", 0, 3);

    /* Prefer an explicitly stored token (e.g., bot token or saved session) */
    const gchar *stored_token =
        purple_account_get_string(account, "token", NULL);

    if (stored_token && *stored_token) {
        fd->token = g_strdup(stored_token);
        purple_debug_info("fluxer", "Using stored token (len=%zu prefix=%.8s...)\n",
                          strlen(stored_token), stored_token);
        fluxer_use_token(fd);
        return;
    }

    /* Fall back to email + password */
    const gchar *email    = purple_account_get_username(account);
    const gchar *password = purple_account_get_password(account);

    if (!password || !*password) {
        purple_connection_error_reason(gc,
            PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
            "Password required (or set a token in Advanced settings)");
        return;
    }

    JsonObject *body_obj = json_object_new();
    json_object_set_string_member(body_obj, "email",    email);
    json_object_set_string_member(body_obj, "password", password);
    gchar *body_str = json_object_to_string(body_obj);
    json_object_unref(body_obj);

    gchar *url = g_strdup_printf("%s/auth/login", fd->api_base);
    purple_connection_update_progress(gc, "Authenticating", 0, 3);
    fluxer_http_request(fd, "POST", url, body_str,
                        fluxer_got_login_response, NULL);
    g_free(url);
    g_free(body_str);
}

static void
fluxer_close(PurpleConnection *gc)
{
    FluxerData *fd = purple_connection_get_protocol_data(gc);
    if (!fd) return;

    /* Send WS close frame */
    if (fd->ssl && fd->ws_handshake_done) {
        guint8 close_code[2] = {0x03, 0xE8};  /* 1000 Normal Closure */
        gsize  frame_len;
        guint8 *frame = ws_build_frame(close_code, 2, WS_OP_CLOSE, &frame_len);
        purple_ssl_write(fd->ssl, frame, frame_len);
        g_free(frame);
    }

    fluxer_data_free(fd);
    purple_connection_set_protocol_data(gc, NULL);
}

/* ─── Messaging ───────────────────────────────────────────────────────── */

typedef struct {
    FluxerData *fd;
    gchar *who;         /* buddy username */
    gchar *message;
} SendImData;

static void
fluxer_sent_message_cb(FluxerData *fd, const gchar *body, gpointer user_data)
{
    (void)fd; (void)body; (void)user_data;
    /* Could parse message ID here for optimistic echo */
}

static void
fluxer_got_open_dm_cb(FluxerData *fd, const gchar *body, gpointer user_data)
{
    SendImData *sid = user_data;

    JsonObject *root = string_to_json_object(body);
    if (!root) { g_free(sid->who); g_free(sid->message); g_free(sid); return; }

    const gchar *ch_id = json_object_get_string_member(root, "id");

    /* Cache dm_channel for this user */
    /* We need user_id; look it up from who in dm_channels by reverse scan */
    /* For now use the channel id directly */
    gchar *url  = g_strdup_printf("%s/channels/%s/messages", fd->api_base, ch_id);
    JsonObject *msg_body = json_object_new();
    json_object_set_string_member(msg_body, "content", sid->message);
    gchar *msg_str = json_object_to_string(msg_body);
    json_object_unref(msg_body);
    json_object_unref(root);

    fluxer_http_request(fd, "POST", url, msg_str,
                        fluxer_sent_message_cb, NULL);
    g_free(url);
    g_free(msg_str);
    g_free(sid->who);
    g_free(sid->message);
    g_free(sid);
}

static int
fluxer_send_im(PurpleConnection *gc, const gchar *who,
               const gchar *message, PurpleMessageFlags flags)
{
    FluxerData *fd = purple_connection_get_protocol_data(gc);

    /* Find channel_id for this buddy via dm_channels (keyed by user_id).
     * We may know their user_id from handle if it matches; otherwise we need
     * to open a DM. For now we look up the buddy's "uid" proto_data field or
     * fall back to opening a DM by username. */

    /* Try to find existing DM channel by scanning dm_channels values */
    gchar *ch_id = NULL;
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, fd->dm_channels);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        /* k = user_id, v = channel_id
         * We'd need a separate user_id→username map to do this right;
         * for now we open a DM via the API using a search for the buddy */
        (void)k; (void)v;
    }

    if (!ch_id) {
        /* Open DM — Fluxer uses POST /users/@me/channels with recipient_id.
         * We need the recipient's user_id; we'll use a buddy data annotation
         * that we set during READY / PRESENCE_UPDATE. */
        PurpleBuddy *buddy = purple_find_buddy(fd->account, who);
        const gchar *uid = buddy
            ? purple_buddy_get_protocol_data(buddy) : NULL;

        if (!uid) {
            purple_debug_warning("fluxer",
                "Cannot send to %s: user_id unknown. "
                "Try opening a DM from the buddy list first.\n", who);
            return -1;
        }

        gchar *url = g_strdup_printf("%s/users/@me/channels", fd->api_base);
        JsonObject *body_obj = json_object_new();
        json_object_set_string_member(body_obj, "recipient_id", uid);
        gchar *body_str = json_object_to_string(body_obj);
        json_object_unref(body_obj);

        SendImData *sid = g_new0(SendImData, 1);
        sid->fd      = fd;
        sid->who     = g_strdup(who);
        sid->message = purple_unescape_html(message);

        fluxer_http_request(fd, "POST", url, body_str,
                            fluxer_got_open_dm_cb, sid);
        g_free(url);
        g_free(body_str);
        return 1;
    }

    gchar *url = g_strdup_printf("%s/channels/%s/messages",
                                 fd->api_base, ch_id);
    JsonObject *body_obj = json_object_new();
    gchar *plain = purple_unescape_html(message);
    json_object_set_string_member(body_obj, "content", plain);
    gchar *body_str = json_object_to_string(body_obj);
    json_object_unref(body_obj);
    g_free(plain);

    fluxer_http_request(fd, "POST", url, body_str,
                        fluxer_sent_message_cb, NULL);
    g_free(url);
    g_free(body_str);
    return 1;
}

static int
fluxer_send_chat(PurpleConnection *gc, int id,
                 const gchar *message, PurpleMessageFlags flags)
{
    FluxerData *fd = purple_connection_get_protocol_data(gc);

    /* Reverse-lookup channel_id from chat_id */
    gchar *ch_id = NULL;
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, fd->chat_id_map);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        if (GPOINTER_TO_INT(v) == id) {
            ch_id = (gchar *)k;
            break;
        }
    }
    if (!ch_id) return -1;

    gchar *url = g_strdup_printf("%s/channels/%s/messages",
                                 fd->api_base, ch_id);
    JsonObject *body_obj = json_object_new();
    gchar *plain = purple_unescape_html(message);
    json_object_set_string_member(body_obj, "content", plain);
    gchar *body_str = json_object_to_string(body_obj);
    json_object_unref(body_obj);
    g_free(plain);

    fluxer_http_request(fd, "POST", url, body_str,
                        fluxer_sent_message_cb, NULL);
    g_free(url);
    g_free(body_str);
    return 1;
}

static unsigned int
fluxer_send_typing(PurpleConnection *gc, const gchar *name,
                   PurpleTypingState state)
{
    /* TODO: find channel_id, POST /channels/{id}/typing */
    (void)gc; (void)name; (void)state;
    return 0;
}

/* ─── Channel history ─────────────────────────────────────────────────── */

static time_t
parse_timestamp(const gchar *ts)
{
    if (!ts) return time(NULL);
    GDateTime *dt = g_date_time_new_from_iso8601(ts, NULL);
    if (!dt) return time(NULL);
    time_t result = (time_t)g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return result;
}

typedef struct {
    FluxerData *fd;
    gchar      *channel_id;
    gint        chat_id;
    gboolean    show_separator; /* TRUE for /more fetches; FALSE for initial join load */
} HistoryFetchData;

static void
fluxer_got_history_cb(FluxerData *fd, const gchar *body, gpointer user_data)
{
    HistoryFetchData *hfd = user_data;

    if (!body) goto done;

    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, body, -1, &err)) {
        purple_debug_error("fluxer", "History parse error: %s\n", err->message);
        g_error_free(err);
        g_object_unref(parser);
        goto done;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root_node)) {
        purple_debug_error("fluxer", "History: expected JSON array\n");
        g_object_unref(parser);
        goto done;
    }

    JsonArray *msgs = json_node_get_array(root_node);
    guint n = json_array_get_length(msgs);

    /* The array is newest-first. Index 0 = newest, index n-1 = oldest. */
    JsonObject *newest_msg = json_array_get_object_element(msgs, 0);
    JsonObject *oldest_msg = json_array_get_object_element(msgs, n - 1);

    if (hfd->show_separator) {
        if (n == 0) {
            fluxer_conv_notice(fd, hfd->channel_id, "── No older messages ──");
            g_object_unref(parser);
            goto done;
        }

        const gchar *newest_ts_str = json_object_get_string_member(newest_msg, "timestamp");
        const gchar *oldest_ts_str = json_object_get_string_member(oldest_msg, "timestamp");
        gchar *header = g_strdup_printf(
            "── History: %u messages (%s → %s) ──────────",
            n,
            oldest_ts_str ? oldest_ts_str : "?",
            newest_ts_str ? newest_ts_str : "?");
        fluxer_conv_notice(fd, hfd->channel_id, header);
        g_free(header);
    }

    /* Replay oldest-first into the chat window */
    for (gint i = (gint)n - 1; i >= 0; i--) {
        JsonObject *msg      = json_array_get_object_element(msgs, i);
        const gchar *content = json_object_get_string_member(msg, "content");
        if (!content || *content == '\0') continue;

        JsonObject  *author    = json_object_get_object_member(msg, "author");
        const gchar *username  = json_object_get_string_member(author, "username");
        const gchar *author_id = json_object_get_string_member(author, "id");
        time_t ts = parse_timestamp(
            json_object_get_string_member(msg, "timestamp"));

        g_hash_table_insert(fd->user_names,
                            g_strdup(author_id), g_strdup(username));

        serv_got_chat_in(fd->gc, hfd->chat_id, username,
                         PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_DELAYED,
                         content, ts);
    }

    if (hfd->show_separator)
        fluxer_conv_notice(fd, hfd->channel_id,
                           "── End of history block — live messages follow ──");

    /* Track the oldest message ID for the next /more page */
    const gchar *oldest_id = json_object_get_string_member(oldest_msg, "id");
    if (oldest_id)
        g_hash_table_insert(fd->oldest_msg_id,
                            g_strdup(hfd->channel_id), g_strdup(oldest_id));

    purple_debug_info("fluxer", "History: loaded %u messages for %s\n",
                      n, hfd->channel_id);
    g_object_unref(parser);

done:
    g_free(hfd->channel_id);
    g_free(hfd);
}

/* ─── Chat (guild channel) ops ────────────────────────────────────────── */

static GList *
fluxer_chat_info(PurpleConnection *gc)
{
    (void)gc;
    GList *info = NULL;
    struct proto_chat_entry *pce = g_new0(struct proto_chat_entry, 1);
    pce->label      = "Channel ID";
    pce->identifier = "channel_id";
    pce->required   = TRUE;
    info = g_list_append(info, pce);
    return info;
}

static GHashTable *
fluxer_chat_info_defaults(PurpleConnection *gc, const char *chat_name)
{
    (void)gc;
    GHashTable *defaults =
        g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    if (chat_name)
        g_hash_table_insert(defaults, "channel_id", g_strdup(chat_name));
    return defaults;
}

static char *
fluxer_get_chat_name(GHashTable *components)
{
    const gchar *ch_id = g_hash_table_lookup(components, "channel_id");
    return ch_id ? g_strdup(ch_id) : NULL;
}

static void
fluxer_join_chat(PurpleConnection *gc, GHashTable *components)
{
    FluxerData *fd = purple_connection_get_protocol_data(gc);
    const gchar *channel_id = g_hash_table_lookup(components, "channel_id");
    if (!channel_id) return;

    gpointer existing = g_hash_table_lookup(fd->chat_id_map, channel_id);
    if (existing) {
        PurpleConversation *conv =
            purple_find_chat(gc, GPOINTER_TO_INT(existing));
        if (conv) purple_conversation_present(conv);
        return;
    }

    gint chat_id = fd->next_chat_id++;
    g_hash_table_insert(fd->chat_id_map,
                        g_strdup(channel_id), GINT_TO_POINTER(chat_id));

    const gchar *ch_name = g_hash_table_lookup(fd->channel_names, channel_id);
    serv_got_joined_chat(gc, chat_id, ch_name ? ch_name : channel_id);

    /* Populate the participant list from the guild's member roster */
    const gchar *guild_id = g_hash_table_lookup(fd->channel_to_guild, channel_id);
    if (guild_id) {
        GList *members = g_hash_table_lookup(fd->guild_members, guild_id);
        if (members) {
            PurpleConversation *conv = purple_find_chat(gc, chat_id);
            if (conv) {
                /* Build a parallel flags list (all NONE) */
                GList *flags = NULL;
                for (GList *l = members; l; l = l->next)
                    flags = g_list_prepend(flags, GINT_TO_POINTER(PURPLE_CBFLAGS_NONE));
                purple_conv_chat_add_users(PURPLE_CONV_CHAT(conv),
                                          members, NULL, flags, FALSE);
                g_list_free(flags);
            }
        }
    }

    /* Fetch recent history */
    HistoryFetchData *hfd = g_new0(HistoryFetchData, 1);
    hfd->fd         = fd;
    hfd->channel_id = g_strdup(channel_id);
    hfd->chat_id    = chat_id;
    gchar *url = g_strdup_printf("%s/channels/%s/messages?limit=50",
                                 fd->api_base, channel_id);
    fluxer_http_request(fd, "GET", url, NULL, fluxer_got_history_cb, hfd);
    g_free(url);
}

/* ─── Status/presence ─────────────────────────────────────────────────── */

static GList *
fluxer_status_types(PurpleAccount *account)
{
    GList *types = NULL;
    PurpleStatusType *type;

    type = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE,
               "online", "Online", TRUE, TRUE, FALSE);
    types = g_list_append(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_AWAY,
               "idle", "Idle", TRUE, TRUE, FALSE);
    types = g_list_append(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_UNAVAILABLE,
               "dnd", "Do Not Disturb", TRUE, TRUE, FALSE);
    types = g_list_append(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_INVISIBLE,
               "invisible", "Invisible", TRUE, TRUE, FALSE);
    types = g_list_append(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_OFFLINE,
               "offline", "Offline", FALSE, TRUE, FALSE);
    types = g_list_append(types, type);

    return types;
}

static void
fluxer_set_status(PurpleAccount *account, PurpleStatus *status)
{
    PurpleConnection *gc = purple_account_get_connection(account);
    FluxerData *fd = purple_connection_get_protocol_data(gc);
    if (!fd || !fd->ws_handshake_done) return;

    const gchar *id = purple_status_get_id(status);

    JsonObject *d = json_object_new();
    json_object_set_string_member(d, "status", id);
    json_object_set_null_member  (d, "since");
    json_object_set_boolean_member(d, "afk",
        g_strcmp0(id, "idle") == 0);
    json_object_set_array_member(d, "activities", json_array_new());

    JsonObject *payload = json_object_new();
    json_object_set_int_member   (payload, "op", OP_PRESENCE_UPDATE);
    json_object_set_object_member(payload, "d",  d);

    fluxer_ws_send_json(fd, payload);
    json_object_unref(payload);
}

/* ─── Room list (guild channel browser) ──────────────────────────────── */

static PurpleRoomlist *
fluxer_roomlist_get_list(PurpleConnection *gc)
{
    FluxerData *fd = purple_connection_get_protocol_data(gc);
    PurpleRoomlist *rl = purple_roomlist_new(fd->account);

    GList *fields = NULL;
    fields = g_list_append(fields,
        purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING,
                                  "Guild", "guild_id", TRUE));

    purple_roomlist_set_fields(rl, fields);

    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, fd->channel_names);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        const gchar *ch_id   = k;
        const gchar *ch_name = v;
        const gchar *guild   = g_hash_table_lookup(fd->channel_to_guild, ch_id);

        PurpleRoomlistRoom *room =
            purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM,
                                     ch_name, NULL);
        purple_roomlist_room_add_field(rl, room, ch_id);
        if (guild) purple_roomlist_room_add_field(rl, room, guild);
        purple_roomlist_room_add(rl, room);
    }

    purple_roomlist_set_in_progress(rl, FALSE);
    return rl;
}

static void
fluxer_roomlist_cancel(PurpleRoomlist *list)
{
    purple_roomlist_set_in_progress(list, FALSE);
}

/* ─── Account options ─────────────────────────────────────────────────── */

static GList *
fluxer_account_options(void)
{
    GList *opts = NULL;
    PurpleAccountOption *opt;

    opt = purple_account_option_string_new(
        "Token (overrides email/password login)",
        "token", "");
    opts = g_list_append(opts, opt);

    opt = purple_account_option_string_new(
        "API base URL (for self-hosted instances)",
        "api_base", FLUXER_API_BASE);
    opts = g_list_append(opts, opt);

    return opts;
}

/* ─── Plugin info and registration ───────────────────────────────────── */

static PurplePluginProtocolInfo prpl_info = {
    OPT_PROTO_PASSWORD_OPTIONAL,    /* options */
    NULL,                           /* user_splits */
    NULL,                           /* protocol_options (set in plugin_init) */
    NO_BUDDY_ICONS,                 /* icon_spec */
    fluxer_list_icon,               /* list_icon */
    NULL,                           /* list_emblem */
    NULL,                           /* status_text */
    NULL,                           /* tooltip_text */
    fluxer_status_types,            /* status_types */
    NULL,                           /* blist_node_menu */
    fluxer_chat_info,               /* chat_info */
    fluxer_chat_info_defaults,      /* chat_info_defaults */
    fluxer_login,                   /* login */
    fluxer_close,                   /* close */
    fluxer_send_im,                 /* send_im */
    NULL,                           /* set_info */
    fluxer_send_typing,             /* send_typing */
    NULL,                           /* get_info */
    fluxer_set_status,              /* set_status */
    NULL,                           /* set_idle */
    NULL,                           /* change_passwd */
    NULL,                           /* add_buddy */
    NULL,                           /* add_buddies */
    NULL,                           /* remove_buddy */
    NULL,                           /* remove_buddies */
    NULL,                           /* add_permit */
    NULL,                           /* add_deny */
    NULL,                           /* rem_permit */
    NULL,                           /* rem_deny */
    NULL,                           /* set_permit_deny */
    fluxer_join_chat,               /* join_chat */
    NULL,                           /* reject_chat */
    fluxer_get_chat_name,           /* get_chat_name */
    NULL,                           /* chat_invite */
    NULL,                           /* chat_leave */
    NULL,                           /* chat_whisper */
    fluxer_send_chat,               /* chat_send */
    NULL,                           /* keepalive */
    NULL,                           /* register_user */
    NULL,                           /* get_cb_info */
    NULL,                           /* get_cb_away */
    NULL,                           /* alias_buddy */
    NULL,                           /* group_buddy */
    NULL,                           /* rename_group */
    NULL,                           /* buddy_free */
    NULL,                           /* convo_closed */
    purple_normalize_nocase,        /* normalize */
    NULL,                           /* set_buddy_icon */
    NULL,                           /* remove_group */
    NULL,                           /* get_cb_real_name */
    NULL,                           /* set_chat_topic */
    NULL,                           /* find_blist_chat */
    fluxer_roomlist_get_list,       /* roomlist_get_list */
    fluxer_roomlist_cancel,         /* roomlist_cancel */
    NULL,                           /* roomlist_expand_category */
    NULL,                           /* can_receive_file */
    NULL,                           /* send_file */
    NULL,                           /* new_xfer */
    NULL,                           /* offline_message */
    NULL,                           /* whiteboard_prpl_ops */
    NULL,                           /* send_raw */
    NULL,                           /* roomlist_room_serialize */
    NULL,                           /* unregister_user */
    NULL,                           /* send_attention */
    NULL,                           /* get_attention_types */
    sizeof(PurplePluginProtocolInfo),
    NULL,                           /* get_account_text_table */
    NULL,                           /* initiate_media */
    NULL,                           /* get_media_caps */
    NULL,                           /* get_moods */
    NULL,                           /* set_public_alias */
    NULL,                           /* get_public_alias */
    NULL,                           /* add_buddy_with_invite */
    NULL                            /* add_buddies_with_invite */
};

/* Icon name — Pidgin looks for prpl-fluxer.png in its pixmaps dir */
static const gchar *
fluxer_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
    return "fluxer";
}

/* ─── /more slash command ─────────────────────────────────────────────── */

static PurpleCmdId fluxer_cmd_more_id = 0;

static PurpleCmdRet
fluxer_cmd_more(PurpleConversation *conv, const gchar *cmd,
                gchar **args, gchar **error, void *data)
{
    (void)cmd; (void)args; (void)data;

    PurpleConnection *gc = purple_conversation_get_gc(conv);
    if (!gc) return PURPLE_CMD_RET_FAILED;

    FluxerData *fd = purple_connection_get_protocol_data(gc);
    if (!fd) return PURPLE_CMD_RET_FAILED;

    /* Resolve chat_id → channel_id */
    gint chat_id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv));
    gchar *channel_id = NULL;
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, fd->chat_id_map);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        if (GPOINTER_TO_INT(v) == chat_id) {
            channel_id = (gchar *)k;
            break;
        }
    }

    if (!channel_id) {
        *error = g_strdup("Cannot determine channel for this conversation.");
        return PURPLE_CMD_RET_FAILED;
    }

    const gchar *before_id = g_hash_table_lookup(fd->oldest_msg_id, channel_id);
    if (!before_id) {
        *error = g_strdup("No history cursor — open the channel first.");
        return PURPLE_CMD_RET_FAILED;
    }

    HistoryFetchData *hfd = g_new0(HistoryFetchData, 1);
    hfd->fd             = fd;
    hfd->channel_id     = g_strdup(channel_id);
    hfd->chat_id        = chat_id;
    hfd->show_separator = TRUE;

    gchar *url = g_strdup_printf("%s/channels/%s/messages?limit=50&before=%s",
                                 fd->api_base, channel_id, before_id);
    fluxer_http_request(fd, "GET", url, NULL, fluxer_got_history_cb, hfd);
    g_free(url);

    return PURPLE_CMD_RET_OK;
}

static gboolean
fluxer_plugin_load(PurplePlugin *plugin)
{
    prpl_info.protocol_options = fluxer_account_options();

    fluxer_cmd_more_id = purple_cmd_register(
        "more", "", PURPLE_CMD_P_PLUGIN,
        PURPLE_CMD_FLAG_CHAT, FLUXER_PLUGIN_ID,
        fluxer_cmd_more,
        "more: Load older message history for this channel",
        NULL);

    return TRUE;
}

static gboolean
fluxer_plugin_unload(PurplePlugin *plugin)
{
    if (fluxer_cmd_more_id != 0) {
        purple_cmd_unregister(fluxer_cmd_more_id);
        fluxer_cmd_more_id = 0;
    }
    return TRUE;
}

static void
fluxer_plugin_init(PurplePlugin *plugin)
{
    (void)plugin;
}

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_PROTOCOL,
    NULL,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,

    FLUXER_PLUGIN_ID,
    "Fluxer",
    FLUXER_PLUGIN_VERSION,

    "Fluxer protocol plugin",
    "Connect to Fluxer (https://fluxer.app), a free and open-source "
    "Discord-compatible messaging platform.",
    "purple-fluxer contributors",
    "https://github.com/beadon/purple-fluxer",

    fluxer_plugin_load,
    fluxer_plugin_unload,
    NULL,

    NULL,
    &prpl_info,
    NULL,
    NULL,
    NULL, NULL, NULL, NULL
};

PURPLE_INIT_PLUGIN(fluxer, fluxer_plugin_init, info)
