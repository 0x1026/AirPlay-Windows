# AirPlay Windows Receiver

Latest release: [v0.3.0](https://github.com/moieric11/AirPlay-Windows/releases/tag/v0.3.0)
— [release notes](docs/releases/v0.3.0.md).

Native AirPlay 2 receiver (mirror + RAOP audio) for Windows, ported
from [UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0). 100 %
native Windows stack — no Bonjour SDK, no Apple runtime dependency.

> **Status — working AirPlay 2 receiver on Windows.**
>
> An iPhone / iPad / Mac on the same Wi-Fi sees `AirPlay-Windows`
> in its Control Center. Screen mirroring shows up **in real time
> in an SDL2 window** (498x1080 for an iPhone 16 in portrait,
> resizable, aspect-preserving). Apple Music, native videos and
> iOS notification audio **come out of the default Windows audio
> device** via WASAPI/SDL.
>
> What does NOT work: apps that force the **AirPlay Streaming**
> mode with FairPlay DRM (YouTube / Netflix / Apple TV+) are out
> of scope — they need a separate protocol and a separate DRM,
> several days of work.

## What works

| Step                                         | State | UxPlay source ported                          |
|----------------------------------------------|-------|-----------------------------------------------|
| mDNS `_airplay._tcp` + `_raop._tcp`          | OK    | Windows `dnsapi.dll` (native, no Apple SDK)   |
| `GET /info` — full bplist00                  | OK    | `plist/info.plist` (via libplist)             |
| `POST /pair-setup` (no-SRP)                  | OK    | `lib/pairing.c`                               |
| `POST /pair-verify` (X25519+AES-CTR+Ed25519) | OK    | `lib/pairing.c`                               |
| `POST /fp-setup` (4x142 replay)              | OK    | `lib/fairplay_playfair.c` (blobs isolated)    |
| AirPlay 2 `SETUP` (session + streams plist)  | OK    | `raop_handler_setup`                          |
| `GET_PARAMETER volume`                       | OK    | `raop_handler_get_parameter`                  |
| `RECORD`, partial `TEARDOWN` + session       | OK    | `raop_handler_teardown`                       |
| NTP client (poll iOS timing server)          | OK    | `lib/raop_ntp.c`                              |
| Mirror TCP listener + frame parser           | OK    | `lib/raop_rtp_mirror.c`                       |
| SPS parse (profile/level/resolution)         | OK    | H.264 spec, standalone                        |
| **FairPlay decrypt (ekey -> AES key)**       | OK    | `lib/playfair/*` (bundled in `third_party/`)  |
| **AES post-hash with ECDH secret**           | OK    | `raop_handler_setup` (modern-client path)     |
| **AES-CTR on H.264 NALs (in-place decrypt)** | OK    | `lib/mirror_buffer.c`                         |
| **Split NAL + Annex-B conversion**           | OK    | `raop_rtp_mirror_thread` (port-by-port)       |
| **H.264 decoder (libavcodec)**               | OK    | FFmpeg — SPS/PPS extracted from avcC          |
| **Real-time video renderer (SDL2)**          | OK    | SDL2 IYUV texture, GPU YUV->RGB               |
| **Audio UDP RTP + AES-CBC decrypt**          | OK    | `lib/raop_buffer.c`                           |
| **RAOP RTP seq dedup**                       | OK    | 65k bitset, sliding window                    |
| **AAC-ELD decoder (libavcodec)**             | OK    | ASC `F8 E8 50 00` built from scratch          |
| **SDL2 / WASAPI audio output**               | OK    | int16 stereo 44.1 kHz, push mode              |
| **Volume control (`SET_PARAMETER` -> gain)** | OK    | dB -> linear, atomic, fast-path unity         |
| **Cover art (image/jpeg -> disk)**           | OK    | dump `cover.jpg` on every track change        |
| **DAAP metadata (mlit/minm/asar/asal/astm)** | OK    | standalone DMAP parser, `metadata: "Title" - Artist (Album, 234s)` |
| **ALAC fallback (ct=2)**                     | OK    | 36 B magic cookie + dispatch `AV_CODEC_ID_ALAC` |
| **Cover + metadata overlay (SDL_ttf)**       | OK    | rendered on the idle stage when audio-only    |
| **In-app overlay (Dear ImGui)**              | OK    | toolbar / sidebar / options / status bar      |
| **Hide UI mode (borderless, draggable)**     | OK    | SDL hit-test edges = resize, video = drag     |
| **USB Personal Hotspot path**                | OK    | works over the cable subnet, no Wi-Fi needed  |
| AirPlay Streaming mode (FairPlay Streaming)  | NO    | YouTube / Netflix / ATV+ — out of scope       |

## Strategy

Mirror UxPlay as closely as possible, line by line, validating each
step against a real iPhone. No improvisation: every file cites its
UxPlay source and keeps a GPL-3.0 compatible license.

## Display rotation

In **OPTIONS > Rotation**, choose **Automatic (device)** (default) or a manual
clockwise rotation of **0, 90, 180 or 270 degrees** relative to the received
image. Changes apply immediately and the choice is saved when the app closes.
The image keeps its proportions, including in fullscreen and video-only mode.

Automatic mode follows portrait/landscape changes in the video sent by the device;
it does not read the device's physical orientation sensor. If the device keeps the
stream in portrait (for example with orientation lock), use manual rotation
to turn the displayed image. Manual 0 degrees displays the received image
without an additional rotation. Recordings keep the original stream orientation.

In **OPTIONS > Mirror Resolution**, presets specify the maximum stream dimensions
advertised to the sending device, not its native screen resolution. The device
chooses the actual resolution and aspect ratio. Square limits leave room for
both portrait and landscape on phones and tablets; rectangular presets also
remain available for wider displays. The panel shows the received resolution
while streaming. Resolution changes apply on the next device connection.

## GPU decode

The **GPU decode** option uses D3D11VA and keeps decoded images in GPU memory
through color conversion and display. Decode and SDL rendering use the same GPU
adapter, sharing textures with GPU fence synchronization. There is no per-frame
GPU-to-CPU pixel transfer for display. Reconnect the sending device after changing
this option. Software decoding remains available if the GPU path is unsupported.
Decoder surface views and render targets are reused between frames. The display
device's present queue is limited to one frame to reduce queued-image latency.

With GPU decoding and a compatible encoder, **Auto / GPU** MP4 recording uses
D3D11 surfaces directly. Scaling, color conversion and letterboxing after device
rotation stay on the GPU; the recorder uses its own surfaces so encoder buffering
does not hold the decoder's pool. The codec status shows **[GPU resident]**.
Explicit **GPU** recording never silently downloads GPU-decoded images: if no
compatible direct encoder exists, it reports an error. **Auto** can fall back to
a CPU pixel path, and **CPU** recording remains available. Audio encoding and
writing compressed MP4 packets still run on the CPU.
The first-frame PPM diagnostic is skipped in GPU mode to avoid a readback.

To run the Windows hardware integration checks, configure with
`-DBUILD_GPU_TESTS=ON`, build `test-gpu-video` in Release, then run
`build/Release/test-gpu-video.exe`. The decoding fixtures use NVIDIA NVENC;
the test checks H.264/HEVC CPU and GPU output, resolution and codec switches,
GPU texture array slices, full/limited color range and four display rotations.
It also verifies GPU H.264/HEVC recordings with concurrent display, Auto/CPU modes,
software-decoded input, zero-readback counters, MP4 decoding, audio and timestamps.
Pixel upload/readback in this test is validation instrumentation only.
Pass `--benchmark` to also measure 1080p conversion submission and rendering.
These local timings do not measure device-to-screen AirPlay latency.

## Requirements (Windows)

- **Visual Studio 2022** (or Build Tools) with C++ workload
- **CMake 3.21+**
- **vcpkg** (for OpenSSL, libplist, FFmpeg, SDL2, SDL2_ttf — declared
  in `vcpkg.json`)
- **Windows 10 1809+ or Windows 11** (native mDNS API
  `DnsServiceRegister`)

No Apple dependency required: the receiver advertises itself through
Windows' `dnsapi.dll`, not through Bonjour SDK.

## Build

```powershell
git clone https://github.com/moieric11/AirPlay-Windows.git
cd AirPlay-Windows

# If your VS bundles vcpkg (typical 2022+ install):
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="C:\Program Files (x86)\Microsoft Visual Studio\17\BuildTools\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

First configure: vcpkg downloads and compiles `libplist`, `openssl`,
`ffmpeg`, `sdl2`, `sdl2-ttf` (~5 min). Then it stays cached.

## FairPlay

The `/fp-setup` handshake uses 4 pre-recorded Apple TV responses
(568 bytes total) plus a decryption routine (`playfair`). Both are
**bundled in this repo** under `third_party/` — `cmake --build`
detects them and links them in automatically. No manual provisioning.

Origin and legal status of these assets: see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Short version:
the `playfair` sources are GPL-3.0; the 568 FairPlay bytes are
produced by Apple's FairPlay implementation, redistributed here on
the same precedent as UxPlay / RPiPlay / shairport-sync (openly
published for several years without enforcement). If Apple requests
removal we'll comply.

How to re-derive those bytes from a real Apple TV (transparency +
verification): [`docs/FAIRPLAY.md`](docs/FAIRPLAY.md).

## Run

```powershell
build\Release\airplay-windows.exe
```

The released binary is silent by default (no console pops up). For
init / runtime logs, pass `--log` (or `--verbose`):

```powershell
build\Release\airplay-windows.exe --log
```

Expected log lines at startup with `--log`:

```
ip=192.168.x.x
TCP server listening on [::]:7000 (IPv4+IPv6)
mDNS registered: AirPlay-Windows._airplay._tcp.local
mDNS registered: XXXXXXXXXXXX@AirPlay-Windows._raop._tcp.local
```

Then on iPhone / iPad / Mac on the same Wi-Fi: Control Center ->
Screen Mirroring -> `AirPlay-Windows`. On tap, the logs show the
full handshake + the H.264 stream coming in.

### Receiver name

```powershell
build\Release\airplay-windows.exe --name "hugoo-computer"
```

`--name NAME` (or `--name=NAME`) sets the name used by AirPlay, RAOP,
`/info` and the idle screen. Priority: CLI > saved `receiver_name` >
`AirPlay-Windows`. Existing settings files without that key keep the original
name. The effective name is saved with the other settings on normal exit
(close the window or press Ctrl-C); a forced termination does not save it.

On Windows you can also edit `receiver_name=hugoo-computer` in
`%APPDATA%\AirPlay-Windows\settings.ini` while the app is closed. Save the
file as UTF-8 and restart the app. To reset, launch with
`--name "AirPlay-Windows"` and close normally, or remove the key while closed.
Settings persistence remains Windows-only, as before.

Names support Unicode and internal spaces, and must occupy 1-50 UTF-8 bytes
(RAOP reserves 13 of the DNS label's 63 bytes for the MAC address and `@`).
Control characters, dots, backslashes and leading/trailing ASCII spaces are
rejected. Invalid CLI names exit with code 2; invalid saved names are ignored.
Use `--help` for command-line help.

### In-app shortcuts

| Key                | Action                                          |
|--------------------|-------------------------------------------------|
| `H`                | Toggle "Hide UI" — hides every panel + the SDL window border, snaps the window to the video aspect ratio |
| `F` / `F11` / dbl-click | Toggle real OS fullscreen                  |
| `R`                | Start / stop an MP4 recording of the AirPlay video stream |
| `ESC`              | Peels back: exits fullscreen, then Hide UI, then quits |

The toolbar also exposes a **Disconnect** button (only visible when a
session is active) that drops the current AirPlay client and returns
the renderer to the idle "Waiting for AirPlay" screen.

The **REC** toolbar button (or `R`) records the decoded AirPlay video to
an MP4 file. The destination can be selected with the native **Browse...**
button in **Options > Recording**; by default files are saved next to the
`airplay-windows.exe` application. Encoding runs
on a bounded background queue so a slow disk cannot increase mirroring
latency. Resolution/orientation changes are fitted into the original MP4
canvas without stretching.
The recording panel also exposes **Auto / GPU / CPU** encoder selection and
a 2-40 Mbps bitrate control. Auto prefers NVIDIA NVENC, AMD AMF, Intel Quick
Sync, then Windows Media Foundation before falling back to a CPU encoder.
The MP4 video codec can be switched between broadly compatible **H.264** and
more efficient **H.265/HEVC**. HEVC uses `hevc_nvenc`, `hevc_amf`, `hevc_qsv`,
`hevc_mf`, or `libx265`, depending on the selected hardware mode and what the
installed FFmpeg build exposes.
Decoded RAOP audio is muxed into the same MP4 as a synchronized stereo AAC
track (192 kbps), independently of whether a Windows playback device exists.

## AirPlay over USB-C cable (no Wi-Fi)

An iPhone plugged in via USB can route AirPlay through the cable —
useful in airplane mode, on a PC without Wi-Fi, or to avoid the
latency / jitter of a busy Wi-Fi. **No code changes needed**, just
an iOS toggle and an Apple driver on Windows.

### Requirements

- **iTunes** or **Apple Devices** (Microsoft Store) installed on
  Windows -> provides `Apple Mobile Device USB Driver` which lets
  iOS and Windows bring up a virtual ethernet interface over the
  cable.
- The iPhone must have trusted this PC at least once (the "Trust
  this computer?" prompt -> Yes, iPhone passcode).

### Procedure

1. **Plug the iPhone** into the PC via USB-C / Lightning.
2. On the iPhone: **Settings -> Personal Hotspot -> Allow Others
   to Join** -> ON. (The "USB only" option is enough if offered.)
3. On Windows, in the network menu, a new **`Apple Mobile Device
   Ethernet`** connection appears with an IP on the
   `172.20.10.0/24` subnet.
4. Run `airplay-windows.exe --log` — at startup the logs announce
   `mDNS registered: ... 172.20.10.x`.
5. On the iPhone: Control Center -> Screen Mirroring ->
   `AirPlay-Windows` shows up **even with no shared Wi-Fi**.

AirPlay then routes over the Personal Hotspot ethernet subnet, which
exists only along the cable. No cellular data is consumed (the
AirPlay session stays local between the iPhone and the PC), no Wi-Fi
needed at all.

### Technical notes

- **Direct QuickTime over USB is dead on iOS 17/18.** Apple
  repurposed the legacy QT interface (composite child `MI_02`,
  historical class `0xFF/0x2A`) to USB Ethernet (class `0xFF/0xFD`).
  The old "QuickTime Video Hack" + Zadig recipes no longer work on
  modern iPhones.
- The binary's **USB supervisor** (`src/usb/usb_supervisor.cpp`)
  detects an iPhone being plugged in and logs a Personal Hotspot
  reminder — purely informational, no driver claim is performed.
- The `172.20.10.0/24` subnet is arbitrated by iOS; the gateway is
  the iPhone, the PC gets `172.20.10.2` over DHCP. `_airplay._tcp`
  mDNS advertises naturally on this interface because we bind on
  `0.0.0.0` (all interfaces).

## Tests

A Python suite that role-plays an iOS client for 6 scenarios
(pair-verify round-trip, absolute URI + real iOS headers, corrupted
signature, concurrent sessions, fp-setup framing, legacy
ANNOUNCE/SETUP/RECORD/TEARDOWN):

```bash
pip install cryptography
python3 tools/test_pair_verify.py
# === 26/26 checks passed ===
```

## What's next

The functional core is in place: video mirror + RAOP audio in both
modes (AAC-ELD + ALAC), reactive volume, cover art and DAAP
metadata extracted, in-app overlay UI. What remains is either UI
polish or significant chunks of work:

1. **Playback UI** — iOS sends `progress: start/cur/end` which we
   already log; a progress bar in the window would be a natural
   next step.
2. **AirPlay Streaming mode** for YouTube / Netflix / Apple TV+.
   Distinct protocol (`/reverse`, `/play` with URL, FairPlay
   Streaming DRM). Several days of work, sensitive DRM license.
3. **Multi-session** — currently single-iPhone assumption. Sidebar
   already has the data model for multiple devices, the RTSP layer
   would need session-id-based routing.

## References

- **[UxPlay](https://github.com/FDH2/UxPlay)** — primary reference,
  active fork of RPiPlay, maintained for iOS 17/18 (GPL-3.0)
- [RPiPlay](https://github.com/FD-/RPiPlay) — ancestor, unmaintained
  since 2021 (GPL-3.0)
- [OpenAirplay](https://openairplay.github.io/airplay-spec/) —
  protocol notes
- [playfair](https://github.com/EstebanKubata/playfair) — FairPlay
  decryptor
- [shairport-sync](https://github.com/mikebrady/shairport-sync) —
  for the RAOP audio path

## License

Project code: **GPL-3.0** (see [`LICENSE`](LICENSE)). Ported from
UxPlay, itself GPL-3.0.

**Bundled or linked third-party components** (FairPlay reply blobs,
playfair, libplist, OpenSSL, FFmpeg, SDL2, Dear ImGui, etc.): see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the origin,
license and legal status of each component — in particular the 568
FairPlay bytes which are produced by Apple (Apple keeps ownership;
redistribution under the UxPlay/RPiPlay precedent).
