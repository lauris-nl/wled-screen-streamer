# wled-screen-streamer

A low-latency native Linux streamer that captures an X11 desktop or decodes an
RTSP network video source, scales it to a small RGB LED matrix, and sends it to
WLED using correctly fragmented DDP/UDP frames.

The screen path uses X11 MIT-SHM. The RTSP path uses FFmpeg libraries directly
in-process (`libavformat`, `libavcodec`, and `libswscale`); it never launches an
`ffmpeg` subprocess.

Default configuration:

- source: X11 screen;
- WLED host: `wled-matrix2.local`;
- protocol: DDP over UDP port 4048;
- output: 64×64 RGB24, 12,288 data bytes per frame;
- target rate: 60 fps;
- capture: monitor 0, full monitor;
- resize filter: nearest-neighbor;
- anti-aliasing: 2× supersampling;
- saturation 1.15, contrast 1.10, brightness 1.00, gamma 1.00.

## Features

- Native X11 capture through MIT-SHM.
- Native RTSP decode with selectable TCP or UDP transport.
- Automatic RTSP reconnect and interruptible network timeouts.
- Single latest-frame RTSP slot: no application-level video queue.
- Preallocated capture, RGB, AA, header, and socket buffers.
- One Linux `sendmmsg()` call for all DDP fragments in a frame.
- Full, centered-square, centered-native, and explicit crops.
- Automatic crop recalculation after XRandR resolution changes.
- Nearest-neighbor and bilinear resize filters.
- Box/area supersampling and separable Gaussian anti-aliasing.
- Fixed-point color correction with startup-generated LUTs.
- No output frame queue: missed deadlines are skipped.
- Exact test patterns and one-frame DDP header diagnostics.
- Capture, resize, color, send, FPS, skip, and CPU measurements.
- Clean shutdown through Ctrl+C, SIGINT, or SIGTERM.

## Requirements

- Linux with an X11 session for screen capture
- X11 MIT-SHM and XRandR extensions
- A C++20 compiler
- CMake 3.20 or newer
- Ninja (recommended)
- Xlib, Xext, and Xrandr development files
- FFmpeg development libraries: libavformat, libavcodec, libavutil, libswscale
- Network access to WLED UDP port 4048
- Working mDNS when using a `.local` hostname

On CachyOS or Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake ninja libx11 libxext libxrandr ffmpeg
```

## Build

From the repository root:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is written to:

```text
build/wled-screen-streamer
```

Release builds use `-O3`, `-march=native`, `-mtune=native`, and link-time
optimization (`-flto`). The resulting executable is optimized for the local CPU
and may not be ideal for older machines.

Optional local installation:

```bash
cmake --install build --prefix ~/.local
```

## Quick start

```bash
# Defaults: X11, 64x64, 60 fps, full monitor, nearest, 2x supersampling
./build/wled-screen-streamer

# Square crop without stretching or black bars
./build/wled-screen-streamer --crop-mode center-square

# Centered 128x128 source crop and true 2x2 area downsample
./build/wled-screen-streamer --crop-mode center-native

# 120 fps with timing output
./build/wled-screen-streamer --crop-mode center-square --fps 120 --benchmark

# Uncapped sender benchmark
./build/wled-screen-streamer --fps 0 --benchmark

# Native RTSP camera over TCP
./build/wled-screen-streamer \
  --source rtsp \
  --url rtsp://HOST:8554/reolink \
  --crop-mode center-square \
  --fps 20
