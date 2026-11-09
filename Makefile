# Tsunagu-Pad, build:  make   |   pasang:  make install   (tanpa sudo, ke ~/.local)

VERSION  := 0.1.0
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
