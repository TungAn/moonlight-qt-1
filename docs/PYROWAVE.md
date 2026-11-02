# PyroWave integration

PyroWave is an optional, explicitly selected video codec in Moonlight Qt
PyroWave. Automatic codec selection continues to use Moonlight's standard
codecs. This guide describes how the integration is structured and the
contracts changes must preserve. For user setup see the [README](../README.md);
for transport dialects, receive buffers and calibration see
[networking and compatibility](PYROWAVE_NETWORKING.md); for build commands see
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Components

| Area | Files | Responsibility |
| --- | --- | --- |
| Protocol | `moonlight-common-c` submodule (`src/PyroWave.h`, `SdpGenerator.c`, RTP/FEC queue) | Capability normalization, dialect selection, ANNOUNCE attributes, frame reassembly, record metadata |
| Session | `app/streaming/session.cpp` | Launch checks (host profile bits, frame budget, decoder availability), format selection, decoder creation, receive-buffer warning |
| Decoder | `app/streaming/video/pyrowave.{h,cpp}` | `PyroWaveVideoDecoder`: device borrowing, mailbox, render thread, GPU decode, statistics |
| Record adapter | `app/streaming/video/pyrowaveframing.{h,cpp}` | Validates and compacts record-framed frames; never used for native frames |
| Frame and color rules | `app/streaming/video/pyrowaveframe.h`, `pyrowavecolor.h` | Native frame validation; libplacebo color representation |
| Renderer | `app/streaming/video/ffmpeg-renderers/plvk.{h,cpp}` | libplacebo Vulkan renderer shared with the decoder |
| Settings | `app/settings/streamingpreferences.*`, `bitratecalculator.h`, `app/gui/SettingsView.qml`, `app/cli/commandlineparser.cpp` | `VCC_FORCE_PYROWAVE`, availability probe, default bitrate, CLI `--video-codec PyroWave` |
| Network | `app/backend/networkbuffers.*`, `pyrowavecalibrator.*`, `pyrowavebandwidth.h`, `nvhttp.cpp` | Receive-buffer diagnostics and remediation; authenticated bandwidth probe |

The code is compiled only with `CONFIG+=enable-pyrowave`, which defines
`HAVE_PYROWAVE`. Without it, selecting PyroWave fails with a launch error.

## Capability and negotiation

`StreamingPreferences::isPyroWaveAvailable()` creates a PyroWave device and a
small decoder once; the codec is offered in Settings only when that succeeds.
At launch, `session.cpp` requires the host's PyroWave profile bits for the
selected chroma and HDR mode, a valid per-frame budget
(`LiPyroWaveFrameBudget`), and a hardware decoder. PyroWave never falls back to
another codec, CPU decoding, or a software Vulkan device.

The client requires advertised profile bits and an explicit transport contract.
Native wire-v1 (Pyroshine) is preferred; record framing (Vibepollo) is selected
only through setup negotiation. Unknown bitstream families or contradictory
dialect advertisements fail with a launch error. Details are in
[networking and compatibility](PYROWAVE_NETWORKING.md).

## Decode and render pipeline

```text
common-c reassembly ─▶ submitDecodeUnit ─▶ single-slot mailbox ─▶ render thread
   (complete frame)      (copy, no decode)     (newest frame wins)    GPU decode → libplacebo → present
```

- **Shared device.** The decoder borrows libplacebo's Vulkan instance, device,
  graphics queue and queue-lock callbacks. The renderer requests Vulkan 1.3 with
  subgroup-size control and timeline semaphores for PyroWave streams.
- **Planes.** Three GPU-local R16 UNORM planes are exchanged with the renderer
  through explicit timeline-semaphore ownership. Decoded pixels are never staged
  through CPU memory.
- **Mailbox and render thread.** The decoder advertises direct submit: its
  callback only copies the encoded frame into a bounded latest-frame mailbox,
  bypassing Moonlight's decode-unit queue. A dedicated high-priority render
  thread latches the newest frame and submits its GPU decode before waiting for
  presentation capacity, overlapping decode with the swapchain wait without
  buffering another frame. The client frame-queue statistic counts frames
  replaced in the mailbox before decode.
- **Bounded memory.** Encoded inputs are capped at 3 MiB and reuse host buffers,
  avoiding steady-state per-frame allocation.
