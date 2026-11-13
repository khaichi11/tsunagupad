/* Web server kecil: menyajikan halaman iPad dan kanal WebSocket
 * (signaling WebRTC + event sentuh/Pencil/keyboard).
 *
 * Keamanan (lihat juga src/crypto.c):
 *   - kode akses tidak pernah lewat jaringan; iPad membuktikannya lewat
 *     challenge-response, lalu semua pesan dienkripsi dan diautentikasi;
 *   - hanya alamat jaringan lokal, link-local, dan Tailscale yang dilayani,
 *     kecuali dijalankan dengan --allow-public;
 *   - percobaan kode yang salah dibatasi per alamat;
 *   - WebSocket dari halaman situs lain (Origin berbeda) ditolak;
 *   - header HTTP mencegah halaman dibingkai situs lain dan membatasi skrip
 *     hanya dari server ini.
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <string.h>

#include "crypto.h"
#include "tsunagupad.h"

/* Percobaan kode akses yang salah: setelah AUTH_MAX_FAILS dalam AUTH_WINDOW
 * detik, alamat itu harus menunggu AUTH_LOCK detik. */
#define AUTH_MAX_FAILS 5
#define AUTH_WINDOW    60
#define AUTH_LOCK      60
/* SDP jawaban Safari sekitar 3-6 KB; batas ini jauh di atasnya. */
#define MAX_MESSAGE    (256 * 1024)

struct _Client {
    App *app;
    SoupWebsocketConnection *conn;
    char *addr;
    gboolean authed;
    guint auth_timeout;
    guint8 server_nonce[NONCE_LEN];
    Channel channel;
};

typedef struct {
    guint fails;
    gint64 first;           /* awal jendela hitungan (µs, monotonic) */
    gint64 locked_until;
} AuthRecord;

static GHashTable *auth_records;   /* alamat -> AuthRecord */

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

/* ---------- siapa yang boleh menyambung ---------- */

/* Loopback, jaringan privat (RFC 1918), link-local, CGNAT 100.64/10 yang
 * dipakai Tailscale, dan padanannya di IPv6 (ULA, link-local, alamat IPv4
 * yang dibungkus IPv6). */
static gboolean address_is_local(GSocketAddress *sa)
{
    if (!G_IS_INET_SOCKET_ADDRESS(sa))
        return FALSE;
    GInetAddress *ia = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sa));
    if (g_inet_address_get_is_loopback(ia) || g_inet_address_get_is_link_local(ia))
        return TRUE;
    const guint8 *b = g_inet_address_to_bytes(ia);
    gsize n = g_inet_address_get_native_size(ia);
    if (n == 16) {
        if ((b[0] & 0xfe) == 0xfc)
            return TRUE;                    /* fc00::/7, ULA (termasuk Tailscale IPv6) */
        static const guint8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(b, mapped, 12) != 0)
            return FALSE;
        b += 12;                            /* ::ffff:a.b.c.d */
    } else if (n != 4) {
        return FALSE;
    }
    return b[0] == 10 || b[0] == 127 ||
           (b[0] == 172 && (b[1] & 0xf0) == 16) ||
           (b[0] == 192 && b[1] == 168) ||
           (b[0] == 169 && b[1] == 254) ||
           (b[0] == 100 && (b[1] & 0xc0) == 64);
}

static gboolean client_allowed(App *app, SoupServerMessage *msg)
{
    return app->allow_public || address_is_local(soup_server_message_get_remote_address(msg));
}

/* WebSocket hanya dari halaman yang disajikan server ini sendiri. Safari
 * selalu mengirim Origin; klien tanpa Origin (alat baris perintah) tetap
 * harus lolos kode akses. */
