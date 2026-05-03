# ruby_player_gst

A GStreamer media player with a GTK4 UI.  
Supports four source types: **Shared Memory**, **Named Pipe**, **UDP RTP**, and **File**.

---

## Project layout

```
ruby_player_gst/
├── ruby_player_gst.c      # Full application source
├── ruby_player_gst.h      # Types and declarations
├── ruby_player.yaml       # Configuration (kept next to the binary)
├── CMakeLists.txt         # CMake build
└── Makefile               # Direct Makefile build
```

---

## Dependencies

| Package              | Ubuntu / Debian                         | Fedora / RHEL              |
|----------------------|-----------------------------------------|----------------------------|
| GTK 4                | `libgtk-4-dev`                          | `gtk4-devel`               |
| GStreamer core        | `libgstreamer1.0-dev`                   | `gstreamer1-devel`         |
| GStreamer video       | `libgstreamer-plugins-base1.0-dev`      | `gstreamer1-plugins-base-devel` |
| GStreamer good/bad   | `gstreamer1.0-plugins-good` `-bad`      | `gstreamer1-plugins-good`  |
| libav (H.264/H.265)  | `gstreamer1.0-libav`                    | `gstreamer1-libav`         |

### Quick install – Debian / Ubuntu

```bash
sudo apt install \
    libgtk-4-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-libav \
    pkg-config build-essential
```

---

## Build

### With Make (simplest)

```bash
make
./ruby_player_gst
```

### With CMake

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ruby_player_gst
```

The binary looks for `ruby_player.yaml` in the **same directory** as itself,
or accepts an explicit path as the first argument:

```bash
./ruby_player_gst /etc/myconfig.yaml
```

---

## Configuration – `ruby_player.yaml`

Edit the `source` block to select your source:

```yaml
source:
  type: file      # shm | pipe | udp | file
  autoplay: false
  loop: false
```

### Source types

#### `file` – Local media file
```yaml
source:
  type: file
  file:
    path: "/home/user/video.mp4"
```
Uses `playbin` internally; supports mp4, mkv, avi, ts, webm, mov, and more.

#### `udp` – UDP RTP stream (e.g. from a drone GCS)
```yaml
source:
  type: udp
  udp:
    host: "0.0.0.0"
    port: 5600
    encoding: "h264"    # h264 | h265 | mjpeg | raw
    latency_ms: 200
```
Test sender:
```bash
gst-launch-1.0 videotestsrc ! x264enc ! rtph264pay ! udpsink host=127.0.0.1 port=5600
```

#### `shm` – Shared Memory (zero-copy IPC)
```yaml
source:
  type: shm
  shm:
    path: "/tmp/ruby_shm_video"
    socket_path: "/tmp/ruby_shm.sock"
    format: "BGRx"
    width: 1920
    height: 1080
    framerate: 30
```
Compatible with OpenCV `shmsink` or any process that writes raw frames to shm.

#### `pipe` – Named Pipe / FIFO
```yaml
source:
  type: pipe
  pipe:
    path: "/tmp/ruby_video.pipe"
    width: 1280
    height: 720
    framerate: 30
```
Feed it with:
```bash
mkfifo /tmp/ruby_video.pipe
gst-launch-1.0 videotestsrc ! video/x-raw,width=1280,height=720 \
    ! filesink location=/tmp/ruby_video.pipe
```

---

## UI – Sidebar controls

| Control            | Description                                      |
|--------------------|--------------------------------------------------|
| Source dropdown    | Switch between SHM / Pipe / UDP / File           |
| **Open File…**     | File picker (sets path and switches to File mode) |
| **▶ Play / ⏸ Pause** | Toggle playback                                |
| **⏹ Stop**         | Stop and release the pipeline                    |
| Volume slider      | Adjust audio volume (0–100 %)                    |
| Status area        | Shows pipeline errors in red                     |
| **↺ Reload Config**| Re-read YAML without restarting the app          |

---

## GStreamer debug

```bash
GST_DEBUG=3 ./ruby_player_gst
```

Set `logging.gst_debug` in the YAML to control verbosity (1 = errors only, 9 = trace).
