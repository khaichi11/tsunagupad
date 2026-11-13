/* Tsunagu-Pad, satukan layar komputer (Ubuntu atau Windows) dan iPad.
 *
 *   tsunagupad share    Layar komputer -> iPad (Safari, WebRTC) + kontrol sentuh/Pencil
 *   tsunagupad receive  Layar iPad -> komputer (AirPlay, lewat UxPlay)
 *   tsunagupad probe    Cek encoder video yang tersedia
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <glib-unix.h>
#endif

#include "tsunagupad.h"
#ifdef _WIN32
#include "win/win.h"
#endif

static const struct {
    const char *name;
    int max_w, fps, kbps;
} presets[] = {
    { "hemat", 960, 30, 1800 },      /* jaringan lemah / tethering Bluetooth */
    { "lancar", 1280, 60, 5000 },    /* bawaan: responsif untuk Wi-Fi/Tailscale */
    { "seimbang", 1600, 60, 8000 },
    { "tajam", 0, 60, 14000 },       /* resolusi asli */
    { "monitor", 1920, 30, 12000 },  /* pengganti monitor: 1080p tanpa skala agar tulisan tajam */
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
#ifdef G_OS_WIN32
    if (!win_random(raw, sizeof raw))
        g_error("Bilangan acak dari Windows gagal dibuat");
#else
    if (getrandom(raw, sizeof raw, 0) != sizeof raw)
        g_error("getrandom gagal: %s", g_strerror(errno));
#endif
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

#ifndef G_OS_WIN32

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

#else /* G_OS_WIN32 */

/* Windows: alamat dan folder web dicari lewat API Windows (src/win/sys.c). */
static GPtrArray *local_addresses(void)
{
    return win_local_addresses();
}

static char *find_web_dir(const char *given)
{
    return win_find_web_dir(given);
}

#endif /* G_OS_WIN32 */

/* ---------- perintah ---------- */

#ifndef G_OS_WIN32
static gboolean on_quit_signal(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}
#endif

#ifndef G_OS_WIN32
/* Area tangkap dihitung sekali saat mulai. Kalau ukuran layar X berubah,
 * misalnya monitor baru dicolok setelah sesi berjalan, area lama membuat iPad
 * hanya menampilkan pojok layar. Proses keluar dengan kode gagal supaya
 * systemd menyalakannya lagi dengan ukuran yang benar. */
typedef struct {
    Display *dpy;
    Window root;
    int width, height;
    gboolean changed;
    GMainLoop *loop;
} ScreenWatch;

