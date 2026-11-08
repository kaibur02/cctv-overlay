# Disposable Makefile for the M1 feasibility spike.
# Not product build system.

CC ?= gcc
CFLAGS := -std=gnu17 -Wall -Wextra -O2 -g $(shell pkg-config --cflags gtk4 gtk4-layer-shell-0 mpv epoxy)
# gtk4-layer-shell MUST come before gtk4/wayland libs in the link order.
LDLIBS := $(shell pkg-config --libs gtk4-layer-shell-0 gtk4 mpv epoxy) -lpthread -lm

TARGET := mpv_gl

.PHONY: all clean

all: $(TARGET)

$(TARGET): mpv_gl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
