# cctv-overlay
#
#   make              real build: GTK4 + gtk4-layer-shell overlay, libmpv video  -> ./cctv-overlay
#   make STUB=1       stub build (simulated video/overlay, no GTK/mpv needed)    -> ./cctv-overlay
#   make check        unit tests + tests/test_ipc.sh against the STUB binary (headless, no display)
#   make check-real   opt-in: tests/test_real.sh drives the real build on a Wayland session
#                     (shows the overlay briefly; needs ffmpeg, grim, jq, hyprctl)
PREFIX ?= /usr/local
DESTDIR ?=
CC ?= cc
STUB ?=

PKGS     := glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wformat=2 -Isrc $(shell pkg-config --cflags $(PKGS)) -MMD -MP
LDLIBS   := $(shell pkg-config --libs $(PKGS))

# GTK/mpv part. gtk4-layer-shell MUST come first in the link line so it is loaded before libwayland
# (otherwise windows silently stay normal toplevels). No LD_PRELOAD is needed or used.
REAL_PKGS   := gtk4 gtk4-layer-shell-0 mpv epoxy
REAL_CFLAGS := $(shell pkg-config --cflags $(REAL_PKGS))
REAL_LIBS   := $(shell pkg-config --libs gtk4-layer-shell-0 gtk4 mpv epoxy)

CORE_OBJ := build/util.o build/logging.o build/config.o build/camera.o build/app.o build/ipc.o
STUB_OBJ := build/video_stub.o build/overlay_stub.o
REAL_OBJ := build/video_mpv.o build/overlay_gtk.o

build/video_mpv.o build/overlay_gtk.o: CFLAGS += $(REAL_CFLAGS)

BIN      := cctv-overlay
STUB_BIN := build/stub/cctv-overlay
ifeq ($(STUB),1)
BIN_OBJ  := $(STUB_OBJ)
BIN_LIBS := $(LDLIBS)
else
BIN_OBJ  := $(REAL_OBJ)
BIN_LIBS := $(REAL_LIBS) $(LDLIBS)
endif

TESTS    := build/tests/test_util build/tests/test_logging build/tests/test_config \
            build/tests/test_camera build/tests/test_app

.PHONY: all clean check check-real install uninstall
all: $(BIN)

# build/variant records which flavour ./cctv-overlay was linked from; its mtime only changes when
# `make` and `make STUB=1` are switched, which forces the relink.
VARIANT := $(if $(filter 1,$(STUB)),stub,real)
ifneq ($(shell cat build/variant 2>/dev/null),$(VARIANT))
$(shell mkdir -p build && echo $(VARIANT) > build/variant)
endif

$(BIN): build/main.o $(CORE_OBJ) $(BIN_OBJ) build/variant
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(BIN_LIBS)

# Same basename as the real binary: tests/test_ipc.sh matches processes by "cctv-overlay daemon".
$(STUB_BIN): build/main.o $(CORE_OBJ) $(STUB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%.o: tests/%.c
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%: build/tests/%.o $(CORE_OBJ) $(STUB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

.SECONDARY:

check: $(STUB_BIN) $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; env -u CCTV_OVERLAY_PASSWORD -u CCTV_OVERLAY_USERNAME $$t; done
	@echo "== tests/test_ipc.sh (stub build)"
	@bash tests/test_ipc.sh ./$(STUB_BIN)

check-real: $(BIN)
	@test "$(STUB)" != 1 || { echo "check-real needs the real build (omit STUB=1)"; exit 1; }
	@bash tests/test_real.sh ./$(BIN)

# Installs the binary and documentation/examples only. It never touches ~/.config.
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 examples/config.example.ini $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/config.example.ini
	install -Dm644 examples/secrets.example.ini $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/secrets.example.ini
	install -Dm644 docs/behaviour.md $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/behaviour.md
	install -Dm644 docs/overlay.md $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/overlay.md

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay

clean:
	rm -rf build $(BIN)

-include $(wildcard build/*.d build/tests/*.d)
