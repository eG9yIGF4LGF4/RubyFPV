# Makefile for ruby_player_gst
# Requires: gtk4, gstreamer-1.0, gstreamer-video-1.0

TARGET  := ruby_player_gst
SRCS    := ruby_player_gst.c
CC      := gcc

PKG_DEPS := gtk4 gstreamer-1.0 gstreamer-video-1.0

CFLAGS  := -Wall -Wextra -Wpedantic -O2 -Wno-deprecated-declarations $(shell pkg-config --cflags $(PKG_DEPS))
LDFLAGS := $(shell pkg-config --libs   $(PKG_DEPS))

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRCS) ruby_player_gst.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET)        $(DESTDIR)/usr/local/bin/$(TARGET)
	install -Dm644 ruby_player.yaml $(DESTDIR)/usr/local/bin/ruby_player.yaml

run: $(TARGET)
	./$(TARGET)