```

Press Ctrl+C to stop.

## Command-line options

| Option | Meaning | Default |
|---|---|---|
| `--host HOST` | WLED hostname or IP address | `wled-matrix2.local` |
| `--source SOURCE` | `screen` or `rtsp` | `screen` |
| `--url URL` | Network video URL; required for RTSP | unset |
| `--rtsp-transport MODE` | RTSP over `tcp` or `udp` | `tcp` |
| `--width N` | Output width | `64` |
| `--height N` | Output height | `64` |
| `--fps N` | Maximum output FPS; `0` is uncapped | `60` |
| `--monitor N` | Active XRandR monitor index | `0` |
| `--crop X,Y,W,H` | Explicit crop in absolute source coordinates | unset |
| `--crop-mode MODE` | `full`, `center-square`, or `center-native` | `full` |
| `--crop-x N` | Horizontal center-square offset within the source | automatic |
| `--crop-y N` | Vertical center-square offset within the source | automatic |
| `--crop-size N` | Side length of the center-square crop | automatic |
| `--filter FILTER` | `nearest` or `bilinear` | `nearest` |
| `--aa MODE` | `off`, `gaussian`, or `supersample` | `supersample` |
| `--aa-strength FLOAT` | Gaussian sigma from 0.5 to 2.0 | `1.0` |
| `--supersample N` | Supersampling factor from 1 to 8 | `2` |
| `--saturation FLOAT` | Saturation factor from 0 to 4 | `1.15` |
| `--contrast FLOAT` | Contrast around 127.5, from 0 to 4 | `1.10` |
| `--brightness FLOAT` | Brightness factor from 0 to 4 | `1.00` |
| `--gamma FLOAT` | Streamer gamma from 0.1 to 5 | `1.00` |
| `--no-color-correction` | Disable the entire color stage | disabled |
| `--test-pattern PATTERN` | Generate pixels without screen or RTSP capture | unset |
| `--ddp-debug` | Dump DDP headers for the first frame only | disabled |
| `--benchmark` | Print periodic pipeline timing | disabled |
| `--help` | Print built-in help | — |

`--crop-x`, `--crop-y`, and `--crop-size` require
`--crop-mode center-square`. A value of `-1` means automatic; smaller negative
values are invalid.

There is intentionally no `--source test`. Use `--test-pattern` instead.

## Video sources

### X11 screen

```bash
./build/wled-screen-streamer --source screen
```

`screen` is the default. Capture uses X11 MIT-SHM directly and retains all
monitor, XRandR, and crop behavior.

### RTSP network camera

```bash
./build/wled-screen-streamer \
  --source rtsp \
  --url rtsp://HOST:8554/reolink \
  --rtsp-transport tcp \
  --crop-mode center-square \
  --fps 20
```

RTSP is opened with libavformat, decoded with libavcodec, and converted to RGB24
with libswscale. No external executable is started.

TCP is more resilient to packet loss. UDP can reduce latency on a reliable
local network:

```bash
./build/wled-screen-streamer \
  --source rtsp \
  --url rtsp://CAMERA/stream \
  --rtsp-transport udp
```

The decoder runs in one background thread and publishes a single shared latest
frame. A new decoded frame immediately replaces the previous one. The streamer
does not build a camera frame queue.

The DDP thread only processes a new decoder sequence. It never sends the same
camera frame repeatedly, so a 20 fps camera is not artificially interpolated to
60 fps. For RTSP, `--fps` is a maximum transmission rate.

Low-latency RTSP settings include `nobuffer`, low-delay mode, zero additional
maximum delay, one decoder thread, and interruptible I/O. When opening, reading,
or decoding fails, the current session is closed and rebuilt after one second.
Ctrl+C can interrupt a blocked network read.

After decode, RTSP uses the same crop, resize, AA, color correction, and DDP
pipeline as screen capture. A camera resolution change recalculates and prints
the effective crop:

```text
Capture: 1920x1080 (RTSP)
Crop: center-square 1080x1080+420+0
Output: 64x64
```

Test patterns override either live source and do not require an RTSP URL.

## Capture and crop modes

### Full

`--crop-mode full` captures the complete selected monitor or camera frame. If
the source and output aspect ratios differ, the result is stretched.

```bash
./build/wled-screen-streamer --monitor 0 --crop-mode full
```

### Center-square

`--crop-mode center-square` selects the largest square centered within the
source before resize. It fills a square matrix without stretching or black
bars.

For a 1920×1080 source:

```text
size = min(1920, 1080) = 1080
x    = (1920 - 1080) / 2 = 420
y    = (1080 - 1080) / 2 = 0
crop = 1080x1080+420+0
```

Startup output:

```text
Capture: 1920x1080
Crop: center-square 1080x1080+420+0
Output: 64x64
```

Manual overrides are relative to the selected monitor or decoded frame.
Omitted coordinates remain automatically centered:

```bash
./build/wled-screen-streamer \
  --crop-mode center-square \
  --crop-x 400 \
  --crop-y 0 \
  --crop-size 1080
