/* Tsunagu-Pad, satukan layar Ubuntu dan iPad.
 *
 *   tsunagupad share    Layar Ubuntu -> iPad (Safari, WebRTC) + kontrol sentuh/Pencil
 *   tsunagupad receive  Layar iPad -> Ubuntu (AirPlay, lewat UxPlay)
 *   tsunagupad probe    Cek encoder video yang tersedia
 *
 * SPDX-License-Identifier: MIT
 */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <glib-unix.h>

#include "tsunagupad.h"

static const struct {
    const char *name;
    int max_w, fps, kbps;
} presets[] = {
    { "hemat", 960, 30, 1800 },      /* jaringan lemah / tethering Bluetooth */
    { "lancar", 1280, 60, 5000 },    /* bawaan: responsif untuk Wi-Fi/Tailscale */
    { "seimbang", 1600, 60, 8000 },
    { "tajam", 0, 60, 14000 },       /* resolusi asli */
};

void tsunagu_event(const char *event, ...)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "event");
    json_builder_add_string_value(b, event);

    va_list ap;
    va_start(ap, event);
    const char *key;
    while ((key = va_arg(ap, const char *))) {
        const char *value = va_arg(ap, const char *);
        json_builder_set_member_name(b, key);
        json_builder_add_string_value(b, value ? value : "");
    }
    va_end(ap);
    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    char *line = json_to_string(root, FALSE);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    g_free(line);
    json_node_unref(root);
    g_object_unref(b);
}

/* Kode akses disimpan agar shortcut Tsunagu-Pad di layar utama iPad tetap berlaku. */
static char *load_token(gboolean reset)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "tsunagupad", NULL);
    char *path = g_build_filename(dir, "token", NULL);
    char *token = NULL;

    if (!reset && g_file_get_contents(path, &token, NULL, NULL)) {
        g_strstrip(token);
        if (strlen(token) >= 8)
            goto out;
        g_clear_pointer(&token, g_free);
    }

    static const char abc[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    guint8 raw[12];
    if (getrandom(raw, sizeof raw, 0) != sizeof raw)
        g_error("getrandom gagal: %s", g_strerror(errno));
    token = g_malloc(sizeof raw + 1);
    for (guint i = 0; i < sizeof raw; i++)
        token[i] = abc[raw[i] % (sizeof abc - 1)];
    token[sizeof raw] = '\0';

    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents_full(path, token, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
out:
    g_free(dir);
    g_free(path);
    return token;
}

/* ---------- alamat untuk iPad ---------- */

typedef struct {
    int rank;
    guint order;
    char *addr;
} Address;

static int address_rank(const char *ifname)
{
    if (g_str_equal(ifname, "ap0"))
        return 0;   /* Hotspot Tsunagu-Pad: iPad tersambung langsung ke laptop */
    if (g_str_equal(ifname, "tailscale0"))
        return 1;   /* Tailscale menembus client isolation jaringan kampus */
    if (g_str_has_prefix(ifname, "wl"))
        return 2;
    return 3;       /* Ethernet, tethering USB, dll. */
}

static gint compare_address(gconstpointer a, gconstpointer b)
{
    const Address *x = a, *y = b;
    if (x->rank != y->rank)
        return x->rank - y->rank;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* Alamat IPv4 lokal yang bisa dijangkau iPad, urut dari yang paling mungkin dipakai. */
static GPtrArray *local_addresses(void)
{
    static const char *const skip[] = { "docker", "veth", "virbr", "br-", "vboxnet", "vmnet", "lxcbr", "lxdbr" };
    GArray *found = g_array_new(FALSE, FALSE, sizeof(Address));
    struct ifaddrs *ifs;

    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
                continue;
            if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING))
                continue;
            gboolean ignored = FALSE;
            for (guint s = 0; s < G_N_ELEMENTS(skip); s++)
                ignored = ignored || g_str_has_prefix(i->ifa_name, skip[s]);
            if (ignored)
                continue;

            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, buf, sizeof buf);
            Address a = { address_rank(i->ifa_name), found->len, g_strdup(buf) };
            g_array_append_val(found, a);
        }
        freeifaddrs(ifs);
    }

    g_array_sort(found, compare_address);
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < found->len; i++)
        g_ptr_array_add(list, g_array_index(found, Address, i).addr);
    g_array_free(found, TRUE);
    return list;
}