- **Loss.** Native whole-frame FEC is used; incomplete native frames are dropped,
  and the next frame resumes decoding without an IDR request because every frame
  is intra-coded. With a host that offers it (Pyroshine), the FEC block layout
  is extended from four to sixteen Reed-Solomon blocks per frame, so frames
  above about 1.1 Gb/s at 120 FPS keep the host's configured parity instead of
  most of it being dropped to fit four blocks (see `x-ss-pyrowave.fecBlocks` in
  common-c's `docs/pyrowave.md`).
- **Renderer refresh.** After creating or resetting the renderer, request an
  independent frame through the normal IDR control message. Pyroshine replays
  its cached frame, so reconnect does not wait for a new desktop redraw if the
  first arriving picture was discarded while the decoder lock was held.
- **Decode path.** The integration uses PyroWave's device preference: the
  fragment decode path on mobile-class GPUs and the compute iDWT path on desktop
  GPUs. `PYROWAVE_FRAGMENT_PATH=1` or `=0` forces either path for benchmarking.
- **Statistics.** The performance overlay reports PyroWave's GPU dequantization
  and iDWT timestamps separately from CPU decode submission and presentation
  waiting.

V-Sync is neither changed nor forced by the decoder. With V-Sync disabled, the
Vulkan renderer selects the lowest-latency supported present mode.

With [VRR presentation](VRR.md) a decode thread and the VRR pacing worker
replace the render thread: the decode thread takes frames from a single-slot
mailbox (skipping frames it has not started when a newer one arrives), decodes
each into one of a bounded pool of plane sets, and the worker renders and
presents it when its schedule requires.

## Color, range and dimensions

The **HDR** and **YUV 4:4:4** preferences select the PyroWave profile. libplacebo
receives the stream's Rec.709 or PQ/BT.2020 metadata, range, chroma siting and
host mastering metadata.

- Wire-v1 SDR uses the scaler's sRGB transfer with BT.709 primaries and matrix.
  The bitstream's SDR/PQ transfer bit cannot distinguish sRGB from other SDR
  transfers, so the renderer uses libplacebo's sRGB transfer for this protocol.
- Decoded planes are normalized floats stored in R16_UNORM. Their libplacebo
  sample and color depths must both be 8 for SDR or both 10 for HDR, without a
  65535/255 or 65535/1023 rescale.
- Full-range SDR8 and HDR10 are the supported profiles. Ten-bit SDR and
  limited-range streams are not part of the host protocol.
- The bitstream carries the visible width and height. PyroWave's 32-pixel
  wavelet alignment stays internal: textures and crop use the visible
  dimensions, so padded rows or columns are never presented or rescaled.

Actual HDR output also depends on an HDR-capable display, compositor, GPU and driver.

## Bitrate

PyroWave is intra-only, so its bitrate is a per-frame budget multiplied by FPS.
`BitrateCalculator::pyroWaveDefaultKbps` starts at 400,000 bytes per 3840x2160
4:2:0 SDR frame, scales with pixel count, adds 20% each for 4:4:4 and HDR, and
multiplies by FPS. 4K defaults are about 192 Mbps at 60 FPS, 384 Mbps at
120 FPS and 461 Mbps at 144 FPS before 4:4:4/HDR scaling. These are tuning
seeds, not requirements.

The codec accepts up to 2,000,000 Kbps and 240 FPS while the encoded-frame
budget stays between 1 KiB and 3 MiB. Selecting a bitrate manually clears
default tracking; `autoAdjustBitrate` only follows setting changes and is not a
live network rate controller.

## Dependency and build contract

| Item | Value |
| --- | --- |
| Source | `vendor/pyrowave` of [`karsyboy/pyroshine`](https://github.com/karsyboy/pyroshine) at `4f1a90b0cc6d2d58663ee29c8a3b8a80e55f3cc2` ([upstream PyroWave](https://github.com/Themaister/pyrowave) plus `vendor/pyrowave-patches/`) |
| C API | `pyrowave-shared` 1.1.0; `app/app.pro` rejects other versions |
| Granite | `53090429539bf3404250d18fe58db971f0907c04` via `checkout_granite.sh` |
| Bitstream family | `186f0393` |

The pin appears in [build-appimage.yml](../.github/workflows/build-appimage.yml),
[release.yml](../.github/workflows/release.yml) and the build instructions; keep
them identical. Both workflows and `scripts/build-pyrowave-appimage.sh` run the vendored
`checkout_granite.sh`, which applies its Granite patches, and the release workflows fail unless the packaged client links and
bundles the PyroWave runtime and its license.

The Pyroshine host may build a newer base or patch series than this pin. Host
and client may differ as long as they share the block-format family; the
golden-bitstream tests decode fixtures produced by both. When changing the pin,
rerun those tests and the smoke test, and do not substitute unpatched upstream
PyroWave or an arbitrary newer ABI.

## Renderer smoke test

After building the normal dependencies, generate the alternate test target from
the application build directory:

```sh
cd build/app
qmake6 ../../app/app.pro -o Makefile.smoke \
  CONFIG+=enable-pyrowave CONFIG+=pyrowave-smoke-test
make -f Makefile.smoke -j"$(nproc)" release
./pyrowave-render-smoke
```

The test exercises decoder creation, consecutive and malformed frames, mailbox
replacement, drops, two frame sizes, SDR/PQ, and 4:2:0/4:4:4. It needs a
Vulkan-capable SDL display even though the window is hidden. Setting
`SUNSHINE_TEST_PYROWAVE_OUTPUT` to a directory of compatible host frame dumps
also renders those frames (the variable keeps its historical name).

The smoke test covers GPU decoding and rendering; it cannot verify physical HDR
luminance or end-to-end network behavior.