```

### Center-native

`--crop-mode center-native` selects a centered source area matching the output
dimensions multiplied by the active supersampling factor.

With the defaults—64×64 output and 2× supersampling—on a 1920×1080 source:

```text
Capture: 1920x1080
Crop: center-native 128x128+896+476
Output: 64x64
AA: supersample factor=2
```

The 128×128 crop is reduced using a true 2×2 box average. With
`--supersample 3`, the crop is 192×192 at `+864+444`, and each output pixel is
the average of its corresponding 3×3 source region.

```bash
./build/wled-screen-streamer --crop-mode center-native
```

With `--aa off` or `--aa gaussian`, center-native captures exactly the output
dimensions. The effective crop, including supersampling, must fit inside the
source. `--crop-x`, `--crop-y`, and `--crop-size` only apply to center-square.

### Explicit crop

`--crop X,Y,W,H` uses absolute source coordinates and takes precedence over
monitor and crop-mode calculations:

```bash
./build/wled-screen-streamer --crop 100,50,800,800
```

The crop must fit completely within the source.

### Resolution and monitor changes

The X11 backend listens for XRandR screen, CRTC, and output events. It queries
the selected monitor again and recalculates the crop. The XShm buffer is only
rebuilt when geometry actually changes; the monitor is not polled every frame.

The RTSP backend similarly recalculates crop geometry when decoded dimensions
change.

## Resize filters

- `nearest` is fastest and preserves hard pixel edges.
- `bilinear` mixes four source pixels for a softer image at higher CPU cost.

```bash
./build/wled-screen-streamer --filter nearest
./build/wled-screen-streamer --filter bilinear
```

## Anti-aliasing and moiré reduction

AA only applies to live screen or RTSP capture. Test patterns intentionally
bypass it so their RGB payload remains byte-exact.

### Off

```bash
./build/wled-screen-streamer --aa off
```

The source is resized directly to the output. This is the cheapest mode, but
fine patterns may produce aliasing or moiré on a 64×64 matrix.

### Supersample

```bash
./build/wled-screen-streamer --aa supersample --supersample 2
```

This is the default. The source is first rendered into a preallocated buffer
whose width and height are both `N` times the output. Every output pixel is then
the rounded average of exactly its corresponding `N×N` RGB pixels. This is a
real area/box downsample: no samples are skipped and no per-frame allocation is
performed.

For a 64×64 center-native source:

| Factor | Capture crop | Samples per output pixel |
|---:|---:|---:|
| 1 | 64×64 | 1×1 |
| 2 | 128×128 | 2×2 |
| 3 | 192×192 | 3×3 |

Factors from 1 through 8 are accepted. Internal working dimensions may not
exceed 4096×4096.

### Gaussian

```bash
./build/wled-screen-streamer --aa gaussian --aa-strength 1.0
```

Gaussian applies a light separable low-pass filter to the 64×64 RGB output.
Strength is sigma and ranges from 0.5 to 2.0. The normalized kernel is generated
once at startup out to three sigma. Horizontal results use a preallocated float
buffer, and image edges are clamped.

- `0.5`: very light filtering, retains most detail;
- `1.0`: balanced;
- `2.0`: visibly softer, stronger moiré reduction.

`--aa-strength` is only used by Gaussian mode. `--supersample` is only used by
supersample mode.

## Color correction

Color correction only applies to live capture. It runs after resize and AA and
immediately before DDP packetization.

Defaults:

```text
saturation = 1.15
contrast   = 1.10
brightness = 1.00
gamma      = 1.00
```

Processing order:

1. contrast around midpoint 127.5;
2. saturation relative to Rec.709 luminance;
3. brightness;
4. optional gamma using `pow(channel, 1/gamma)`;
5. clamp to 0–255.

Contrast is prepared as a 256-entry Q12 table. Saturation and brightness use
fixed-point integer math. Gamma uses a 256-entry LUT generated once at startup.
The color stage performs no per-frame allocation.

Gamma deliberately defaults to `1.00`, which is an exact identity. This avoids
applying gamma once in the streamer and again through WLED real-time gamma.
Only choose another value when WLED is not already applying gamma.

Disable every color adjustment:

```bash
./build/wled-screen-streamer --no-color-correction
```

Startup reports either:

```text
Color correction: saturation=1.15 contrast=1.10 brightness=1.00 gamma=1.00
```

or:

```text
Color correction: disabled
```

Recommended TV-style mode:

```bash
./build/wled-screen-streamer \
  --crop-mode center-square \
  --aa supersample \
  --supersample 2 \
  --fps 60 \
  --saturation 1.15 \
  --contrast 1.10