static char *find_web_dir(const char *given)
{
    if (given)
        return g_strdup(given);

    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *bindir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    char *candidates[] = {
        g_build_filename(bindir, "..", "share", "tsunagupad", "web", NULL),
        g_build_filename(bindir, "..", "web", NULL),     /* dijalankan dari folder build/ */
#ifdef TSUNAGUPAD_WEB_DIR
        g_strdup(TSUNAGUPAD_WEB_DIR),
#endif
        NULL,
    };
    char *found = NULL;
    for (int i = 0; candidates[i]; i++) {
        char *index = g_build_filename(candidates[i], "index.html", NULL);
        if (!found && g_file_test(index, G_FILE_TEST_EXISTS))
            found = g_canonicalize_filename(candidates[i], NULL);
        g_free(index);
        g_free(candidates[i]);
    }
    g_free(exe);
    g_free(bindir);
    return found;
}

/* ---------- satu instance saja ---------- */

typedef gboolean (*ProcessMatch)(char **argv, int argc, gpointer data);

/* argv proses milik pengguna ini; NULL bila bukan milik kita atau sudah jadi zombie. */
static char **process_argv(pid_t pid, int *argc)
{
    char path[64];
    struct stat st;
    g_snprintf(path, sizeof path, "/proc/%d", (int) pid);
    if (stat(path, &st) != 0 || st.st_uid != getuid())
        return NULL;

    g_strlcat(path, "/cmdline", sizeof path);
    char *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL) || len == 0) {
        g_free(data);
        return NULL;
    }
    GPtrArray *args = g_ptr_array_new();
    for (gsize i = 0; i < len; i += strlen(data + i) + 1)
        g_ptr_array_add(args, g_strdup(data + i));
    *argc = args->len;
    g_ptr_array_add(args, NULL);
    g_free(data);
    return (char **) g_ptr_array_free(args, FALSE);
}

static gboolean is_program(const char *arg0, const char *name)
{
    char *base = g_path_get_basename(arg0);
    gboolean same = g_str_equal(base, name);
    g_free(base);
    return same;
}

static gboolean match_share(char **argv, int argc, gpointer data)
{
    return argc >= 2 && is_program(argv[0], "tsunagupad") && g_str_equal(argv[1], "share");
}

static gboolean match_receiver(char **argv, int argc, gpointer name)
{
    if (argc >= 2 && is_program(argv[0], "tsunagupad") && g_str_equal(argv[1], "receive"))
        return TRUE;
    if (argc < 1 || !is_program(argv[0], "uxplay"))
        return FALSE;
    for (int i = 1; i + 1 < argc; i++)
        if (g_str_equal(argv[i], "-n") && g_str_equal(argv[i + 1], name))
            return TRUE;
    return FALSE;
}

static gboolean still_matches(pid_t pid, ProcessMatch match, gpointer data)
{
    int argc = 0;
    char **argv = process_argv(pid, &argc);
    gboolean alive = argv && match(argv, argc, data);
    g_strfreev(argv);
    return alive;
}

/* Setelah GNOME Shell dimuat ulang (Alt+F2 r), proses lama tidak ikut mati dan
 * masih memegang port (8765 / AirPlay). Hentikan dulu sebelum mulai. */
static void stop_stale(ProcessMatch match, gpointer data)
{
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(pid_t));
    GDir *dir = g_dir_open("/proc", 0, NULL);
    const char *name;

    while (dir && (name = g_dir_read_name(dir))) {
        char *end;
        long value = strtol(name, &end, 10);
        pid_t pid = (pid_t) value;
        if (*end || pid <= 1 || pid == getpid())
            continue;
        if (still_matches(pid, match, data)) {
            kill(pid, SIGTERM);
            g_array_append_val(pids, pid);
        }
    }
    if (dir)
        g_dir_close(dir);

    for (int tries = 0; tries < 30 && pids->len > 0; tries++) {
        g_usleep(100 * 1000);
        for (guint i = pids->len; i-- > 0;)
            if (!still_matches(g_array_index(pids, pid_t, i), match, data))
                g_array_remove_index(pids, i);
    }
    for (guint i = 0; i < pids->len; i++) {
        pid_t pid = g_array_index(pids, pid_t, i);
        if (still_matches(pid, match, data))
            kill(pid, SIGKILL);
    }
    g_array_free(pids, TRUE);
}

