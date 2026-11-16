# MA35D1 Graphics

2D graphics stack for the Nuvoton MA35D1. The GC520 is a 2D-only Vivante GPU (no OpenGL), so Qt is
rendered by a custom QPA plugin that drives the GPU's 2D engine directly, and the result goes to the display
by DMA-BUF, with no CPU copy.

## How it works

```
 Qt5 application (QPainter / Widgets software)
        |
 qt/etnaviv2d-qpa    QPA plugin "etnaviv2d": paint engine on the GC520, damage-aware flush
        |
 drm/libetna2d       C API over the GC520: fill, copy, blend, stretch, glyph mask, opacity
        |
 etnaviv DRM driver  (/dev/dri/renderD128)        <-- kernel
        |
        +-- direct KMS : atomic page flip on the primary or overlay plane
        +-- Wayland    : DMA-BUF ring to Weston (ivi-shell / kiosk-shell, pixman renderer)
                         Weston puts the buffer on a hardware plane
```

1. **Rendering.** The plugin's paint engine turns fills, image blits, text, opacity and scrolling into GC520
   commands in `libetna2d`. Operations the GPU cannot do fall back to Qt's CPU raster engine on the same buffer.
2. **Present.** On `flush()` only the regions the target buffer is missing are copied from the backing store
   (per-buffer damage tracking). The GPU access is serialised by a mutex, so several threads can paint.
3. **Output.** Two transports:
   - **KMS** (`QT_QPA_PLATFORM=etnaviv2d:plane=primary`): double-buffered scanout, non-blocking, event-driven page flip.
     Screen size is the connector mode. Plane is `primary`, or `overlay` with `QT_ETNAVIV2D_PLANE=overlay`.
   - **Wayland** (`QT_QPA_PLATFORM=etnaviv2d`, chosen automatically when `WAYLAND_DISPLAY` is set; `:plane=wayland` forces it): the plugin is a raw `libwayland-client` client (qtwayland is not used) with a `linux-dmabuf` buffer ring, ivi-shell or xdg-shell, and wl_seat input. Window size follows the output and live resizes.
4. **Multi-plane.** Weston runs with the pixman renderer. Qt UI and VPU H.264 video (GStreamer `waylandsink`)
   land on separate hardware planes, composed by the display controller.

## Repository layout

| Path | Contents |
|---|---|
| `drm/libetna2d/` | GC520 2D HAL (`etna2d.h`, shared + static lib) and `etna2d_selftest` acceptance test |
| `qt/etnaviv2d-qpa/` | The Qt5 platform plugin (`src/`), Makefile / `.pro`, tests |
| `qt/etnaviv2d-qpa/tests/etnaviv2d_benchmark/` | 2D operations + composite benchmark |
| `qt/demos/` | `cockpit` and `dashboard` demos, asset generator, runner (see `qt/demos/README.md`) |
| `weston/ivi/` | `weston-ivi.ini` and `nu-hmi-controller`, the ivi-shell layer controller plugin |

## Build

Cross-compile with the Buildroot or Yocto SDK environment sourced; there is no host build.

```sh
source <sdk>/environment-setup-...            # or Buildroot output/host/environment-setup
make -C drm/libetna2d                         # 1. the HAL, everything links it
cd qt/etnaviv2d-qpa && make plugin            # 2. the plugin; QT_BUILD=<qt5base build dir> if needed
cd ../demos && qmake demos.pro && make        # 3. optional demos
```

The plugin must be built with the Qt build's own `qmake` (`$QT_BUILD/bin/qmake`) and is installed to
`<qt>/plugins/platforms/libqetnaviv2d.so`. Buildroot and Yocto packages exist for the plugin and the demos
(`BR2_PACKAGE_QT5_ETNAVIV2D_QPA_DEMOS`, PACKAGECONFIG `demos`).

Kernel requirements: etnaviv DRM driver, MA35D1 atomic KMS driver, and CONFIG_CMA_SIZE_MBYTES=64 or higher.
For concurrent Qt UI and video playback, 64 MB is sufficient for lower resolutions, while 128 MB is
recommended for 1080p.

