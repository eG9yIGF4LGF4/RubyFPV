
CC      = gcc
TARGET  = shm_player
SRC     = shm_player.c
 
CFLAGS  = -Wall -Wextra -O2 \
          $(shell pkg-config --cflags gtk4 gstreamer-1.0 gstreamer-video-1.0)
 
LDFLAGS = $(shell pkg-config --libs gtk4 gstreamer-1.0 gstreamer-video-1.0)
 
all: $(TARGET)
 
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
 
clean:
	rm -f $(TARGET)
 
.PHONY: all clean
 