/* ---------- perintah ---------- */

static gboolean on_quit_signal(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}

static int fail(const char *message)
{
    tsunagu_event("error", "message", message, NULL);
    g_printerr("tsunagupad: %s\n", message);
    return 1;
}

/* Utamakan UxPlay yang dipasang pengguna sendiri. Ini memungkinkan perbaikan
 * upstream dipakai tanpa menimpa paket Ubuntu di /usr/bin. */
static char *find_uxplay(void)
{
    char *local = g_build_filename(g_get_home_dir(), ".local", "bin", "uxplay", NULL);
    if (g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    return g_find_program_in_path("uxplay");
}

static int cmd_share(int argc, char **argv)
{
    App app = { 0 };
    int port = 8765;
    char *region = NULL, *web_dir = NULL;
    gboolean no_input = FALSE, reset_token = FALSE;
    app.quality = "lancar";
    app.encoder = "auto";
    app.pen_mode = "auto";

    GOptionEntry entries[] = {
        { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port HTTP (bawaan 8765)", "PORT" },
        { "region", 'r', 0, G_OPTION_ARG_STRING, &region, "Area layar yang dibagikan", "X,Y,W,H" },
        { "quality", 'q', 0, G_OPTION_ARG_STRING, &app.quality, "hemat | lancar | seimbang | tajam", "Q" },
        { "encoder", 'e', 0, G_OPTION_ARG_STRING, &app.encoder, "auto | nvidia | intel-lp | intel | cpu", "ENC" },
        { "pen", 0, 0, G_OPTION_ARG_STRING, &app.pen_mode, "Apple Pencil: auto | tablet | mouse", "MODE" },
        { "no-input", 0, 0, G_OPTION_ARG_NONE, &no_input, "iPad hanya menonton, tanpa kontrol", NULL },
        { "test-source", 0, 0, G_OPTION_ARG_NONE, &app.test_source, "Pola uji, bukan layar asli", NULL },
        { "web-dir", 0, 0, G_OPTION_ARG_FILENAME, &web_dir, "Folder halaman web iPad", "DIR" },
        { "reset-token", 0, 0, G_OPTION_ARG_NONE, &reset_token, "Buat kode akses baru", NULL },
        { NULL },
    };
    GOptionContext *ctx = g_option_context_new("- bagikan layar Ubuntu ke iPad");
    g_option_context_add_main_entries(ctx, entries, NULL);
    GError *err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &err))
        return fail(err->message);
    g_option_context_free(ctx);

    gst_init(NULL, NULL);
    app.port = port;
    app.allow_input = !no_input;

    int preset = -1;
    for (guint i = 0; i < G_N_ELEMENTS(presets); i++)
        if (g_str_equal(presets[i].name, app.quality))
            preset = i;
    if (preset < 0)
        return fail("Kualitas tidak dikenal (pilih hemat, lancar, seimbang, atau tajam)");

    if (region) {
        if (sscanf(region, "%d,%d,%d,%d", &app.rx, &app.ry, &app.rw, &app.rh) != 4 || app.rw < 16 || app.rh < 16)
            return fail("Format --region harus X,Y,LEBAR,TINGGI");
    } else if (app.test_source) {
        app.rw = 1920;
        app.rh = 1080;
    } else {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy)
            return fail("Tidak bisa membuka display X11. Tsunagu-Pad saat ini butuh sesi \"Ubuntu on Xorg\".");
        app.rw = DisplayWidth(dpy, DefaultScreen(dpy));
        app.rh = DisplayHeight(dpy, DefaultScreen(dpy));
        XCloseDisplay(dpy);
    }

    app.fps = presets[preset].fps;
    app.kbps = presets[preset].kbps;
    app.out_w = app.rw;
    app.out_h = app.rh;
    if (presets[preset].max_w && app.rw > presets[preset].max_w) {
        app.out_w = presets[preset].max_w;
        app.out_h = (int) ((gint64) app.rh * presets[preset].max_w / app.rw);
    }
    app.out_w &= ~1;
    app.out_h &= ~1;

    stop_stale(match_share, NULL);

    app.enc_name = encoder_pick(app.encoder);
    if (!app.enc_name)
        return fail("Tidak ada encoder H.264 yang jalan. Jalankan install.sh untuk memasang plugin GStreamer.");

    app.web_dir = find_web_dir(web_dir);
    if (!app.web_dir)
        return fail("Folder web Tsunagu-Pad tidak ditemukan (pakai --web-dir)");
    app.token = load_token(reset_token);

    if (app.allow_input)
        app.input = input_new(&app);

    app.loop = g_main_loop_new(NULL, FALSE);
    if (!server_start(&app, &err))
        return fail(err->message);

    GPtrArray *addrs = local_addresses();
    if (addrs->len == 0)
        g_ptr_array_add(addrs, g_strdup("127.0.0.1"));
    GString *urls = g_string_new(NULL);
    char *first_url = NULL;
    for (guint i = 0; i < addrs->len; i++) {
        char *url = g_strdup_printf("http://%s:%u/?k=%s", (char *) addrs->pdata[i], app.port, app.token);
        if (i)
            g_string_append_c(urls, ' ');
        g_string_append(urls, url);
        if (!first_url)
            first_url = g_strdup(url);
        g_free(url);
    }
    char *qr = qr_rows(first_url);
    char *size = g_strdup_printf("%dx%d", app.out_w, app.out_h);
    char *fps = g_strdup_printf("%d", app.fps);
    tsunagu_event("ready", "urls", urls->str, "qr", qr, "encoder", app.enc_name, "size", size,
               "fps", fps, "quality", app.quality, NULL);
    g_printerr("Tsunagu-Pad siap. Buka di Safari iPad: %s\n", first_url);

    g_unix_signal_add(SIGINT, on_quit_signal, app.loop);
    g_unix_signal_add(SIGTERM, on_quit_signal, app.loop);
    g_main_loop_run(app.loop);

    server_stop(&app);
    share_stop(&app);
    input_free(app.input);
    g_main_loop_unref(app.loop);
    g_ptr_array_unref(addrs);
    g_string_free(urls, TRUE);
    g_free(first_url);
    g_free(qr);
    g_free(size);
    g_free(fps);
    g_free(app.token);
    g_free(app.web_dir);
    return 0;
}

