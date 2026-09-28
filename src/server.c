/* Web server kecil: menyajikan halaman iPad dan kanal WebSocket
 * (signaling WebRTC + event sentuh/Pencil/keyboard).
 * SPDX-License-Identifier: MIT
 */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>

#include "tsunagupad.h"

struct _Client {
    App *app;
    SoupWebsocketConnection *conn;
    char *addr;
    gboolean authed;
    guint auth_timeout;
};

static const char *content_type(const char *path)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" },
        { ".webmanifest", "application/manifest+json" },
        { ".svg", "image/svg+xml" },
        { ".png", "image/png" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(types); i++)
        if (g_str_has_suffix(path, types[i].ext))
            return types[i].type;
    return "application/octet-stream";
}

static void http_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
                         GHashTable *query, gpointer user_data)
{
    App *app = user_data;
    const char *method = soup_server_message_get_method(msg);

    if (g_strcmp0(method, "GET") != 0 && g_strcmp0(method, "HEAD") != 0) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }
    if (g_strcmp0(path, "/") == 0)
        path = "/index.html";
    if (strstr(path, "..")) {
        soup_server_message_set_status(msg, SOUP_STATUS_FORBIDDEN, NULL);
        return;
    }

    char *file = g_build_filename(app->web_dir, path, NULL);
    char *data = NULL;
    gsize len = 0;
    gboolean found = g_file_get_contents(file, &data, &len, NULL);
    g_free(file);
    if (!found) {
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }

    SoupMessageHeaders *h = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(h, "Cache-Control", "no-cache");
    soup_server_message_set_response(msg, content_type(path), SOUP_MEMORY_TAKE, data, len);
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

static void send_builder(Client *c, JsonBuilder *b)
{
    if (soup_websocket_connection_get_state(c->conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return;
    JsonNode *root = json_builder_get_root(b);
    char *text = json_to_string(root, FALSE);
    soup_websocket_connection_send_text(c->conn, text);
    g_free(text);
    json_node_unref(root);
}

void server_send(App *app, JsonBuilder *b)
{
    if (app->client)
        send_builder(app->client, b);
}

const char *server_client_addr(App *app)
{
    return app->client ? app->client->addr : NULL;
}

static void send_simple(Client *c, const char *type, const char *key, const char *value)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, type);
    if (key) {
        json_builder_set_member_name(b, key);
        json_builder_add_string_value(b, value);
    }
    json_builder_end_object(b);
    send_builder(c, b);
    g_object_unref(b);
}

/* Lepas klien aktif: hentikan stream dan lepaskan tombol yang masih ditekan. */
static void drop_active(App *app)
{
    Client *c = app->client;
    if (!c)
        return;
    app->client = NULL;
    share_stop(app);
    input_release_all(app->input);
    tsunagu_event("client", "state", "disconnected", "addr", c->addr, NULL);
}

static void client_free(Client *c)
{
    if (c->auth_timeout)
        g_source_remove(c->auth_timeout);
    g_signal_handlers_disconnect_by_data(c->conn, c);
    g_object_unref(c->conn);
    g_free(c->addr);
    g_free(c);
}

static gboolean token_ok(const char *given, const char *expected)
{
    gsize a = strlen(given), b = strlen(expected);
    guint8 diff = a != b;
    for (gsize i = 0; i < a && i < b; i++)
        diff |= given[i] ^ expected[i];
    return diff == 0;
}

static void handle_hello(Client *c, JsonObject *o)
{
    App *app = c->app;
    const char *token = json_object_get_string_member_with_default(o, "token", "");

    if (!token || !token_ok(token, app->token)) {
        send_simple(c, "error", "message", "Kode akses salah. Scan ulang QR dari panel Tsunagu-Pad.");
        tsunagu_event("client", "state", "rejected", "addr", c->addr, NULL);
        soup_websocket_connection_close(c->conn, 4001, "token");
        return;
    }

    c->authed = TRUE;
    if (c->auth_timeout) {
        g_source_remove(c->auth_timeout);
        c->auth_timeout = 0;
    }

    /* Hanya satu iPad sekaligus; yang baru menggantikan yang lama. */
    if (app->client) {
        Client *old = app->client;
        drop_active(app);
        send_simple(old, "error", "message", "Diambil alih perangkat lain.");
        soup_websocket_connection_close(old->conn, 4000, "replaced");
    }
    app->client = c;
    tsunagu_event("client", "state", "connected", "addr", c->addr, NULL);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "welcome");
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, app->out_w);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, app->out_h);
    json_builder_set_member_name(b, "fps");
    json_builder_add_int_value(b, app->fps);
    json_builder_set_member_name(b, "encoder");
    json_builder_add_string_value(b, app->enc_name);
    json_builder_set_member_name(b, "input");
    json_builder_add_boolean_value(b, app->input != NULL);
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, TSUNAGUPAD_VERSION);
    json_builder_end_object(b);
    send_builder(c, b);
    g_object_unref(b);

    GError *err = NULL;
    if (!share_start(app, &err)) {
        send_simple(c, "error", "message", err->message);
        tsunagu_event("error", "message", err->message, NULL);
        g_error_free(err);
    }
}