## Run

```sh
# Check the HAL on the target
etna2d_selftest /dev/dri/renderD128

# Direct KMS (stop Weston first)
QT_QPA_PLATFORM=etnaviv2d:plane=primary ./your_qt_app

# Under Weston: see "Starting Weston" below
WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=etnaviv2d QT_IVI_SURFACE_ID=2000 ./your_qt_app

# Demos (generate assets from your own video on a host first)
cd qt/demos && ./run_demo.sh cockpit <assets-dir>
```

### Starting Weston

Both images start Weston with **kiosk-shell** by default and ship two configs in `/etc/xdg/weston/`:
`weston-kiosk.ini` and `weston-ivi.ini`.

| Image | Weston starts from | Environment for your app |
|---|---|---|
| Buildroot | `/etc/profile.d/weston.sh`, at login | `XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1` (the login shell already sets them) |
| Yocto | `weston.service` (systemd), at boot | `XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1` |

Run your app as usual (set `QT_QPA_PLATFORM=etnaviv2d`, or make it the Qt default with `BR2_PACKAGE_QT5BASE_DEFAULT_QPA` on Buildroot):

```sh
QT_QPA_PLATFORM=etnaviv2d ./your_qt_app
```

**Switch to ivi-shell** (same on both images; the shell is chosen by `shell=` in `weston.ini`)

```sh
cp /etc/xdg/weston/weston-ivi.ini /etc/xdg/weston/weston.ini
```

then restart Weston: `systemctl restart weston.service` on Yocto; on Buildroot `killall weston` and log in again
(or run the `weston ...` line from `/etc/profile.d/weston.sh`).

`qt/demos/run_demo.sh` restarts Weston in the shell the demo needs, so you don't have to do this for the demos.

`weston-ivi.ini` has `touchscreen_calibrator=true` and needs `/etc/xdg/save-resistive-touch-calibration.sh`.

**Buildroot, kiosk-shell (default).** Weston is already running after login; just set the environment:

```sh
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
QT_QPA_PLATFORM=etnaviv2d ./your_qt_app
```

**Buildroot, ivi-shell.** `weston.sh` always starts kiosk-shell, so stop it and start Weston yourself (Weston
holds the DRM master, so only one can run):

```sh
killall weston; sleep 1
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR
weston --tty=1 --backend=drm-backend.so --config=/etc/xdg/weston/weston-ivi.ini \
       --shell=ivi-shell.so --use-pixman >/tmp/weston.log 2>&1 &
export WAYLAND_DISPLAY=wayland-1
```

To make ivi the boot default, edit the `weston --tty=1 ...` line in `/etc/profile.d/weston.sh`
(`--config=.../weston-ivi.ini --shell=ivi-shell.so`).

**Yocto, kiosk-shell (default).** The service is already running. Clients started as root reach its socket
through the `weston` user's runtime dir:

```sh
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1
QT_QPA_PLATFORM=etnaviv2d ./your_qt_app
```

**Yocto, ivi-shell as the boot default.** Either build with `PACKAGECONFIG:append:pn-weston-init = " ivi-shell"`,
or on the target:

```sh
cp /etc/xdg/weston/weston-ivi.ini /etc/xdg/weston/weston.ini
systemctl restart weston.service        # status: systemctl status weston, journalctl -u weston
```

(`cp weston-kiosk.ini weston.ini` switches back.) The ivi ini already contains `use-pixman=true`.

**Yocto, starting Weston by hand** (to test a config without replacing `weston.ini`). The service holds the DRM
master and respawns Weston, so stop it first and restore it afterwards:

```sh
systemctl stop weston.service weston.socket
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR
weston --backend=drm-backend.so --config=/etc/xdg/weston/weston-ivi.ini \
       --shell=ivi-shell.so --use-pixman >/tmp/weston.log 2>&1 &
export WAYLAND_DISPLAY=wayland-1
# restore:
killall weston; systemctl start weston.socket weston.service
```