/* iPad -> Ubuntu: jalankan UxPlay (penerima AirPlay open source, GPL) dengan
 * pengaturan latensi rendah. Port tetap (-p) supaya mudah dibuka di firewall. */
static int cmd_receive(int argc, char **argv)
{
    char *name = "Tsunagu-Pad Ubuntu";
    int fps = 60;
    gboolean fullscreen = FALSE, pin = FALSE, no_audio = FALSE;

    GOptionEntry entries[] = {
        { "name", 'n', 0, G_OPTION_ARG_STRING, &name, "Nama yang muncul di iPad", "NAMA" },
        { "fps", 0, 0, G_OPTION_ARG_INT, &fps, "Frame per detik maksimum (bawaan 60)", "N" },
        { "fullscreen", 'f', 0, G_OPTION_ARG_NONE, &fullscreen, "Tampilkan layar penuh", NULL },
        { "pin", 0, 0, G_OPTION_ARG_NONE, &pin, "Minta PIN 4 digit saat menyambung", NULL },
        { "no-audio", 0, 0, G_OPTION_ARG_NONE, &no_audio, "Tanpa suara", NULL },
        { NULL },
    };
    GOptionContext *ctx = g_option_context_new("- terima layar iPad lewat AirPlay");
    g_option_context_add_main_entries(ctx, entries, NULL);
    GError *err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &err))
        return fail(err->message);
    g_option_context_free(ctx);

    char *uxplay = find_uxplay();
    if (!uxplay)
        return fail("UxPlay belum terpasang. Jalankan ./install.sh (atau: sudo apt install uxplay)");

    stop_stale(match_receiver, name);

    GPtrArray *args = g_ptr_array_new();
    char *stdbuf = g_find_program_in_path("stdbuf");
    if (stdbuf) {
        /* UxPlay menahan log bila keluarannya pipe; stdbuf membuatnya per baris. */
        g_ptr_array_add(args, stdbuf);
        g_ptr_array_add(args, "-oL");
        g_ptr_array_add(args, "-eL");
    }
    char *fps_str = g_strdup_printf("%d", CLAMP(fps, 10, 60));
    g_ptr_array_add(args, uxplay);
    g_ptr_array_add(args, "-n");
    g_ptr_array_add(args, name);
    g_ptr_array_add(args, "-nh");
    g_ptr_array_add(args, "-p");
    g_ptr_array_add(args, "-fps");
    g_ptr_array_add(args, fps_str);
    g_ptr_array_add(args, "-vsync");
    g_ptr_array_add(args, "no");
    g_ptr_array_add(args, "-nohold");
    /* iPad kadang menghentikan paket sebentar saat layar dikunci atau pindah
     * aplikasi. Jangan anggap jeda itu sebagai sesi mati. Jendela juga tetap
     * ada agar sambungan berikutnya langsung terlihat. */
    g_ptr_array_add(args, "-reset");
    g_ptr_array_add(args, "0");
    g_ptr_array_add(args, "-nc");
    /* Di X11, XVideo memberi jendela terkelola GNOME yang lebih andal daripada
     * overlay OpenGL/VAAPI. Decoder software juga menghindari driver Intel
     * yang kadang berhasil menerima stream tetapi gagal mempresentasikan frame. */
    g_ptr_array_add(args, "-avdec");
    g_ptr_array_add(args, "-vs");
    g_ptr_array_add(args, "xvimagesink");
    if (fullscreen)
        g_ptr_array_add(args, "-fs");
    if (pin)
        g_ptr_array_add(args, "-pin");
    if (no_audio)
        g_ptr_array_add(args, "-a");
    g_ptr_array_add(args, NULL);

    tsunagu_event("receiver", "state", "starting", "name", name, NULL);
    execv(args->pdata[0], (char **) args->pdata);
    return fail(g_strerror(errno));
}