```

## DDP wire format

A 64×64 RGB24 frame contains exactly:

```text
64 × 64 × 3 = 12,288 data bytes
```

The packetizer follows WLED's 1,440-byte data fragment size. Each UDP datagram
contains a 10-byte DDP header followed immediately by only the corresponding
RGB byte range. Offset and length are big-endian byte counts, not pixel counts.

| Packet | Flags | Byte offset | Data length | Datagram length |
|---:|---:|---:|---:|---:|
| 0 | `0x40` | 0 | 1440 | 1450 |
| 1 | `0x40` | 1440 | 1440 | 1450 |
| 2 | `0x40` | 2880 | 1440 | 1450 |
| 3 | `0x40` | 4320 | 1440 | 1450 |
| 4 | `0x40` | 5760 | 1440 | 1450 |
| 5 | `0x40` | 7200 | 1440 | 1450 |
| 6 | `0x40` | 8640 | 1440 | 1450 |
| 7 | `0x40` | 10080 | 1440 | 1450 |
| 8 | `0x41` | 11520 | 768 | 778 |

Header layout:

| Bytes | Contents |
|---|---|
| 0 | DDP version 1 (`0x40`), with PUSH only on packet 8 (`0x41`) |
| 1 | Packet sequence in the low nibble, 1–15 with wraparound |
| 2 | Data type `1`: RGB24 |
| 3 | Destination `1`: display |
| 4–7 | 32-bit byte offset, big-endian |
| 8–9 | 16-bit payload length, big-endian |

The final packet contains exactly 768 payload bytes: no padding and no header
bytes in the RGB payload. All fragments are submitted to the kernel using one
`sendmmsg()` call per frame.

### DDP header diagnostics

```bash
./build/wled-screen-streamer \
  --test-pattern solid-blue \
  --ddp-debug
