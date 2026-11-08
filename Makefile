# cctv-overlay — Milestone 2 headless core
PREFIX ?= /usr/local
DESTDIR ?=
CC ?= cc

PKGS     := glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wformat=2 -Isrc $(shell pkg-config --cflags $(PKGS)) -MMD -MP
LDLIBS   := $(shell pkg-config --libs $(PKGS))

# Swap these two objects for the real GTK/mpv/layer-shell implementations later.
VIDEO_OBJ   := build/video_stub.o
OVERLAY_OBJ := build/overlay_stub.o

CORE_OBJ := build/util.o build/logging.o build/config.o build/camera.o build/app.o build/ipc.o
BIN      := cctv-overlay
TESTS    := build/tests/test_util build/tests/test_logging build/tests/test_config \
            build/tests/test_camera build/tests/test_app

.PHONY: all clean check install uninstall
all: $(BIN)

$(BIN): build/main.o $(CORE_OBJ) $(VIDEO_OBJ) $(OVERLAY_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%.o: tests/%.c
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%: build/tests/%.o $(CORE_OBJ) $(VIDEO_OBJ) $(OVERLAY_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

.SECONDARY:

check: $(BIN) $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; env -u CCTV_OVERLAY_PASSWORD -u CCTV_OVERLAY_USERNAME $$t; done
	@echo "== tests/test_ipc.sh"
	@bash tests/test_ipc.sh ./$(BIN)

# Installs the binary and documentation/examples only. It never touches ~/.config.
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 examples/config.example.ini $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/config.example.ini
	install -Dm644 examples/secrets.example.ini $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/secrets.example.ini
	install -Dm644 docs/behaviour.md $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay/behaviour.md

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/cctv-overlay

clean:
	rm -rf build $(BIN)

-include $(wildcard build/*.d build/tests/*.d)