static int cmd_probe(void)
{
    gst_init(NULL, NULL);
    const char *enc = encoder_pick("auto");
    char *uxplay = find_uxplay();
    tsunagu_event("probe", "encoder", enc ? enc : "", "uxplay", uxplay ? "yes" : "no", NULL);
    g_free(uxplay);
    return enc ? 0 : 1;
}

/* Dipakai panel untuk QR WiFi hotspot. */
static int cmd_qr(int argc, char **argv)
{
    if (argc < 2)
        return fail("Pemakaian: tsunagupad qr TEKS");
    char *rows = qr_rows(argv[1]);
    if (!rows)
        return fail("Teks terlalu panjang untuk QR");
    tsunagu_event("qr", "rows", rows, NULL);
    g_free(rows);
    return 0;
}

static void usage(void)
{
    g_print("Tsunagu-Pad %s, satukan layar Ubuntu dan iPad\n\n"
            "Pemakaian:\n"
            "  tsunagupad share   [--region X,Y,W,H] [--quality Q] [--encoder E] ...\n"
            "  tsunagupad receive [--name NAMA] [--fullscreen] ...\n"
            "  tsunagupad probe\n"
            "  tsunagupad qr TEKS\n\n"
            "Tambahkan --help setelah perintah untuk semua opsi.\n", TSUNAGUPAD_VERSION);
}

int main(int argc, char **argv)
{
    /* Ikut berhenti bila GNOME Shell (induk proses) keluar. */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];
    if (g_str_equal(cmd, "share"))
        return cmd_share(argc - 1, argv + 1);
    if (g_str_equal(cmd, "receive"))
        return cmd_receive(argc - 1, argv + 1);
    if (g_str_equal(cmd, "probe"))
        return cmd_probe();
    if (g_str_equal(cmd, "qr"))
        return cmd_qr(argc - 1, argv + 1);
    if (g_str_equal(cmd, "--version") || g_str_equal(cmd, "version")) {
        g_print("Tsunagu-Pad %s\n", TSUNAGUPAD_VERSION);
        return 0;
    }
    usage();
    return g_str_equal(cmd, "--help") || g_str_equal(cmd, "-h") ? 0 : 1;
}