static gboolean screen_watch_tick(gpointer data)
{
    ScreenWatch *w = data;
    while (XPending(w->dpy)) {
        XEvent ev;
        XNextEvent(w->dpy, &ev);
        if (ev.type == ConfigureNotify && ev.xconfigure.window == w->root &&
            (ev.xconfigure.width != w->width || ev.xconfigure.height != w->height)) {
            w->changed = TRUE;
            g_main_loop_quit(w->loop);
            return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}

#endif

static int fail(const char *message)
{
    tsunagu_event("error", "message", message, NULL);
    g_printerr("tsunagupad: %s\n", message);
    return 1;
}

#ifndef G_OS_WIN32
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

#endif

static int cmd_share(int argc, char **argv)
{
    App app = { 0 };
    int port = 8765;
#ifdef G_OS_WIN32
    gboolean screen_changed = FALSE;
    char *monitor = NULL;
#else
    static ScreenWatch screen_watch;
#endif
    char *region = NULL, *second = NULL, *cursor = NULL, *web_dir = NULL, *scale = NULL;
    gboolean no_input = FALSE, reset_token = FALSE;
    app.quality = "lancar";
    app.encoder = "auto";
    app.pen_mode = "auto";
    app.backend = "auto";

    GOptionEntry entries[] = {
        { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port HTTP (bawaan 8765)", "PORT" },
        { "region", 'r', 0, G_OPTION_ARG_STRING, &region, "Area layar yang dibagikan", "X,Y,W,H" },
        { "cursor", 0, 0, G_OPTION_ARG_STRING, &cursor, "Kursor layar kedua: sistem (bawaan) | lokal", "MODE" },
        { "scale", 0, 0, G_OPTION_ARG_STRING, &scale, "Skala layar kedua: 1 (bawaan), 1.25, 1.5, 2, atau ikut", "S" },
#ifdef G_OS_WIN32
        { "second", 0, 0, G_OPTION_ARG_STRING, &second, "iPad jadi layar kedua, mis. 1280x800", "WxH" },
        { "monitor", 'm', 0, G_OPTION_ARG_STRING, &monitor,
          "Monitor yang dibagikan: primary (bawaan), nomor urut, atau nama seperti \\\\.\\DISPLAY2", "MON" },
#else
        { "second", 0, 0, G_OPTION_ARG_STRING, &second, "iPad jadi layar kedua, mis. 1280x800 (Wayland)", "WxH" },
#endif
        { "quality", 'q', 0, G_OPTION_ARG_STRING, &app.quality, "hemat | lancar | seimbang | tajam | monitor", "Q" },
#ifdef G_OS_WIN32
        { "encoder", 'e', 0, G_OPTION_ARG_STRING, &app.encoder, "auto | nvidia | intel | amd | mf | cpu | cpu-sw", "ENC" },
#else
        { "encoder", 'e', 0, G_OPTION_ARG_STRING, &app.encoder, "auto | nvidia | intel-lp | intel | tegra | vic-x264 | cpu", "ENC" },
#endif
        { "pen", 0, 0, G_OPTION_ARG_STRING, &app.pen_mode, "Apple Pencil: auto | tablet | mouse", "MODE" },
#ifdef G_OS_WIN32
        { "backend", 0, 0, G_OPTION_ARG_STRING, &app.backend, "Tangkap layar: auto | dxgi", "B" },
#else
        { "backend", 0, 0, G_OPTION_ARG_STRING, &app.backend, "Tangkap layar: auto | x11 | portal (Wayland)", "B" },
#endif
        { "no-input", 0, 0, G_OPTION_ARG_NONE, &no_input, "iPad hanya menonton, tanpa kontrol", NULL },
        { "allow-public", 0, 0, G_OPTION_ARG_NONE, &app.allow_public,
          "Terima iPad dari alamat publik (bawaan: hanya jaringan lokal dan Tailscale)", NULL },
        { "test-source", 0, 0, G_OPTION_ARG_NONE, &app.test_source, "Pola uji, bukan layar asli", NULL },
        { "web-dir", 0, 0, G_OPTION_ARG_FILENAME, &web_dir, "Folder halaman web iPad", "DIR" },
        { "reset-token", 0, 0, G_OPTION_ARG_NONE, &reset_token, "Buat kode akses baru", NULL },
        { "pick-screen", 0, 0, G_OPTION_ARG_NONE, &app.pick_screen,
          "Tanya lagi layar mana yang dibagikan (Wayland)", NULL },
        { NULL },
    };
    GOptionContext *ctx = g_option_context_new("- bagikan layar komputer ke iPad");
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
        return fail("Kualitas tidak dikenal (pilih hemat, lancar, seimbang, tajam, atau monitor)");

#ifdef G_OS_WIN32
    if (!g_str_equal(app.backend, "auto") && !g_str_equal(app.backend, "dxgi"))
        return fail("Backend tidak dikenal (di Windows pilih auto atau dxgi)");

    /* Proses lama yang masih memegang port dihentikan dulu, lalu encoder dicek
     * sebelum monitor virtual dipasang, supaya tata letak layar tidak berubah
     * percuma bila ternyata tidak ada encoder yang jalan. */
    win_stop_stale_share(app.port);
    app.enc_name = encoder_pick(app.encoder);
    if (!app.enc_name)
        return fail("Tidak ada encoder H.264 yang jalan. Pasang ulang Tsunagu-Pad untuk memulihkan plugin GStreamer.");
    if (!app.test_source) {
        GstElementFactory *dxgi = gst_element_factory_find("d3d11screencapturesrc");
        if (!dxgi)
            return fail("Plugin GStreamer untuk menangkap layar (d3d11screencapturesrc) tidak ditemukan. Pasang ulang Tsunagu-Pad.");
        gst_object_unref(dxgi);
    }

    if (second) {
        int vw = 0, vh = 0;
        if (sscanf(second, "%dx%d", &vw, &vh) != 2 || vw < 320 || vh < 240)
            return fail("Format --second harus LEBARxTINGGI, mis. 1280x800");
        /* Skala menentukan luas ruang kerja di iPad, sama seperti di Ubuntu:
         * 1 berarti ukuran yang diminta itu juga ukuran desktopnya, "ikut"
         * menyamakan dengan monitor utama. */
        double vscale = 1.0;
        if (scale && g_str_equal(scale, "ikut"))
            vscale = 0;
        else if (scale) {
            char *endp = NULL;
            vscale = g_ascii_strtod(scale, &endp);
            if (endp == scale || vscale < 0.5 || vscale > 4)
                return fail("Nilai --scale harus antara 0.5 dan 4, atau kata ikut");
        }
        GError *verr = NULL;
        app.vdisp = vdisp_open(vw & ~1, vh & ~1, vscale, &verr);
        if (!app.vdisp)
            return fail(verr ? verr->message : "Monitor virtual gagal dibuat");
        WinMonitor vm;
        if (!win_monitor_by_device(vdisp_device(app.vdisp), &vm)) {
            vdisp_close(app.vdisp);
            return fail("Monitor virtual tidak muncul di Windows");
        }
        app.monitor = g_strdup(vm.device);
        app.rx = vm.x;
        app.ry = vm.y;
        app.rw = vm.width;
        app.rh = vm.height;
        /* Kursor asli dari Windows ikut digambar ke video; "lokal" mematikannya. */
        app.hide_cursor = g_strcmp0(cursor, "lokal") == 0;
        tsunagu_event("second", "state", "placed", "monitor", vm.device, NULL);
    } else if (app.test_source) {
        app.rw = 1920;
        app.rh = 1080;
    } else {
        WinMonitor mon;
        int qx = 0, qy = 0, qw = 0, qh = 0;
        if (region) {
            if (sscanf(region, "%d,%d,%d,%d", &qx, &qy, &qw, &qh) != 4 || qw < 16 || qh < 16)
                return fail("Format --region harus X,Y,LEBAR,TINGGI");
            /* Area dipotong dari monitor yang memuat sudut kiri atasnya. */
            char *spec = g_strdup_printf("@%d,%d", qx, qy);
            gboolean found = win_monitor_find(spec, &mon);
            g_free(spec);
            if (!found)
                return fail("Tidak ada monitor di titik --region itu");
        } else if (!win_monitor_find(monitor, &mon)) {
            return fail(monitor ? "Monitor yang diminta tidak ditemukan. Lihat daftarnya dengan: tsunagupad monitors"
                                : "Tidak ada monitor aktif untuk dibagikan");
        }
        app.monitor = g_strdup(mon.device);
        if (region) {
            int x0 = CLAMP(qx, mon.x, mon.x + mon.width - 16);
            int y0 = CLAMP(qy, mon.y, mon.y + mon.height - 16);
            app.rw = MIN(qw, mon.x + mon.width - x0);
            app.rh = MIN(qh, mon.y + mon.height - y0);
            app.rx = x0;
            app.ry = y0;
            app.crop = TRUE;
            app.crop_x = x0 - mon.x;
            app.crop_y = y0 - mon.y;
        } else {
            app.rx = mon.x;
            app.ry = mon.y;
            app.rw = mon.width;
            app.rh = mon.height;
        }
    }
#else
    if (!g_str_equal(app.backend, "auto") && !g_str_equal(app.backend, "x11") && !g_str_equal(app.backend, "portal"))
        return fail("Backend tidak dikenal (pilih auto, x11, atau portal)");
    gboolean use_portal = g_str_equal(app.backend, "portal") ||
        (g_str_equal(app.backend, "auto") && !app.test_source && !region &&
         g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0);

    /* Layar kedua memakai monitor virtual kompositor Wayland. Di Xorg jalur itu
     * tidak ada; dulu opsinya diam-diam diabaikan dan pengguna mengira layar
     * keduanya gagal, jadi sekarang ditolak dengan jelas. */
    if (second && !use_portal)
        return fail("Layar kedua hanya ada di sesi Wayland. Sesi ini Xorg: keluar dan "
                    "masuk lagi memilih Ubuntu on Wayland, atau jalankan tanpa --second "
                    "untuk membagikan layar yang ada.");

    /* Encoder dicek dulu supaya jendela izin GNOME tidak muncul bila ternyata tidak bisa jalan. */
    stop_stale(match_share, NULL);
    app.enc_name = encoder_pick(app.encoder);
    if (!app.enc_name)
        return fail("Tidak ada encoder H.264 yang jalan. Jalankan install.sh untuk memasang plugin GStreamer.");

    if (use_portal) {
        GstElementFactory *pw = gst_element_factory_find("pipewiresrc");
        if (!pw)
            return fail("Plugin GStreamer untuk PipeWire belum terpasang. Jalankan install.sh (atau: sudo apt install gstreamer1.0-pipewire)");
        gst_object_unref(pw);
        GError *perr = NULL;
        if (second) {
            if (!virtmon_available())
                return fail("Layar kedua hanya tersedia di sesi Wayland GNOME. "
                            "Di mesin tanpa layar (misalnya Jetson), jalankan dulu sesi "
                            "GNOME-nya, lalu jalankan perintah ini di dalam sesi itu.");
            int vw = 0, vh = 0;
            if (sscanf(second, "%dx%d", &vw, &vh) != 2 || vw < 320 || vh < 240)
                return fail("Format --second harus LEBARxTINGGI, mis. 1280x800");
            GError *verr = NULL;
            /* Kursor asli dari laptop ikut ditampilkan. Mutter mengirim posisi
             * dan gambarnya lewat metadata, lalu ditempel ke tiap frame. Mode
             * "lokal" mematikannya dan iPad menggambar penunjuknya sendiri. */
            /* Skala menentukan luas ruang kerja di iPad: pada skala 1 ukuran
             * yang diminta itu juga ukuran desktopnya, pada skala 1,5 ruangnya
             * dua pertiga tetapi tulisannya lebih besar. "ikut" menyamakan
             * dengan monitor utama. */
            double vscale = 1.0;
            if (scale && g_str_equal(scale, "ikut"))
                vscale = 0;
            else if (scale) {
                char *endp = NULL;
                vscale = g_ascii_strtod(scale, &endp);
                if (endp == scale || vscale < 0.5 || vscale > 4)
                    return fail("Nilai --scale harus antara 0.5 dan 4, atau kata ikut");
            }
            app.virtmon = virtmon_open(vw & ~1, vh & ~1,
                                       g_strcmp0(cursor, "lokal") == 0, vscale, &verr);
            if (!app.virtmon)
                return fail(verr ? verr->message : "Monitor virtual gagal dibuat");
            virtmon_size(app.virtmon, &app.rw, &app.rh);
            app.rx = app.ry = 0;
        } else {
            app.portal = portal_open(&app, &perr);
            if (!app.portal)
                return fail(perr ? perr->message : "Sesi layar Wayland gagal dibuka");
            int px, py;
            portal_geometry(app.portal, &px, &py, &app.rw, &app.rh);
            app.rx = app.ry = 0;
            if (app.rw < 16 || app.rh < 16)
                return fail("Ukuran layar dari portal tidak valid");
        }
    } else if (region) {
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
        screen_watch.dpy = dpy;
        screen_watch.root = DefaultRootWindow(dpy);
        screen_watch.width = app.rw;
        screen_watch.height = app.rh;
        XSelectInput(dpy, screen_watch.root, StructureNotifyMask);
    }
#endif /* G_OS_WIN32 */

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
        /* Kode akses ditaruh di fragmen (#k=…): browser tidak pernah mengirim
         * bagian ini ke server, jadi kodenya tidak lewat jaringan. */
        char *url = g_strdup_printf("http://%s:%u/#k=%s", (char *) addrs->pdata[i], app.port, app.token);
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

#ifdef G_OS_WIN32
    win_quit_watch(app.loop);
    /* Monitor yang dibagikan bisa dicabut atau diubah resolusinya. Area lama
     * membuat iPad hanya menampilkan sebagian layar, jadi proses keluar dengan
     * kode 3 dan panel menyalakannya lagi dengan ukuran yang benar. Layar kedua
     * tidak diawasi: tata letaknya justru diubah oleh program ini sendiri. */
    if (app.monitor && !app.vdisp)
        win_screen_watch(&app, app.loop, &screen_changed);
    g_main_loop_run(app.loop);

    server_stop(&app);
    share_stop(&app);
    input_free(app.input);
    vdisp_close(app.vdisp);
    g_free(app.monitor);
#else
    g_unix_signal_add(SIGINT, on_quit_signal, app.loop);
    g_unix_signal_add(SIGTERM, on_quit_signal, app.loop);
    if (screen_watch.dpy) {
        screen_watch.loop = app.loop;
        g_timeout_add(1000, screen_watch_tick, &screen_watch);
    }
    g_main_loop_run(app.loop);

    server_stop(&app);
    share_stop(&app);
    if (screen_watch.dpy)
        XCloseDisplay(screen_watch.dpy);
    input_free(app.input);
    virtmon_close(app.virtmon);
    portal_free(app.portal);
#endif
    g_main_loop_unref(app.loop);
    g_ptr_array_unref(addrs);
    g_string_free(urls, TRUE);
    g_free(first_url);
    g_free(qr);
    g_free(size);
    g_free(fps);
    g_free(app.token);
    g_free(app.web_dir);
#ifdef G_OS_WIN32
    if (screen_changed) {
#else
    if (screen_watch.changed) {
#endif
        g_printerr("tsunagupad: ukuran layar berubah, perlu dinyalakan ulang\n");
        return 3;
    }
    return 0;
}

/* iPad -> komputer: jalankan UxPlay (penerima AirPlay open source, GPL) dengan
 * pengaturan latensi rendah. Port tetap (-p) supaya mudah dibuka di firewall. */
static int cmd_receive(int argc, char **argv)
{
#ifdef G_OS_WIN32
    char *name = "Tsunagu-Pad Windows";
#else
    char *name = "Tsunagu-Pad Ubuntu";
#endif
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

#ifdef G_OS_WIN32
    /* Windows tidak punya execv yang menggantikan proses; UxPlay dijalankan
     * sebagai anak proses dan ikut berhenti bersama proses ini. */
    return win_cmd_receive(name, fps, fullscreen, pin, no_audio);
#else
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
#endif
}

/* Cepat dan tanpa uji encoder: dipanggil panel setiap kali dimuat. */
static int cmd_capabilities(void)
{
    char *model = caps_model();
#ifdef G_OS_WIN32
    /* Di Windows ada dua kemampuan tambahan yang dipakai panel: layar kedua
     * (driver layar virtual terpasang) dan layanan untuk layar login. */
    tsunagu_event("capabilities", "model", model, "jetson", caps_is_jetson() ? "yes" : "no",
                  "hotspot", caps_hotspot(), "pencil", caps_has_uinput() ? "tablet" : "mouse",
                  "second", vdisp_available() ? "yes" : "no",
                  "service", win_service_installed() ? "yes" : "no",
                  "os", "windows", NULL);
#else
    tsunagu_event("capabilities", "model", model, "jetson", caps_is_jetson() ? "yes" : "no",
                  "hotspot", caps_hotspot(), "pencil", caps_has_uinput() ? "tablet" : "mouse", NULL);
#endif
    g_free(model);
    return 0;
}

static int cmd_probe(void)
{
    gst_init(NULL, NULL);
    const char *enc = encoder_pick("auto");
#ifdef G_OS_WIN32
    char *uxplay = win_find_uxplay();
    GstElementFactory *cap = gst_element_factory_find("d3d11screencapturesrc");
    tsunagu_event("probe", "encoder", enc ? enc : "", "uxplay", uxplay ? "yes" : "no",
                  "capture", cap ? "yes" : "no", NULL);
    if (cap)
        gst_object_unref(cap);
#else
    char *uxplay = find_uxplay();
    GstElementFactory *pw = gst_element_factory_find("pipewiresrc");
    tsunagu_event("probe", "encoder", enc ? enc : "", "uxplay", uxplay ? "yes" : "no",
                  "pipewire", pw ? "yes" : "no", NULL);
    if (pw)
        gst_object_unref(pw);
#endif
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

#ifndef G_OS_WIN32

/* ---------- kendali dari jarak jauh (SSH) ---------- */

static char *runtime_path(const char *name)
{
    const char *dir = g_get_user_runtime_dir();
    return g_build_filename(dir, name, NULL);
}

static char *session_log_path(void)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "tsunagupad", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "session.log", NULL);
    g_free(dir);
    return path;
}

/* Lewat SSH tidak ada variabel sesi grafis, padahal portal dan kompositor
 * hanya bisa dihubungi melalui bus sesi pengguna. */
static void adopt_session_env(void)
{
    const char *runtime = g_get_user_runtime_dir();
    if (!g_getenv("DBUS_SESSION_BUS_ADDRESS")) {
        char *bus = g_build_filename(runtime, "bus", NULL);
        if (g_file_test(bus, G_FILE_TEST_EXISTS)) {
            char *value = g_strdup_printf("unix:path=%s", bus);
            g_setenv("DBUS_SESSION_BUS_ADDRESS", value, TRUE);
            g_free(value);
        }
        g_free(bus);
    }
    if (!g_getenv("WAYLAND_DISPLAY")) {
        GDir *dir = g_dir_open(runtime, 0, NULL);
        const char *name;
        while (dir && (name = g_dir_read_name(dir))) {
            if (g_str_has_prefix(name, "wayland-") && !g_str_has_suffix(name, ".lock")) {
                g_setenv("WAYLAND_DISPLAY", name, TRUE);
                break;
            }
        }
        if (dir)
            g_dir_close(dir);
    }
    if (!g_getenv("DISPLAY"))
        g_setenv("DISPLAY", ":0", TRUE);
}

static pid_t running_share(void)
{
    GDir *dir = g_dir_open("/proc", 0, NULL);
    const char *name;
    pid_t found = 0;
    while (dir && (name = g_dir_read_name(dir))) {
        char *end;
        pid_t pid = (pid_t) strtol(name, &end, 10);
        if (*end || pid <= 1 || pid == getpid())
            continue;
        if (still_matches(pid, match_share, NULL)) {
            found = pid;
            break;
        }
    }
    if (dir)
        g_dir_close(dir);
    return found;
}

/* Baris "ready" terakhir dari log sesi; berisi alamat untuk iPad. */
static char *last_ready_urls(void)
{
    char *log = session_log_path();
    char *data = NULL;
    g_file_get_contents(log, &data, NULL, NULL);
    g_free(log);
    if (!data)
        return NULL;
    char *urls = NULL;
    char **lines = g_strsplit(data, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *key = strstr(lines[i], "\"urls\":\"");
        if (!key)
            continue;
        key += 8;
        const char *end = strchr(key, '"');
        if (end) {
            g_free(urls);
            urls = g_strndup(key, end - key);
        }
    }
    g_strfreev(lines);
    g_free(data);
    return urls;
}

static int cmd_status(void);

static int cmd_on(int argc, char **argv)
{
    adopt_session_env();
    if (running_share()) {
        g_print("Tsunagu-Pad sudah berjalan.\n");
        return cmd_status();
    }

    char *self = g_file_read_link("/proc/self/exe", NULL);
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, self ? self : (char *) "tsunagupad");
    g_ptr_array_add(args, "share");
    for (int i = 1; i < argc; i++)
        g_ptr_array_add(args, argv[i]);
    g_ptr_array_add(args, NULL);

    char *log = session_log_path();
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        g_free(log);
        return fail("Tidak bisa menulis log sesi");
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        g_setenv("TSUNAGUPAD_DETACHED", "1", TRUE);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        prctl(PR_SET_PDEATHSIG, 0);
        execv(args->pdata[0], (char **) args->pdata);
        _exit(127);
    }
    close(fd);
    g_ptr_array_free(args, TRUE);
    if (pid < 0) {
        g_free(log);
        return fail("Gagal menjalankan proses berbagi layar");
    }

    char *pidfile = runtime_path("tsunagupad.pid");
    char *text = g_strdup_printf("%d\n", (int) pid);
    g_file_set_contents(pidfile, text, -1, NULL);
    g_free(text);
    g_free(pidfile);

    g_print("Menunggu izin berbagi layar di layar laptop…\n");
    for (int i = 0; i < 120; i++) {
        g_usleep(500 * 1000);
        char *urls = last_ready_urls();
        if (urls) {
            g_print("Aktif. Buka di iPad:\n");
            char **list = g_strsplit(urls, " ", -1);
            for (int u = 0; list[u]; u++)
                g_print("  %s\n", list[u]);
            g_strfreev(list);
            g_free(urls);
            g_free(log);
            return 0;
        }
        if (!running_share()) {
            g_printerr("Gagal. Lihat %s\n", log);
            g_free(log);
            return 1;
        }
    }
    g_print("Belum ada jawaban. Dialog izin menunggu di layar laptop; lihat %s\n", log);
    g_free(log);
    return 0;
}

static int cmd_off(void)
{
    pid_t pid = running_share();
    if (!pid) {
        g_print("Tsunagu-Pad tidak sedang berjalan.\n");
        return 0;
    }
    stop_stale(match_share, NULL);
    g_print("Tsunagu-Pad dihentikan.\n");
    return 0;
}

static int cmd_status(void)
{
    pid_t pid = running_share();
    g_print("Berbagi layar : %s\n", pid ? "aktif" : "mati");
    if (pid) {
        char *urls = last_ready_urls();
        if (urls) {
            char **list = g_strsplit(urls, " ", -1);
            for (int u = 0; list[u]; u++)
                g_print("Alamat        : %s\n", list[u]);
            g_strfreev(list);
        }
        g_free(urls);
    }
    char *out = NULL;
    if (g_spawn_command_line_sync("systemctl is-active tsunagupad-hotspot.service",
                                  &out, NULL, NULL, NULL) && out) {
        g_strstrip(out);
        g_print("Hotspot       : %s\n", out);
    }
    g_free(out);
    return pid ? 0 : 1;
}

static int cmd_hotspot(const char *what)
{
    if (g_strcmp0(what, "on") != 0 && g_strcmp0(what, "off") != 0)
        return fail("Pemakaian: tsunagupad hotspot on|off");
    char *cmd = g_strdup_printf("systemctl %s tsunagupad-hotspot.service",
                                g_str_equal(what, "on") ? "start" : "stop");
    int status = 0;
    char *err = NULL;
    gboolean ok = g_spawn_command_line_sync(cmd, NULL, &err, &status, NULL) && status == 0;
    g_free(cmd);
    if (!ok) {
        g_printerr("%s", err && *err ? err : "Gagal mengubah hotspot.\n");
        g_printerr("Lewat SSH, polkit meminta hak administrator. Coba dengan sudo.\n");
    } else {
        g_print("Hotspot %s.\n", g_str_equal(what, "on") ? "dinyalakan" : "dimatikan");
    }
    g_free(err);
    return ok ? 0 : 1;
}

#endif /* !G_OS_WIN32 */

static void usage(void)
{
#ifdef G_OS_WIN32
    g_print("Tsunagu-Pad %s, satukan layar Windows dan iPad\n\n"
            "Pemakaian:\n"
            "  tsunagupad share   [--monitor M] [--second WxH] [--quality Q] [--encoder E] ...\n"
            "  tsunagupad receive [--name NAMA] [--fullscreen] ...\n"
            "  tsunagupad on | off | status\n"
            "  tsunagupad hotspot on|off|status|config NAMA\n"
            "  tsunagupad monitors\n"
            "  tsunagupad second-off\n"
            "  tsunagupad service install|uninstall|enable|disable|status|start|stop\n"
            "  tsunagupad probe\n"
            "  tsunagupad capabilities\n"
            "  tsunagupad qr TEKS\n\n"
            "Tambahkan --help setelah perintah untuk semua opsi.\n", TSUNAGUPAD_VERSION);
#else
    g_print("Tsunagu-Pad %s, satukan layar Ubuntu dan iPad\n\n"
            "Pemakaian:\n"
            "  tsunagupad share   [--region X,Y,W,H] [--quality Q] [--encoder E] ...\n"
            "  tsunagupad receive [--name NAMA] [--fullscreen] ...\n"
            "  tsunagupad on | off | status\n"
            "  tsunagupad hotspot on|off\n"
            "  tsunagupad probe\n"
            "  tsunagupad capabilities\n"
            "  tsunagupad qr TEKS\n\n"
            "Tambahkan --help setelah perintah untuk semua opsi.\n", TSUNAGUPAD_VERSION);
#endif
}

int main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    /* Argumen dibaca ulang sebagai UTF-8 (nama monitor, nama Wi-Fi), lalu
     * DPI, konsol, dan lokasi plugin GStreamer disiapkan. */
    argv = win_init_process(&argc, argv);
#else
    /* Ikut berhenti bila GNOME Shell (induk proses) keluar. Tidak berlaku untuk
     * sesi yang dilepas lewat "tsunagupad on": induknya memang langsung selesai
     * setelah mencetak alamat, dan tanpa pengecualian ini daemonnya ikut mati. */
    if (!g_getenv("TSUNAGUPAD_DETACHED"))
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];
    if (g_str_equal(cmd, "share"))
        return cmd_share(argc - 1, argv + 1);
    if (g_str_equal(cmd, "receive"))
        return cmd_receive(argc - 1, argv + 1);
