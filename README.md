# Plume

A lightweight Steam Remote Play client in C, with SDL3 and FFmpeg.

Plume discovers Steam hosts on the LAN, decodes their H264 video and Opus audio
with FFmpeg, and presents the stream in an SDL3 window, forwarding keyboard,
mouse and gamepads back to the host. The launcher is built for a controller:
D-pad or stick navigation, resolution and scaling settings in a menu, and
Hotkey + Start to leave a running stream. About 1500 lines on top of
[IHSlib](https://github.com/mariotaku/IHSlib), no exotic dependencies, and it
holds 1080p60 on a Raspberry Pi 5.

Runs on Linux **amd64** and **arm64** — nothing is arch-specific; FFmpeg picks
threaded/hardware decode paths per platform. Packaged for Recalbox on the
Raspberry Pi 5 as the `plume` Buildroot package.

## Dependencies (Debian/Ubuntu)

```sh
sudo apt install cmake pkg-config build-essential \
  libsdl3-dev libsdl3-ttf-dev libavcodec-dev libavutil-dev libswscale-dev \
  libswresample-dev libprotobuf-c-dev libmbedtls-dev
```

## Build

```sh
git clone --recursive https://github.com/beudbeud/plume.git
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`--recursive` is not optional. IHSlib lives at `external/IHSlib` as a submodule
of [our fork](https://github.com/beudbeud/ihslib/tree/plume), which carries the
patches listed below. Without it that directory is empty, and CMake silently
falls back to fetching *upstream* IHSlib — which builds, and then misbehaves.
An existing clone catches up with `git submodule update --init`.

Build natively on each architecture (or cross-compile with an aarch64 toolchain).

## Use

```sh
./build/plume --pair     # first time: enter the shown PIN in Steam on the host
./build/plume            # opens the launcher
```

The launcher is an IHSplay-style screen: hosts on the LAN appear in the list.
Navigate with the D-pad / left stick / arrow keys, **A / Enter** starts streaming
the selected host, **B / Esc** goes back. Three icons sit in the top-right
corner: **≡** opens the settings screen, **?** is decorative, **X** quits. It
loops back to the launcher when a stream ends. Uses DejaVu Sans by default; set
`PLUME_FONT=/path/to.ttf` to override.

First time on a host, Start shows a **pairing screen** with a PIN — type it into
Steam on the host (`Settings → Remote Play → Pair Steam Link app`), and streaming
resumes automatically once approved. (`--pair` does the same headlessly.)

Leave a running stream with **Hotkey + Start** on the gamepad (Hotkey = Guide/Home,
or Select if the pad has no Guide), or Esc on the keyboard.

## Settings

The **≡** screen writes `~/.local/share/plume/settings.conf` (plain
`key value` lines) and is applied to the next stream:

| Setting | Values | Notes |
|---|---|---|
| Resolution | 240p / 480p / 720p / 1080p | See the caveat below |
| Scaling | Fit / Stretch / Crop | Fit letterboxes, Stretch distorts, Crop fills and cuts edges |
| Desktop mode | On / Off | Off asks the host for Big Picture instead of the desktop |
| HEVC video | On / Off | Only takes effect if the host offers codec 5 — see below |
| Audio | On / Off | |

Resolution is a **bounding box, not an aspect request**. The host scales its own
desktop to fit inside it and keeps *its* aspect ratio: asking `640x480` of a 16:9
host gets you `640x360`, not a 4:3 picture. To fill a 4:3 display, set Scaling to
Stretch or Crop.

The same values are overridable per-run on the command line, which wins over the
saved file: `--no-audio`, `--no-desktop`, `--hevc`, `--host <ip>` (only with
`--pair`).

Device identity lives next door in `creds.bin`, so pairing survives restarts. A
pre-rename `~/.local/share/steamlink-ihs/` is adopted on first run, so no one has
to re-enter a PIN.

## Hardware decode

Tried first, software second; the fallback is the load-bearing path, so nothing
here needs configuring. The Pi generations disagree on the shape of it:

| | H264 | HEVC |
|---|---|---|
| **Pi 3 / Pi 4** | `h264_v4l2m2m` (VideoCore) | — |
| **Pi 5** | none — software | V4L2-request/DRM hwaccel |
| **PC** | software | software |

The Pi5 lost the H264 block its predecessors have, and its HEVC block never gets
used: the Steam hosts tested only ever advertise codec 4 (H264) and 1 (Raw) at
negotiation, whatever `--hevc`, `device_version` or the host's own HEVC toggle
say. Software H264 keeps 1080p60 there comfortably.

The Pi3 and Pi4 need their hardware decoder. Software H264 on a Cortex-A53 will
not hold 1080p60, and the Pi3's VideoCore caps H264 at 1080p30 — expect 720p on
that board.

The `decode: N ms/frame` log line is *not* the cost of decoding a frame. With
frame threading, `avcodec_send_packet` returns before the work is done, so it
measures the serialized part only; the real work is spread across cores.

## Patches to IHSlib

Non-obvious fixes, one commit each on the fork's `plume` branch, that must
survive a rebase onto upstream:

- **`session/window.c`** — `Poll` skips orphan fragments at the window head
  (joining a stream in progress lands mid-message, and the window used to wedge
  full forever); new `ReleaseAll` for the overflow path.
- **`session/channels/ch_data.c`** — the age-based discard now re-runs on every
  wakeup, not just before the wait loop, so a lost head packet can't wedge `Poll`.
- **`hid/report.c`** — reports store buffer *offsets* and rebind pointers in
  `GetMessage`: `dataBuffer` reallocs, so pointers captured at Add time dangle.
  `AddDelta` reserves `ceil(reportLen/8) + len` — a worst-case delta is a full
  changed-byte bitmask *plus* every byte, larger than the report itself.
- **`hid/sdl/`** — ported SDL2 → SDL3; gamepad events only accumulate state, and
  the caller flushes once per rendered frame with `IHS_SessionHIDSendReport`.
  One reliable control packet per SDL event saturated the Wi-Fi uplink (sticks +
  gyro emit hundreds/sec) and starved the video downlink.
- **`platforms/ihs_udp_posix.c`** — 4 MB `SO_RCVBUF`; the ~208 KB default drops
  most of every keyframe burst.
- **`client/streaming.c`** — `device_version = "1.1.0"`, without which the host
  ignores our resolution/framerate/bitrate caps.

`ctest` covers the window and HID-report fixes; each test was verified to fail
against the pre-fix code (the HID ones need ASan to trip).

## License

LGPL-3.0-or-later, in `COPYING.LESSER` (the Lesser terms) and `COPYING` (the GPL
text they extend) — the FSF's prescribed pair.

Plume links a patched IHSlib *statically*, so the whole binary is covered by
IHSlib's LGPL. Publishing this source, patches included, is what satisfies the
relinking requirement.

## Not included (add when needed)

- Relative-mouse capture for FPS games (only absolute mouse is forwarded).
- Zero-copy for HW decode: HEVC frames are read back and uploaded to an SDL
  texture. DRM-prime/EGL DMABUF import would avoid the copy — add if the readback
  becomes a bottleneck.
- An in-stream settings/help overlay, and the launcher's **?** icon.
- `IHSLIB_SAMPLES=OFF`: the upstream samples still use the old `submit` callback
  signature and would not compile.
