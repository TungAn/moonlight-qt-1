# Pyrolight

<p align="center"><img src="assets/icon.png" alt="Pyrolight" width="160"></p>

<p align="center">
  <a href="https://github.com/karsyboy/pyrolight/actions/workflows/ci.yml"><img src="https://github.com/karsyboy/pyrolight/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="https://github.com/karsyboy/pyrolight/actions/workflows/release.yml"><img src="https://github.com/karsyboy/pyrolight/actions/workflows/release.yml/badge.svg" alt="Release status"></a>
</p>

Pyrolight is a game-streaming client based on
[Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt). It keeps
Moonlight's standard codecs and features and adds an explicitly selected
[PyroWave](https://github.com/karsyboy/pyrowave) video path for
[Pyroshine](https://github.com/karsyboy/pyroshine) hosts.

- PyroWave decoding on the GPU in 4:2:0 and 4:4:4, SDR and HDR10.
- H.264, HEVC, and AV1 with any Moonlight-compatible host.
- Network bandwidth calibration (UDP on Pyroshine hosts, any codec) and PyroWave receive-buffer diagnostics.
- Named streaming profiles for switching between sets of stream settings.
- [Local integration API](docs/CLIENT_INTEGRATION.md) for PyroDeck: authoritative settings/profile transactions, stable-ID launches and lifecycle events.
- [VRR presentation](docs/VRR.md): frames are shown on the host's frame timing,
  so a VRR display follows the game's frame rate with any host.
- Native Wayland on Linux, with automatic fallback to X11/XWayland. **Settings →
  Advanced Settings → Display backend** shows which one is in use; see
  [Linux display backends](docs/LINUX_DISPLAY.md).
- Native model metadata for Xbox Elite, classic Steam Controller, Steam Deck,
  and DualSense Edge on matching Pyroshine hosts; see
  [controller support and limits](docs/NATIVE_CONTROLLERS.md).
- Selectable audio quality with Pyroshine hosts, including higher-bitrate
  stereo and surround sound.

PyroWave is an intra-only wavelet codec designed for high-bandwidth local
networks. Select it explicitly in Settings; it is never selected automatically.

## Requirements

- Windows 10 or later (x64), or Linux (x86_64).
- For PyroWave: a hardware Vulkan 1.3 GPU and driver that pass the client's
  startup decoder probe, a [Pyroshine](https://github.com/karsyboy/pyroshine)
  host whose `pyroshine healthcheck` reports PyroWave support, and a network
  that sustains the selected bitrate. Software Vulkan devices are rejected.

Standard codecs work with Pyroshine, Sunshine, and other hosts supported by
upstream Moonlight Qt.

## Installation

Download the file for your system from the
[releases page](https://github.com/karsyboy/pyrolight/releases) and install it:

| System | Download | Install |
| --- | --- | --- |
| Windows | `*-windows-x64-installer.exe` | Run the installer |
| Windows (portable) | `*-windows-x64-portable.zip` | Extract and run `Pyrolight.exe` |
| Arch Linux / CachyOS (pacman repository) | `pyrolight-bin` | Add the [`[pyrowave]` repository](https://github.com/karsyboy/pyrowave-packages#setup), then `sudo pacman -Syu pyrolight-bin` |
| Arch Linux / CachyOS | `pyrolight-*.pkg.tar.zst` | `sudo pacman -U ./pyrolight-*.pkg.tar.zst` |
| Debian / Ubuntu | `pyrolight_*.deb` | `sudo apt install ./pyrolight_*.deb` |
| Fedora / RHEL | `pyrolight-*.rpm` | `sudo dnf install ./pyrolight-*.rpm` |
| Other Linux | `*-linux-x86_64.AppImage` | `chmod +x pyrolight-*.AppImage`, then run it |

To upgrade, install the newer release the same way; with the pacman repository,
`sudo pacman -Syu` upgrades `pyrolight-bin` with the rest of the system. After
installing a Linux package, start **Pyrolight** from the application menu or run
`pyrolight`.

- The Linux packages need Ubuntu 22.04, Debian 12, Fedora, RHEL 10, Arch or
  newer (glibc 2.35). They replace an installed `moonlight-qt`; its settings and
  paired hosts carry over. See [Linux packages](docs/LINUX_PACKAGES.md).
- Each release includes `SHA256SUMS` for checking downloads.
- Pyrolight does not check for updates; watch the releases page. macOS, Steam
  Link, ARM, Snap, and Flatpak builds are not published.

## Quick start

1. Install Pyrolight using the [installation options above](#installation),
   then open it from the application menu or run the portable executable.
2. Select your host in Pyrolight to start pairing. If it is missing, add its
   LAN or VPN IP manually.
3. Approve the PIN on the host. For Pyroshine, follow its
   [pairing guide](https://github.com/karsyboy/pyroshine#3-pair); other hosts
   use their own pairing workflow.
4. Select a game or application from the host's list to start streaming.
   Adjust resolution, frame rate, and bitrate in **Settings** for your display
   and network.

For Pyroshine hosts, quit Steam on the host's local desktop before launching
streamed Steam. To use PyroWave, select it explicitly as described below.

## Streaming with PyroWave

1. Pair with the host in Pyrolight.
2. In **Settings**, set **Video codec** to **PyroWave**. The option appears when
   the client's PyroWave decoder probe succeeds on this GPU.
3. Enable **HDR** or **YUV 4:4:4** only when the host and your display support
   them.
4. Choose a bitrate. PyroWave's bitrate is a per-frame quality budget that scales
   with frame rate; the default is approximately 192 Mbps for 4K60 and 384 Mbps
   for 4K120 in 4:2:0 SDR. Use **Calibrate network bandwidth** to measure a safe
   bitrate for a paired host.

From the command line, pass the same options to the AppImage or `Pyrolight.exe`:

```sh
./pyrolight-v*-linux-x86_64.AppImage stream --video-codec PyroWave --bitrate 200000 \
  --resolution 3840x2160 --fps 60 --hdr --yuv444 HOST Desktop
```

PyroWave accepts up to 2,000,000 Kbps and 240 FPS, provided each encoded frame
budget stays between 1 KiB and 3 MiB. Unsupported host capabilities, devices, or
frame budgets fail with an error instead of falling back to another codec.

## Variable refresh rate (VRR)

On a VRR display, enable **V-Sync** and **Variable refresh rate (VRR)** in
Settings, and enable adaptive sync for the display in your compositor (on KDE
Plasma: **Adaptive sync** set to **Automatic** or **Always**). Pick a **VRR
timing** mode (**Smoothest**, **Balanced** or **Lowest latency**); the frame
rate list suggests VRR rates just below the refresh rate, such as 116 FPS at 120
Hz; use one of these rather than the full refresh rate, which leaves the display
no room to follow the game and causes periodic stutter. VRR currently requires
Linux and the Vulkan renderer; when a requirement is not met, the performance
overlay shows why. See [VRR presentation](docs/VRR.md) for details.

## Audio quality

**Audio quality** in Settings (or `--audio-quality`) applies to every video
codec. On a Pyroshine host, **Standard**, **High** and **Maximum** stream stereo
at 96, 256 and 512 kbps (5.1 at 256, 512 and 768 kbps; 7.1 at 450, 768 and
1024 kbps). **High** and **Maximum** also request high-quality surround sound
from any host, even below the 15 Mbps video bitrate Moonlight otherwise
requires. **Host default** (the default) uses the host's setting; other hosts
stream Moonlight's usual audio.

## Streaming profiles

The **Streaming Profile** section of Settings creates, duplicates, renames,
deletes, and switches between named profiles. A profile stores stream settings
such as resolution, frame rate, bitrate, codec, HDR, YUV 4:4:4, V-Sync, frame
pacing, VRR settings, audio configuration and quality, window mode, decoder, and
renderer. Input, interface, and language settings stay global. Settings from
before profiles existed become the **Default** profile.

## Steam Deck library launches (PyroDeck)

[PyroDeck](https://github.com/karsyboy/pyrodeck) starts Pyrolight from a game's
Steam library page. It reads the host's apps with:

```sh
pyrolight list --json HOST
```

This prints one JSON line: `schema` (1), the `host`'s `name` and `uuid`, and
`apps` with each app's `name`, `id`, `hdrSupported`, `hidden` and `sources`.
`sources` holds the launcher identities a Pyroshine host reports for the app,
such as `steam:892970`; it is empty for other hosts. Pyrolight uses its own
pairing for the request, as for `list` and `stream`.

## Troubleshooting

- **Burst packet loss on Linux:** high PyroWave bitrates need larger UDP receive
  buffers. Settings shows the current `net.core.rmem_max` status and offers
  **Fix it** or **Copy command**. See [network tuning](docs/PYROWAVE_NETWORKING.md).
- **Stream limited below the target frame rate:** test with V-Sync disabled so
  the Vulkan renderer can use the lowest-latency present mode. This is a
  diagnostic, not a PyroWave requirement.
- **"The host does not support the selected PyroWave color mode":** the host did
  not advertise PyroWave, or not with the selected HDR/4:4:4 combination. Change
  those settings, or run `pyroshine healthcheck` on the host to check its
  PyroWave support.
- **Judder with VRR after changing the display's adaptive sync setting:** some
  compositor and driver combinations keep a stale VRR state until the display
  is reinitialized. Sign out and back in (or reboot) after switching, for
  example, KDE's **Adaptive sync** between **Always** and **Automatic**.
- **Linux window or input problems:** check **Display backend** in Settings,
  then start Pyrolight with `QT_QPA_PLATFORM=xcb` (for example
  `QT_QPA_PLATFORM=xcb pyrolight`) to see whether the issue is specific to
  native Wayland.
- **General streaming problems:** see the upstream
  [Moonlight troubleshooting guide](https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting),
  and compare with H.264, HEVC, or AV1 to isolate PyroWave-specific issues.

When [reporting a bug](https://github.com/karsyboy/pyrolight/issues), include
the release, OS, GPU and driver, host version, stream settings, and client and
host logs. On Windows, client logs are in `%TEMP%`; on Linux, run the AppImage
from a terminal and capture its output.

## Documentation

- [VRR presentation](docs/VRR.md)
- [PyroWave network tuning and calibration](docs/PYROWAVE_NETWORKING.md)
- [PyroWave integration architecture](docs/PYROWAVE.md)
- [Native controller metadata and validation](docs/NATIVE_CONTROLLERS.md)
- [Linux display backends and AppImage packaging](docs/LINUX_DISPLAY.md)
- [Linux distribution packages](docs/LINUX_PACKAGES.md)
- [Building, testing, and releasing](CONTRIBUTING.md)
- [Upstream Moonlight documentation](https://github.com/moonlight-stream/moonlight-docs/wiki)

## AI-assisted development

AI coding tools, including Codex, assist implementation, testing and documentation.
The maintainer directs the project. All changes require the same review,
validation and security standards, whether written by a human or with AI assistance.

## License and credits

Pyrolight is an independent community fork of [Moonlight
Qt](https://github.com/moonlight-stream/moonlight-qt) and is not an official
Moonlight or PyroWave project. It periodically merges upstream client changes.
Its `moonlight-common-c` submodule points to the
[`karsyboy/moonlight-common-c`](https://github.com/karsyboy/moonlight-common-c)
fork, which carries the PyroWave and controller protocol extensions. The codec
is the pinned [`karsyboy/pyrowave`](https://github.com/karsyboy/pyrowave) fork
of [Themaister's PyroWave](https://github.com/Themaister/pyrowave).

VRR presentation is based on [Nonary](https://github.com/Nonary)'s idea and
reference implementations in
[moonlight-qt](https://github.com/Nonary/moonlight-qt)
([moonlight-stream/moonlight-qt#1956](https://github.com/moonlight-stream/moonlight-qt/pull/1956))
and [Vibepollo](https://github.com/Nonary/Vibepollo).

Licensed under the [GNU General Public License v3](LICENSE), preserving
Moonlight's copyright and license notices. PyroWave is distributed under its own
MIT license, included with packaged runtime components. Bundled dependencies
retain their respective licenses.