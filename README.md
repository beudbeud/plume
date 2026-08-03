# <img src="assets/logo.svg" height="56" align="center" alt=""> Plume

[![build](https://github.com/beudbeud/plume/actions/workflows/build.yml/badge.svg)](https://github.com/beudbeud/plume/actions/workflows/build.yml)

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
CI builds and tests both on every push. `ctest --test-dir build` runs plume's
self-checks; IHSlib's own tests need a standalone IHSlib build.

## Use

```sh
./build/plume --pair     # first time: enter the shown PIN in Steam on the host
./build/plume            # opens the launcher
```

The launcher is an IHSplay-style screen: hosts on the LAN appear in the list.
Navigate with the D-pad / left stick / arrow keys, **A / Enter** starts streaming
the selected host, **B / Esc** goes back. Three icons sit in the top-right
corner: **≡** opens the settings screen, **?** is decorative, **X** quits. Any
quit from the launcher (X, B or Esc) asks for confirmation first — B on a pad
is one press away from killing the app. It loops back to the launcher when a
stream ends. Uses DejaVu Sans by default; set
`PLUME_FONT=/path/to.ttf` to override.

First time on a host, Start shows a **pairing screen** with a PIN — type it into
Steam on the host (`Settings → Remote Play → Pair Steam Link app`), and streaming
resumes automatically once approved. (`--pair` does the same headlessly.)

Leave a running stream with **Hotkey + Start** on the gamepad (Hotkey = Guide/Home,
or Select if the pad has no Guide), or Esc on the keyboard. **Hotkey + Y** (or F3)
toggles a stats overlay: resolution, codec, fps, bitrate, decode time, dropped
frames. The same line lands in the log once per second under `--verbose`.

If the stream drops without you asking — a Wi-Fi blip, a host hiccup — plume
requests a fresh session from the host and resumes on its own, up to five tries.
Esc or B during the countdown returns to the launcher instead.

## Settings

The **≡** screen writes `~/.local/share/plume/settings.conf` (plain
`key value` lines) and is applied to the next stream:

| Setting | Values | Notes |
|---|---|---|
| Resolution | 240p / 480p / 720p / 1080p | See the caveat below |
| Bitrate | 2–30 Mbps | Left/Right overrides; **A** snaps back to the resolution's tuned default, shown as `(auto)`. The host still adapts downward under this cap on a weak link |
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

By default only problems are printed. `--verbose` adds the IHSlib protocol trace
and the decoder's chatter.

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
used. Not our doing: a Linux Steam host advertises only codec 4 (H264) and 1
(Raw) at negotiation, and nothing a client sends can change that. The streaming
request carries no codec field (checked against Valve's own `.proto`), and the
client's `supports_video_hevc` reaches the host *after* it has already sent its
codec list. Steam's HEVC checkbox lives under *client* options — it says what
that machine can decode, not what it will encode for you. The host log shows
`Allowed Codecs: 4` and never mentions HEVC.

A Windows host does offer codec 5 (confirmed in the field), and the Pi5's
hardware path lights up on its own. Software H264 keeps 1080p60 comfortable
either way on that board.

The Pi3 and Pi4 need their hardware decoder. Software H264 on a Cortex-A53 will
not hold 1080p60, and the Pi3's VideoCore caps H264 at 1080p30 — expect 720p on
that board.

### Zero-copy presentation

When the decoder hands back `DRM_PRIME` frames (Pi HEVC), plume tries three
paths in order, each probed at runtime and each falling back to the next with
one log line saying why:

1. **DRM video plane** (KMSDRM only, needs libdrm at build time): the frame's
   dmabuf becomes a DRM framebuffer scanned out directly by the display
   controller on an overlay plane above SDL's output. No GPU sampling, no
   readback, no upload — true zero-copy, and it handles the Pi's SAND tiling,
   which the HVS understands natively. The plane covers SDL's rendering, so
   toggling the stats overlay switches to the GPU path while it is visible.
2. **EGLImage import**: dmabuf planes wrapped as GL textures inside an SDL
   NV12 texture. Linear buffers only — Mesa v3d *accepts* a SAND import and
   then samples it as linear (the whole screen comes out pink), so tiled
   modifiers are rejected up front.
3. **Readback** on the main thread (the rpi FFmpeg unpacks SAND correctly);
   still lighter for the decode thread than the pre-zero-copy pipeline.

Run with `--verbose` and look for the `drm-plane:` and `zero-copy:` log lines
to see which path you got. `PLUME_NO_ZEROCOPY=1` restores the old
decode-thread readback entirely.

With `--verbose`, the overlay's line is also logged once per second as
`stats: ...` (with a `plane` tag when the DRM plane is scanning) — handy over
ssh. Its `dec` figure is *not* the cost of decoding a frame: with frame
threading, `avcodec_send_packet` returns before the work is done, so it
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
  ignores our resolution/framerate/bitrate caps. Also reserves gamepad slots in
  the streaming request (`IHS_StreamingRequest.gamepadCount`), the way Steam's
  own client does, so the host sets the controllers up from the request instead
  of only learning of them when the HID channel enumerates mid-negotiation.
- **`session/channels/control/control_hid.c`** — never send a HID message before
  the session is streaming. IHSlib gated outbound HID on `BStreamingInput()` but
  not on `IsStreaming()` (both of which Steam checks). The HID manager enumerates
  on the timer thread, so a slow handshake lets the device list race ahead of the
  authentication request; the host accepts that early `RemoteHID` and then never
  answers, and the session dies mid-negotiation. New `IHS_SessionStreaming()`
  (`connectionState == Connected`) gates it. Surfaced under a libretro front-end
  (since removed), where video init delayed the handshake past the first HID tick.
- **`session/channels/channel.c`** — a fragmented outgoing frame is now numbered
  the way the receiving window reads it back. The head carried `size/limit + 1`
  where the window expects `1 + head->fragmentId` fragments *after* the head (one
  too many, two when the body divided evenly), and every fragment reused the
  head's packet id where the window keys its slots by id. No fragmented control
  message (e.g. a large `SetIcon`) was ever acknowledged; the control window then
  overflowed and tore the session down.
- **`session/packet.h` + `session/retransmission.c` + `ch_control.c`** — a
  per-packet retransmit cap (a non-wire header field, default
  `RETRANSMISSION_ATTEMPTS`), set to 3 for `k_EStreamControlRemoteHID`. HID input
  is a full state snapshot; retransmitting a lost one 20 times (200 ms) head-of-
  line-blocks every later input behind it — a ~1/min control freeze on lossy
  Wi-Fi. Three tries (~30 ms) is enough before letting the channel advance; the
  next report supersedes the lost one, and the host resyncs its sequence past the
  gap as it already does on a full give-up. Steers only our retransmit policy, no
  wire change.

`ctest` covers the window and HID-report fixes; each test was verified to fail
against the pre-fix code (the HID ones need ASan to trip). The fragment-numbering
and retransmit-cap fixes are not yet covered — the send path has no seam to
capture emitted packets.

## License

LGPL-3.0-or-later, in `COPYING.LESSER` (the Lesser terms) and `COPYING` (the GPL
text they extend) — the FSF's prescribed pair.

Plume links a patched IHSlib *statically*, so the whole binary is covered by
IHSlib's LGPL. Publishing this source, patches included, is what satisfies the
relinking requirement.

## Not included (add when needed)

- Relative-mouse capture for FPS games (only absolute mouse is forwarded).
- An in-stream settings/help overlay, and the launcher's **?** icon.
- `IHSLIB_SAMPLES=OFF`: the upstream samples still use the old `submit` callback
  signature and would not compile.
