# Qt demos for the etnaviv2d QPA plugin

Multi-plane demos (Qt UI on the GC520-accelerated QPA plugin + VPU H.264 video via GStreamer/Weston):

| Demo | Shell | What it shows |
|---|---|---|
| `cockpit` | kiosk-shell or ivi-shell | full-screen video (primary plane) + transparent cockpit UI with analysis HUD (overlay plane) |
| `dashboard` | ivi-shell | full-screen Qt dashboard (primary) + centered video popup (overlay) opened from an image box |

No videos are shipped. You generate the H.264 clips and thumbnails from **your own video**, then run the demo on the target.

## Build / install

The demos are optional and off by default.

- Buildroot: enable `BR2_PACKAGE_QT5_ETNAVIV2D_QPA_DEMOS` (under qt5-etnaviv2d-qpa).
- Yocto: add `PACKAGECONFIG:append:pn-qt5-etnaviv2d-qpa = " demos"`.
- Manual: `qmake demos.pro && make` with a Qt5 qmake.

Installs `cockpit` and `dashboard` into `/usr/bin`. Runtime needs GStreamer with `v4l2slh264dec`, `h264parse`, `waylandsink`, and Weston.

## Step 1 - host: generate assets from your video

```sh
./gen_assets.sh /path/to/your_video.mp4 [output-dir] [--panel WxH] [--popup WxH] [--fps N]
```

- `--panel WxH`: your panel resolution. The cockpit clip is made at exactly this size (default: source video size).
- `--popup WxH`: dashboard popup size (default: the panel size, else the source video size). Keep it smaller than the panel if the panel is 1080p or larger (display bandwidth).
- `--fps N`: cap the clip frame rate at N (default: source frame rate, at most 60; it never raises it). Use e.g. `--fps 30` for a 60 fps source if the target can't decode 60 fps in real time.
- Output dir default: `./demo_assets`.

Requires `ffmpeg`/`ffprobe`. Produces `videos/cockpit_<W>x<H>.h264`, `videos/clip_<W>x<H>_{1,2,3}.h264` and `thumbs/*.png`.
`run_demo.sh` is copied into the output directory too. Then copy the output directory to the target.

## Step 2 - target: run the demo

```sh
[ENV=value ...] ./run_demo.sh <demo> <assets-dir> [demo args]
```

Environment variables go **before** `./run_demo.sh`; anything after `<assets-dir>` is passed to the demo itself.
The script always restarts Weston (using `/etc/xdg/weston/weston-kiosk.ini` or `weston-ivi.ini`).
Clips are looked up by the panel size read from DRM. Common variable (all demos under ivi-shell):

| Variable | Meaning (default) |
|---|---|
| `QT_IVI_GEOMETRY=WxH` | Qt window size under ivi-shell (panel size) |
| `QT_ETNA2D_FMT=<fmt>` | Qt UI pixel format (per demo, see below) |

## Demos

Each demo is one case below: what it shows, how to run it, its options. When adding a demo, add a section here
with the same fields, plus its assets in `gen_assets.sh` and its branch in `run_demo.sh`.

### cockpit

- **Shows:** full-screen video (primary plane) + transparent cockpit UI with analysis HUD (overlay plane). Works under kiosk-shell or ivi-shell.
- **Assets:** `videos/cockpit_<panel>.h264` (must equal the panel size).
- **Run:**
  ```sh
  ./run_demo.sh cockpit demo_assets                          # kiosk-shell (default)
  WESTON_SHELL=ivi ./run_demo.sh cockpit demo_assets         # ivi-shell
  ```
- **Options:**

  | Variable | Meaning (default) |
  |---|---|
  | `WESTON_SHELL=kiosk\|ivi` | Weston shell (kiosk) |
  | `QT_ETNA2D_FMT` | UI format, needs alpha (argb4444) |
  | `SYNC=true\|false` | video sink sync (false) |

### dashboard

- **Shows:** full-screen Qt dashboard (primary plane) + centered video popup (overlay plane) opened by tapping an image box. ivi-shell only.
- **Assets:** `videos/clip_<WxH>_{1,2,3}.h264` (popup size: panel size first, else the first size found) and `thumbs/ivi_{1,2,3}.png`.
- **Run:**
  ```sh
  ./run_demo.sh dashboard demo_assets
  ./run_demo.sh dashboard demo_assets --auto 8               # no touch/mouse: toggle the popup every 8 s
  POPUP=1024x600 ./run_demo.sh dashboard demo_assets         # force a popup size
  ```
- **Options:**

  | Variable / arg | Meaning (default) |
  |---|---|
  | `POPUP=WxH` | popup clip size (panel size, else first found) |
  | `QT_ETNA2D_FMT` | UI format (rgb565) |
  | `--auto N` (demo arg) | toggle the popup every N s |