#ifdef G_OS_WIN32
    if (g_str_equal(cmd, "on"))
        return win_cmd_on(argc - 1, argv + 1);
    if (g_str_equal(cmd, "off"))
        return win_cmd_off();
    if (g_str_equal(cmd, "status"))
        return win_cmd_status();
    if (g_str_equal(cmd, "hotspot"))
        return win_cmd_hotspot(argc - 1, argv + 1);
    if (g_str_equal(cmd, "monitors"))
        return win_cmd_monitors();
    if (g_str_equal(cmd, "second-off"))
        return win_cmd_second_off();
    if (g_str_equal(cmd, "service"))
        return win_cmd_service(argc - 1, argv + 1);
    if (g_str_equal(cmd, "setup"))
        return win_cmd_setup(argc - 1, argv + 1);
#else
    if (g_str_equal(cmd, "on"))
        return cmd_on(argc - 1, argv + 1);
    if (g_str_equal(cmd, "off"))
        return cmd_off();
    if (g_str_equal(cmd, "status"))
        return cmd_status();
    if (g_str_equal(cmd, "hotspot"))
        return cmd_hotspot(argc > 2 ? argv[2] : "");
#endif
    if (g_str_equal(cmd, "probe"))
        return cmd_probe();
    if (g_str_equal(cmd, "capabilities"))
        return cmd_capabilities();
    if (g_str_equal(cmd, "qr"))
        return cmd_qr(argc - 1, argv + 1);
    if (g_str_equal(cmd, "--version") || g_str_equal(cmd, "version")) {
        g_print("Tsunagu-Pad %s\n", TSUNAGUPAD_VERSION);
        return 0;
    }
    usage();
    return g_str_equal(cmd, "--help") || g_str_equal(cmd, "-h") ? 0 : 1;
}