`qt/demos/run_demo.sh` does the stop / start / restore handling for you on both images.

`weston-ivi.ini` has `use-pixman=true` under `[core]` (the kiosk ini relies on the command line on Buildroot). Weston must be built with GBM (Mesa as the GBM
provider only), otherwise the dma-buf scanout path is silently compiled out.

## Shells: kiosk-shell vs ivi-shell

The plugin detects the shell at run time (it checks whether Weston advertises `ivi_application`). No rebuild
or Qt-side setting is needed; only the Weston config changes.

| | kiosk-shell | ivi-shell |
|---|---|---|
| Window role | `xdg_toplevel`, full-screen | `ivi_surface` with an ID |
| Stacking | Weston decides: the focused / newest window is on top | Fixed by config: `render-order` of each `[nuvoton-layer]` in `weston-ivi.ini` |
| Window size | follows `wl_output` | follows `wl_output`, or pinned with `QT_IVI_GEOMETRY=WxH` |
| Config | `/etc/xdg/weston/weston-kiosk.ini` | `weston/ivi/weston-ivi.ini` + `nu-hmi-controller.so` |
| Use for | one app, or a simple video + UI pair started in order | fixed multi-client layouts (video below, UI above) |

**ivi-shell layers.** `nu-hmi-controller` (`weston/ivi/nu-hmi-controller/`) creates one layer per
`[nuvoton-layer]` section. A client joins the layer whose `id` equals its IVI surface ID. A higher `render-order`
is on top, and the top layers get the higher KMS plane (primary = bottom, overlay = above). Default:

| IVI surface ID | Role | render-order |
|---|---|---|
| 1000 | video (GStreamer `waylandsink`) | 0, bottom, primary plane |
| 2000 | Qt UI | 1, top, overlay plane |
| 9999 | overflow: any surface not listed | -1, always below the others |

- The Qt plugin uses surface ID **2000** unless `QT_IVI_SURFACE_ID` is set. Set a different ID for a second Qt
  app and add a matching `[nuvoton-layer]` section.
- To put the video on top, swap the two `render-order` values in `weston-ivi.ini`.
- An ID that is not in the ini goes to the overflow layer and may be hidden behind the others. Two clients
  with the same ID collide.
- `QT_IVI_SURFACE_ID` has no effect under kiosk-shell.

**kiosk-shell.** There is no per-ID layering. Start the clients in the order you want them stacked (the demo
runner starts the video first, then the Qt UI). Weston still assigns the planes: with the pixman renderer,
surfaces that can be scanned out directly go on hardware planes.

## Configuration

Switches are environment variables, read once; the full table with defaults is in
`qt/etnaviv2d-qpa/src/qetnaviv2dconfig.h`.

| Variable | Effect |
|---|---|
| `QT_QPA_PLATFORM=etnaviv2d` | select the plugin; Wayland is used when `WAYLAND_DISPLAY` is set, else KMS |
| `QT_ETNAVIV2D_PLANE` | `primary`, `overlay` or `wayland` (auto by `WAYLAND_DISPLAY`) |
| `QT_ETNA2D_FMT` | pixel format of the UI buffers (default `argb8888`; 16-bit `rgb565`, `argb4444` save bandwidth) |
| `QT_IVI_GEOMETRY=WxH`, `QT_IVI_SURFACE_ID` | pin the window size / ivi surface id (Wayland) |
| `QT_ETNA2D_CACHED`, `_GPU_TEXT`, `_FULL_FLUSH`, `_SYNC_FLIP`, `QT_ETNAVIV2D_NO_BATCH` | tuning and A/B switches |
| `QT_ETNAVIV2D_DEBUG` | 1 = log, 2 = log + per-second timing report |

## Where to start

- Using Qt on the board: Build, then Run above.
- Seeing what it can do: `qt/demos/README.md`.
- Measuring: `qt/etnaviv2d-qpa/tests/etnaviv2d_benchmark`.

## License

GPL-3.0, see `LICENSE`.