static void handle_message(Client *c, JsonObject *o)
{
    App *app = c->app;
    const char *type = json_object_get_string_member_with_default(o, "type", "");
    if (!type)
        return;

    if (!c->authed) {
        if (g_str_equal(type, "hello"))
            handle_hello(c, o);
        return;
    }
    if (app->client != c)
        return;

    if (g_str_equal(type, "answer")) {
        const char *sdp = json_object_get_string_member_with_default(o, "sdp", NULL);
        if (sdp && g_getenv("TSUNAGUPAD_DEBUG"))
            g_printerr("answer SDP:\n%s\n", sdp);
        if (sdp)
            share_on_answer(app, sdp);
    } else if (g_str_equal(type, "ice")) {
        const char *cand = json_object_get_string_member_with_default(o, "candidate", NULL);
        if (cand && *cand)
            share_on_ice(app, json_object_get_int_member_with_default(o, "sdpMLineIndex", 0), cand);
    } else if (g_str_equal(type, "keyframe")) {
        share_keyframe(app);
    } else if (g_str_equal(type, "ping")) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "pong");
        json_builder_set_member_name(b, "t");
        json_builder_add_double_value(b, json_object_get_double_member_with_default(o, "t", 0));
        json_builder_end_object(b);
        send_builder(c, b);
        g_object_unref(b);
    } else if (g_str_equal(type, "stats")) {
        char *fps = g_strdup_printf("%.0f", json_object_get_double_member_with_default(o, "fps", 0));
        char *delay = g_strdup_printf("%.0f", json_object_get_double_member_with_default(o, "delay", 0));
        tsunagu_event("stats", "fps", fps, "delay", delay, NULL);
        g_free(fps);
        g_free(delay);
    } else if (app->input) {
        input_handle(app->input, o);
    }
}

static void on_message(SoupWebsocketConnection *conn, gint type, GBytes *bytes, gpointer user_data)
{
    if (type != SOUP_WEBSOCKET_DATA_TEXT)
        return;

    gsize len;
    const char *data = g_bytes_get_data(bytes, &len);
    JsonParser *parser = json_parser_new_immutable();
    if (json_parser_load_from_data(parser, data, len, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            handle_message(user_data, json_node_get_object(root));
    }
    g_object_unref(parser);
}

static void on_closed(SoupWebsocketConnection *conn, gpointer user_data)
{
    Client *c = user_data;
    if (c->app->client == c)
        drop_active(c->app);
    client_free(c);
}

static gboolean auth_expired(gpointer user_data)
{
    Client *c = user_data;
    c->auth_timeout = 0;
    soup_websocket_connection_close(c->conn, 4001, "timeout");
    return G_SOURCE_REMOVE;
}

static void ws_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
                       SoupWebsocketConnection *conn, gpointer user_data)
{
    Client *c = g_new0(Client, 1);
    c->app = user_data;
    c->conn = g_object_ref(conn);
    c->addr = g_strdup(soup_server_message_get_remote_host(msg));

    /* Matikan algoritma Nagle: event Pencil harus langsung terkirim. */
    GSocket *sock = soup_server_message_get_socket(msg);
    if (sock)
        g_socket_set_option(sock, IPPROTO_TCP, TCP_NODELAY, 1, NULL);

    soup_websocket_connection_set_keepalive_interval(conn, 5);
    g_signal_connect(conn, "message", G_CALLBACK(on_message), c);
    g_signal_connect(conn, "closed", G_CALLBACK(on_closed), c);
    c->auth_timeout = g_timeout_add_seconds(15, auth_expired, c);
}

gboolean server_start(App *app, GError **error)
{
    app->server = soup_server_new("server-header", "Tsunagu-Pad", NULL);
    soup_server_add_websocket_handler(app->server, "/ws", NULL, NULL, ws_handler, app, NULL);
    soup_server_add_handler(app->server, NULL, http_handler, app, NULL);
    return soup_server_listen_all(app->server, app->port, 0, error);
}

void server_stop(App *app)
{
    if (!app->server)
        return;
    if (app->client) {
        Client *c = app->client;
        drop_active(app);
        soup_websocket_connection_close(c->conn, 1001, "server stop");
    }
    soup_server_disconnect(app->server);
    g_clear_object(&app->server);
}