```

For the first frame, `--ddp-debug` prints packet number, flags, sequence, data
type, destination, offset, and length. Streaming then continues normally.

## Test patterns

Test patterns bypass capture, resize, AA, and color correction. They independently
verify DDP, channel order, and physical pixel mapping. Even explicitly extreme
color settings cannot modify a test pattern.

| Name | Contents |
|---|---|
| `rood` or `red` | Every pixel is RGB(255,0,0) |
| `groen` or `green` | Every pixel is RGB(0,255,0) |
| `blauw` or `blue` | Every pixel is RGB(0,0,255) |
| `solid-blue` | Explicit regression pattern: 4096× RGB(0,0,255) |
| `checkerboard` | Black and white 8×8 blocks |
| `lijn` or `line` | Moving white vertical line |

```bash
./build/wled-screen-streamer --test-pattern red --fps 30
./build/wled-screen-streamer --test-pattern checkerboard
./build/wled-screen-streamer --test-pattern line --fps 60
./build/wled-screen-streamer --test-pattern solid-blue --ddp-debug
```

The solid-blue wire test has been reconstructed byte-for-byte locally. All
12,288 bytes were present and formed exactly 4,096 `(0,0,255)` triplets. The
physical matrix displayed a uniform blue image.

## FPS, latency, and benchmarking

For positive `--fps`, the streamer uses monotonic deadlines. If processing
falls behind, expired periods increment `skipped`; old images are never queued.
The next iteration captures the newest available image. `--fps 0` disables
pacing.

For RTSP, a frame is only sent when the decoder publishes a new sequence.
Camera frames are never repeated to reach the requested output rate.

`--benchmark` reports every second and at shutdown:

- actual FPS and frame count;
- missed deadlines;
- average capture time;
- average resize and AA time;
- average color-correction time;
- average DDP send time;
- average total pipeline time;
- process CPU as a percentage of one CPU core.

Xorg runs in a separate process, so Xorg CPU time is not included. UDP also
cannot confirm that WLED physically displays every uncapped frame.

### Measured screen performance

Test machine: Intel N100, 1920×1080 X11 source, 64×64 RGB24, nearest-neighbor,
local-network WLED. This baseline predates AA and uses AA off:

| Target | Actual | Capture | Resize | Send | Total | Skipped | Process CPU |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 30 | 30.21 | 6.26 ms | 0.10 ms | 0.05 ms | 6.42 ms | 0 | 0.59% |
| 60 | 60.16 | 5.11 ms | 0.11 ms | 0.06 ms | 5.28 ms | 0 | 1.16% |
| 75 | 75.14 | 4.85 ms | 0.12 ms | 0.07 ms | 5.05 ms | 0 | 1.58% |
| 90 | 90.21 | 4.61 ms | 0.12 ms | 0.09 ms | 4.82 ms | 0 | 1.99% |
| 120 | 120.10 | 3.06 ms | 0.09 ms | 0.05 ms | 3.20 ms | 0 | 2.18% |
| uncapped | 429.97 | 2.16 ms | 0.12 ms | 0.05 ms | 2.33 ms | 0 | 7.39% |

A separate 30 fps center-square test reduced capture time from approximately
6.57 ms for 1920×1080 full capture to 4.84 ms for a 1080×1080 crop.

Center-native AA measurements at 60 fps:

| AA mode | Crop | Actual | Capture | Resize + AA | Send | Total | Skipped |
|---|---:|---:|---:|---:|---:|---:|---:|
| supersample 2 | 128×128 | 60.33 | 4.33 ms | 0.27 ms | 0.06 ms | 4.66 ms | 0 |
| supersample 3 | 192×192 | 60.33 | 3.92 ms | 0.70 ms | 0.08 ms | 4.71 ms | 0 |
| Gaussian 0.5 | 64×64 | 60.33 | 4.97 ms | 0.35 ms | 0.07 ms | 5.40 ms | 0 |
| Gaussian 2.0 | 64×64 | 60.37 | 3.80 ms | 0.63 ms | 0.05 ms | 4.48 ms | 0 |
| off | 64×64 | 60.38 | 3.57 ms | 0.06 ms | 0.06 ms | 3.69 ms | 0 |

Color-correction A/B measurement with center-native supersample 2:

| Color correction | Actual | Capture | Resize/AA | Color | Send | Total | Skipped | CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| defaults enabled | 60.13 | 5.45 ms | 0.30 ms | 0.08 ms | 0.07 ms | 5.90 ms | 0 | 2.63% |
| disabled | 60.16 | 5.29 ms | 0.29 ms | 0.00 ms | 0.06 ms | 5.63 ms | 0 | 2.21% |

The color stage itself averaged 0.08 ms per frame. Both runs maintained 60 fps
without skips. Results vary with desktop activity, compositor behavior, CPU
frequency, codec, camera stream, and network load.

## Validation performed

The target installation was validated with:

- WLED hostname resolving correctly through mDNS;
- zero packet loss during the connectivity test;
- WLED 16.0.1 on an ESP32-S3 / Adafruit MatrixPortal build;
- a 64×64 matrix with 4,096 RGB pixels;
- WLED reporting `live=true` and real-time mode `DDP`;
- the physical solid-blue test displaying uniformly blue.

The RTSP extension was linked against libavformat 63.1.100, libavcodec 63.1.100,
libavutil 61.1.100, and libswscale 10.1.100. A local 320×180, 20 fps H.264
source decoded natively. Center-square produced `180x180+70+0`, followed by the
normal AA, color, and DDP pipeline. Failed TCP and UDP connections both entered
the reconnect loop.

After adding RTSP, the X11 regression test achieved 60.24 fps, zero skips, and
4.32 ms total frame time.

DNS or mDNS resolution is performed once at startup, never in the frame loop.
For faster startup, a numeric address can be used:

```bash
./build/wled-screen-streamer --host WLED_IP
```

## systemd user service

The supplied unit uses `DISPLAY=:0`, 60 fps, nearest-neighbor filtering, and the
default screen source:

```text
systemd/wled-screen-streamer.service
```

Install and start it:

```bash
mkdir -p ~/.config/systemd/user
cp systemd/wled-screen-streamer.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now wled-screen-streamer.service
```

Inspect status and logs:

```bash
systemctl --user status wled-screen-streamer.service
journalctl --user -u wled-screen-streamer.service -f
```

Stop and disable it:

```bash
systemctl --user disable --now wled-screen-streamer.service
```

The unit expects the executable at:

```text
~/Development/wled-screen-streamer/build/wled-screen-streamer
```

Edit `ExecStart` for another path, RTSP source, or additional options. Example:

```ini
ExecStart=%h/Development/wled-screen-streamer/build/wled-screen-streamer --fps 120 --filter nearest --crop-mode center-square
```

The user manager must have access to the X11 authentication environment. If
necessary, run this from the active graphical session:

```bash
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user restart wled-screen-streamer.service
```

## Troubleshooting

### X11 display cannot be opened

```bash
echo "$DISPLAY"
echo "$XAUTHORITY"
xrandr --listmonitors
```

The supplied service uses `DISPLAY=:0`; change it when the session uses another
display number.

### MIT-SHM is unavailable

```bash
xdpyinfo | grep MIT-SHM
```

There is intentionally no slow screenshot fallback.

### WLED hostname does not resolve

```bash
getent ahostsv4 wled-matrix2.local
ping -c 3 wled-matrix2.local
```

Check Avahi/mDNS or use `--host` with the numeric address.

### WLED receives no frames

Check the host, firewall, and UDP port 4048. WLED exposes an active stream in
`/json/info` as `live=true` and `lm=DDP`:

```bash
curl -s http://wled-matrix2.local/json/info
./build/wled-screen-streamer --test-pattern solid-blue --ddp-debug
```

### RTSP does not connect

Verify the complete URL, credentials, camera stream path, and selected
transport. Start with TCP, then try UDP on a trusted local network:

```bash
./build/wled-screen-streamer \
  --source rtsp \
  --url rtsp://CAMERA/stream \
  --rtsp-transport tcp \
  --benchmark
```

Connection errors are printed to standard error. The process remains alive and
retries after one second.

### Wrong colors, bands, or black pixels

Use `solid-blue` and compare the header dump with the DDP table above. Also
check WLED matrix dimensions, RGB order, and physical 2D mapping.

### Crop is outside the source

`--crop-x` and `--crop-y` are relative to the selected monitor or RTSP frame,
but the final crop must remain completely inside that source. Reduce the offset
or crop size, or omit values to center automatically.

## Project structure

```text
CMakeLists.txt                         build configuration
src/main.cpp                          streamer implementation
systemd/wled-screen-streamer.service  optional systemd user unit
README.md                              project documentation
build/wled-screen-streamer             local release executable after building
```
