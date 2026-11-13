# Tsunagu-Pad
#   Linux:    make            pasang: make install   (tanpa sudo, ke ~/.local)
#   Windows:  make            di terminal MSYS2 UCRT64 (lihat windows/build.sh untuk pemasangnya)
#   Silang:   make WINDOWS=1 CC=x86_64-w64-mingw32-gcc WINDRES=x86_64-w64-mingw32-windres PKG_CONFIG=...

VERSION  := 0.2.0

ifeq ($(OS),Windows_NT)
WINDOWS := 1
endif

ifdef WINDOWS
# ---------------------------------------------------------------- Windows ---

PKG_CONFIG ?= pkg-config
WINDRES    ?= windres
PYTHON     ?= python3

WIN_PKGS := gstreamer-1.0 gstreamer-rtp-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
            libsoup-3.0 json-glib-1.0 gio-2.0 libqrencode
WIN_DEFS := -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DTSUNAGUPAD_VERSION='"$(VERSION)"'
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(WIN_DEFS)
DAEMON_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(WIN_PKGS))
DAEMON_LIBS   := $(shell $(PKG_CONFIG) --libs $(WIN_PKGS)) \
                 -liphlpapi -lws2_32 -lbcrypt -lsetupapi -lnewdev -lwtsapi32 -ladvapi32 \
                 -lshell32 -luser32 -lgdi32 -lole32 -lcfgmgr32 -lm
TRAY_CFLAGS   := $(shell $(PKG_CONFIG) --cflags glib-2.0 json-glib-1.0) -municode
TRAY_LIBS     := $(shell $(PKG_CONFIG) --libs glib-2.0 json-glib-1.0) \
                 -liphlpapi -lshell32 -luser32 -lgdi32 -lws2_32 -ladvapi32

WIN_SRC := src/main.c src/server.c src/share.c src/encoder.c src/playout.c src/qr.c \
           $(wildcard src/win/*.c)
WIN_OBJ := $(WIN_SRC:src/%.c=build/win/%.o)
comma := ,

all: build/tsunagupad.exe build/tsunagupad-tray.exe

build/tsunagupad.exe: $(WIN_OBJ) build/win/tsunagupad.res.o
	$(CC) $(LDFLAGS) -o $@ $^ $(DAEMON_LIBS)

build/tsunagupad-tray.exe: tray/tray.c build/win/tray.res.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TRAY_CFLAGS) $(LDFLAGS) -mwindows -o $@ $^ $(TRAY_LIBS)

build/win/%.o: src/%.c src/tsunagupad.h src/webrtc-lite.h src/win/win.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DAEMON_CFLAGS) -c -o $@ $<

build/version.h: Makefile
	@mkdir -p build
	printf '#define TSUNAGUPAD_VERSION_NUM %s\n#define TSUNAGUPAD_VERSION_STR "%s"\n' \
	    '$(subst .,$(comma),$(VERSION)),0' '$(VERSION)' > $@

build/tsunagupad.ico: tool/make_icon.py
	@mkdir -p build
	$(PYTHON) tool/make_icon.py $@

build/win/%.res.o: windows/%.rc windows/%.manifest build/version.h build/tsunagupad.ico
	@mkdir -p $(dir $@)
	$(WINDRES) --include-dir=build --include-dir=windows -O coff -i $< -o $@

clean:
	rm -rf build

.PHONY: all clean

else
# ------------------------------------------------------------------ Linux ---

PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share/tsunagupad
EXT_UUID := tsunagu-pad@local
EXT_DIR  ?= $(HOME)/.local/share/gnome-shell/extensions/$(EXT_UUID)
USER_UNIT_DIR ?= $(HOME)/.config/systemd/user
AUTOSTART_UNIT := tsunagupad-share.service

PKGS     := gstreamer-1.0 gstreamer-rtp-1.0 libpipewire-0.3 libsoup-3.0 json-glib-1.0 gio-unix-2.0 libqrencode x11 xtst
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            $(shell pkg-config --cflags $(PKGS)) \
            -DTSUNAGUPAD_VERSION='"$(VERSION)"' -DTSUNAGUPAD_WEB_DIR='"$(DATADIR)/web"'
# libgstwebrtc/libgstsdp di-link langsung ke versi runtime (lihat src/webrtc-lite.h).
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -l:libgstwebrtc-1.0.so.0 -l:libgstsdp-1.0.so.0 -lm

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

all: build/tsunagupad

build/tsunagupad: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/tsunagupad.h src/webrtc-lite.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: all
	install -Dm755 build/tsunagupad "$(DESTDIR)$(BINDIR)/tsunagupad"
	install -d "$(DESTDIR)$(DATADIR)/web"
	install -m644 web/* "$(DESTDIR)$(DATADIR)/web/"
	install -d "$(DESTDIR)$(EXT_DIR)"
	cp -r extension/$(EXT_UUID)/. "$(DESTDIR)$(EXT_DIR)/"

autostart:
	install -d "$(USER_UNIT_DIR)"
	sed 's|@BINDIR@|$(BINDIR)|g' system/$(AUTOSTART_UNIT) > "$(USER_UNIT_DIR)/$(AUTOSTART_UNIT)"
	systemctl --user daemon-reload
	systemctl --user enable $(AUTOSTART_UNIT)

no-autostart:
	-systemctl --user disable --now $(AUTOSTART_UNIT)
	rm -f "$(USER_UNIT_DIR)/$(AUTOSTART_UNIT)"
	-systemctl --user daemon-reload

# Login dari iPad tanpa monitor. Jalankan di terminal sesi grafis dengan monitor tercolok.
login-setup: autostart
	sudo system/tsunagupad-login-setup enable "$(USER)" "$(DISPLAY)" "$(XAUTHORITY)"
	-gsettings reset org.gnome.desktop.screensaver lock-enabled
	-gsettings reset org.gnome.desktop.session idle-delay

login-undo:
	sudo system/tsunagupad-login-setup disable

uninstall: no-autostart
	rm -f "$(BINDIR)/tsunagupad"
	rm -rf "$(DATADIR)" "$(EXT_DIR)"

clean:
	rm -rf build

.PHONY: all install autostart no-autostart login-setup login-undo uninstall clean

endif