static gboolean origin_ok(SoupServerMessage *msg)
{
    SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
    const char *origin = soup_message_headers_get_one(h, "Origin");
    const char *host = soup_message_headers_get_one(h, "Host");
    if (!origin)
        return TRUE;
    if (!host)
        return FALSE;
    const char *rest = g_str_has_prefix(origin, "http://") ? origin + 7 :
                       g_str_has_prefix(origin, "https://") ? origin + 8 : NULL;
    return rest && g_ascii_strcasecmp(rest, host) == 0;
}

static gboolean auth_locked(const char *addr)
{
    if (!auth_records)
        return FALSE;
    AuthRecord *r = g_hash_table_lookup(auth_records, addr);
    return r && r->locked_until > g_get_monotonic_time();
}

static void auth_failed(const char *addr)
{
    if (!auth_records)
        auth_records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gint64 now = g_get_monotonic_time();
    AuthRecord *r = g_hash_table_lookup(auth_records, addr);
    if (!r) {
        r = g_new0(AuthRecord, 1);
        g_hash_table_insert(auth_records, g_strdup(addr), r);
    }
    if (now - r->first > AUTH_WINDOW * G_USEC_PER_SEC) {
        r->first = now;
        r->fails = 0;
    }
    if (++r->fails >= AUTH_MAX_FAILS) {
        r->locked_until = now + AUTH_LOCK * G_USEC_PER_SEC;
        r->fails = 0;
        r->first = now;
        tsunagu_event("client", "state", "locked", "addr", addr, NULL);
    }
}

static void auth_succeeded(const char *addr)
{
    if (auth_records)
        g_hash_table_remove(auth_records, addr);
}

/* ---------- HTTP ---------- */

static void security_headers(SoupServerMessage *msg)
{
    SoupMessageHeaders *h = soup_server_message_get_response_headers(msg);
    const char *host = soup_message_headers_get_one(soup_server_message_get_request_headers(msg), "Host");
    /* Host dipakai di CSP hanya bila isinya nama/alamat dan port biasa. */
    gboolean host_ok = host && *host && strlen(host) < 128;
    for (const char *p = host; host_ok && *p; p++)
        host_ok = g_ascii_isalnum(*p) || strchr(".-:[]", *p);
    char *csp = g_strdup_printf(
        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        "media-src 'self' blob:; connect-src 'self'%s%s; frame-ancestors 'none'; "
        "base-uri 'none'; form-action 'none'; object-src 'none'",
        host_ok ? " ws://" : "", host_ok ? host : "");
    soup_message_headers_replace(h, "Content-Security-Policy", csp);
    soup_message_headers_replace(h, "X-Content-Type-Options", "nosniff");
    soup_message_headers_replace(h, "X-Frame-Options", "DENY");
    soup_message_headers_replace(h, "Referrer-Policy", "no-referrer");
    soup_message_headers_replace(h, "Cross-Origin-Opener-Policy", "same-origin");
    soup_message_headers_replace(h, "Cross-Origin-Resource-Policy", "same-origin");
    soup_message_headers_replace(h, "Cache-Control", "no-store");
    g_free(csp);
}

static void http_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
                         GHashTable *query, gpointer user_data)
{
    App *app = user_data;
    const char *method = soup_server_message_get_method(msg);

    security_headers(msg);
    if (!client_allowed(app, msg)) {
        soup_server_message_set_status(msg, SOUP_STATUS_FORBIDDEN, NULL);
        return;
    }
    if (g_strcmp0(method, "GET") != 0 && g_strcmp0(method, "HEAD") != 0) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }
    if (g_strcmp0(path, "/") == 0)
        path = "/index.html";
    if (strstr(path, "..") || strchr(path, '\\') || strchr(path, ':')) {
        soup_server_message_set_status(msg, SOUP_STATUS_FORBIDDEN, NULL);
        return;
    }

    /* Tombol pintasan boleh diatur pengguna di ~/.config/tsunagupad/shortcuts.json;
     * bila tidak ada, dipakai berkas bawaan dari folder web. */
    char *file = NULL;
    if (g_strcmp0(path, "/shortcuts.json") == 0) {
        file = g_build_filename(g_get_user_config_dir(), "tsunagupad", "shortcuts.json", NULL);
        if (!g_file_test(file, G_FILE_TEST_EXISTS))
            g_clear_pointer(&file, g_free);
    }
    if (!file)
        file = g_build_filename(app->web_dir, path, NULL);
    char *data = NULL;
    gsize len = 0;
    gboolean found = g_file_test(file, G_FILE_TEST_IS_REGULAR) &&
                     g_file_get_contents(file, &data, &len, NULL);
    g_free(file);
    if (!found) {
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }

#ifdef G_OS_WIN32
    /* Halaman yang sama dipakai untuk Ubuntu dan Windows; nama sistemnya
     * diganti di sini supaya tulisan di iPad menyebut komputer yang benar. */
    if (g_str_equal(path, "/index.html") || g_str_equal(path, "/manifest.webmanifest")) {
        char **parts = g_strsplit(data, "Ubuntu", -1);
        g_free(data);
        data = g_strjoinv("Windows", parts);
        len = strlen(data);
        g_strfreev(parts);
    }
#endif

    soup_server_message_set_response(msg, content_type(path), SOUP_MEMORY_TAKE, data, len);
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

/* ---------- WebSocket ---------- */

static void send_text_plain(Client *c, const char *text)
{
    if (soup_websocket_connection_get_state(c->conn) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_send_text(c->conn, text);
}

/* Setelah lolos kode akses semua pesan dikirim terenkripsi. Sebelum itu
 * hanya tantangan dan pesan galat yang lewat apa adanya. */
static void send_builder(Client *c, JsonBuilder *b)
{
    if (soup_websocket_connection_get_state(c->conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return;
    JsonNode *root = json_builder_get_root(b);
    char *text = json_to_string(root, FALSE);
    if (c->authed) {
        GBytes *frame = channel_seal(&c->channel, text, strlen(text));
        soup_websocket_connection_send_message(c->conn, SOUP_WEBSOCKET_DATA_BINARY, frame);
        g_bytes_unref(frame);
    } else {
        soup_websocket_connection_send_text(c->conn, text);
    }
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
    idle_inhibit_set(FALSE);
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
    memset(&c->channel, 0, sizeof c->channel);
    g_free(c);
}

static gboolean decode_b64(const char *text, guint8 *out, gsize want)
{
    if (!text || strlen(text) > 128)
        return FALSE;
    gsize len = 0;
    guchar *raw = g_base64_decode(text, &len);
    gboolean ok = raw && len == want;
    if (ok)
        memcpy(out, raw, want);
    g_free(raw);
    return ok;
}

static void reject(Client *c, const char *message, gushort code, const char *reason)
{
    send_simple(c, "error", "message", message);
    soup_websocket_connection_close(c->conn, code, reason);
}

static void handle_hello(Client *c, JsonObject *o)
{
    App *app = c->app;
    guint8 client_nonce[NONCE_LEN], proof[32], expected[32];

    if (json_object_get_int_member_with_default(o, "v", 1) < 2) {
        /* Halaman versi lama mengirim kode akses apa adanya; tidak diterima lagi. */
        reject(c, "Halaman Tsunagu-Pad versi lama. Muat ulang halaman ini.", 4001, "old");
        return;
    }
    gboolean ok = decode_b64(json_object_get_string_member_with_default(o, "nonce", NULL),
                             client_nonce, NONCE_LEN) &&
                  decode_b64(json_object_get_string_member_with_default(o, "proof", NULL),
                             proof, sizeof proof);
    if (ok) {
        channel_proof(app->token, c->server_nonce, client_nonce, expected);
        ok = equal_time(proof, expected, sizeof proof);
    }
    if (!ok) {
        auth_failed(c->addr);
        tsunagu_event("client", "state", "rejected", "addr", c->addr, NULL);
        reject(c, "Kode akses salah. Scan ulang QR dari panel Tsunagu-Pad.", 4001, "token");
        return;
    }

    auth_succeeded(c->addr);
    channel_init(&c->channel, app->token, c->server_nonce, client_nonce);
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
    idle_inhibit_set(TRUE);
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
    json_builder_set_member_name(b, "second");
#ifdef G_OS_WIN32
    json_builder_add_boolean_value(b, app->vdisp != NULL);
#else
    json_builder_add_boolean_value(b, app->virtmon != NULL);
#endif
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

static JsonObject *parse_object(JsonParser *parser, const char *data, gsize len)
{
    if (!json_parser_load_from_data(parser, data, (gssize) len, NULL))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    return root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
}

static void on_message(SoupWebsocketConnection *conn, gint type, GBytes *bytes, gpointer user_data)
{
    Client *c = user_data;
    gsize len;
    const guint8 *data = g_bytes_get_data(bytes, &len);
    JsonParser *parser = json_parser_new_immutable();

    if (!c->authed) {
        /* Sebelum lolos hanya satu pesan teks yang berarti: jawaban tantangan. */
        JsonObject *o = type == SOUP_WEBSOCKET_DATA_TEXT ? parse_object(parser, (const char *) data, len) : NULL;
        if (o && g_strcmp0(json_object_get_string_member_with_default(o, "type", ""), "hello") == 0)
            handle_hello(c, o);
        else
            soup_websocket_connection_close(conn, 4001, "auth");
    } else if (type == SOUP_WEBSOCKET_DATA_BINARY) {
        char *text = channel_open(&c->channel, data, len);
        if (!text) {
            /* Bingkai palsu, terulang, atau rusak: sambungan ini tidak lagi bisa dipercaya. */
            tsunagu_event("warning", "message", "Pesan dari iPad gagal diverifikasi; sambungan ditutup", NULL);
            soup_websocket_connection_close(conn, 4002, "integrity");
        } else {
            JsonObject *o = parse_object(parser, text, strlen(text));
            if (o)
                handle_message(c, o);
            memset(text, 0, strlen(text));
            g_free(text);
        }
    }
    /* Pesan teks setelah lolos diabaikan: semua pesan sah sudah terenkripsi. */
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
    App *app = user_data;
    Client *c = g_new0(Client, 1);
    c->app = app;
    c->conn = g_object_ref(conn);
    c->addr = g_strdup(soup_server_message_get_remote_host(msg));

    g_signal_connect(conn, "message", G_CALLBACK(on_message), c);
    g_signal_connect(conn, "closed", G_CALLBACK(on_closed), c);

    if (!client_allowed(app, msg) || !origin_ok(msg)) {
        tsunagu_event("client", "state", "rejected", "addr", c->addr, NULL);
        soup_websocket_connection_close(conn, 4003, "forbidden");
        return;
    }
    if (auth_locked(c->addr)) {
        reject(c, "Terlalu banyak percobaan kode akses. Tunggu satu menit.", 4004, "locked");
        return;
    }
    if (!secure_random(c->server_nonce, sizeof c->server_nonce)) {
        soup_websocket_connection_close(conn, 1011, "random");
        return;
    }

    /* Matikan algoritma Nagle: event Pencil harus langsung terkirim. */
    GSocket *sock = soup_server_message_get_socket(msg);
    if (sock)
        g_socket_set_option(sock, IPPROTO_TCP, TCP_NODELAY, 1, NULL);

    soup_websocket_connection_set_max_incoming_payload_size(conn, MAX_MESSAGE);
    soup_websocket_connection_set_keepalive_interval(conn, 5);
    c->auth_timeout = g_timeout_add_seconds(15, auth_expired, c);

    char *nonce = g_base64_encode(c->server_nonce, sizeof c->server_nonce);
    char *challenge = g_strdup_printf("{\"type\":\"challenge\",\"v\":2,\"nonce\":\"%s\"}", nonce);
    send_text_plain(c, challenge);
    g_free(challenge);
    g_free(nonce);
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
