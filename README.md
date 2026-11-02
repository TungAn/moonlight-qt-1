# Pyrolight

<p align="center"><img src="assets/icon.png" alt="Pyrolight" width="160"></p>

<p align="center">
  <a href="https://github.com/karsyboy/pyrolight/actions/workflows/ci.yml"><img src="https://github.com/karsyboy/pyrolight/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="https://github.com/karsyboy/pyrolight/actions/workflows/release.yml"><img src="https://github.com/karsyboy/pyrolight/actions/workflows/release.yml/badge.svg" alt="Release status"></a>
</p>

Pyrolight is a game-streaming client based on
[Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt). It keeps
Moonlight's standard codecs and features and adds
[PyroWave](https://github.com/Themaister/pyrowave) streaming for
[Pyroshine](https://github.com/karsyboy/pyroshine) hosts. On a Steam Deck,
[PyroDeck](https://github.com/karsyboy/pyrodeck) launches Pyrolight streams
from your Steam library.

- GPU PyroWave decoding in 4:2:0 and 4:4:4, SDR and HDR10.
- H.264, HEVC, and AV1 with any Moonlight-compatible host.
- [VRR presentation](docs/VRR.md) that follows the game's frame rate on a VRR
  display, with any host.
- Network bandwidth calibration and PyroWave receive-buffer checks.
- Named streaming profiles for switching between sets of stream settings.
- Selectable audio quality with Pyroshine hosts, including higher-bitrate
  stereo and surround sound.
- [Microphone](#microphone) for games and chat on the host, with Pyroshine,
  Rubylight and Apollo hosts.
- Native model support for Xbox Elite, classic Steam Controller, Steam Deck,
  and DualSense Edge with matching Pyroshine hosts; see
  [controller support and limits](docs/NATIVE_CONTROLLERS.md).
- Native Wayland on Linux, with automatic fallback to X11/XWayland; see
  [Linux display backends](docs/LINUX_DISPLAY.md).

PyroWave is an intra-only wavelet codec designed for high-bandwidth local
networks. Select it explicitly in Settings; it is never selected automatically.

## Requirements

- Windows 10 or later (x64), or Linux (x86_64). Linux packages need glibc 2.35
  or later (Ubuntu 22.04, Debian 12, Fedora, RHEL 10, Arch, or newer).
- A host running Pyroshine, Sunshine, or another host supported by Moonlight Qt,
  on a trusted LAN or VPN.

PyroWave additionally requires a Pyroshine host with PyroWave support, a
hardware Vulkan 1.3 GPU that passes Pyrolight's decoder probe, and a network
fast enough for the selected bitrate. Software Vulkan devices are not supported.

## Installation

Download the file for your system from the
[releases page](https://github.com/karsyboy/pyrolight/releases), or use the
pacman repository on Arch Linux / CachyOS:

| System | Download | Install |
| --- | --- | --- |
| Windows | `*-windows-x64-installer.exe` | Run the installer |
| Windows (portable) | `*-windows-x64-portable.zip` | Extract and run `Pyrolight.exe` |
| Arch Linux / CachyOS (pacman repository) | `pyrolight-bin` | Add the [`[pyrowave]` repository](https://github.com/karsyboy/pyrowave-packages#setup), then `sudo pacman -Syu pyrolight-bin` |
| Arch Linux / CachyOS | `pyrolight-*.pkg.tar.zst` | `sudo pacman -U ./pyrolight-*.pkg.tar.zst` |
| Debian / Ubuntu | `pyrolight_*.deb` | `sudo apt install ./pyrolight_*.deb` |
| Fedora / RHEL | `pyrolight-*.rpm` | `sudo dnf install ./pyrolight-*.rpm` |
| Steam Deck / other Linux | `*-linux-x86_64.AppImage` | `chmod +x pyrolight-*.AppImage`, then run it |

- Linux packages replace an installed `moonlight-qt`; its settings and paired
  hosts carry over. See [Linux packages](docs/LINUX_PACKAGES.md).
- Each release includes `SHA256SUMS` for checking downloads.
- To upgrade, install the newer release the same way, or run `sudo pacman -Syu`
  with the repository. Pyrolight does not check for updates itself; watch the
  releases page.
- macOS, Steam Link, ARM, Snap, and Flatpak builds are not published.

## Quick start

### 1. Install

Install Pyrolight using the [installation options above](#installation), then
open **Pyrolight** from the application menu, run `pyrolight`, or start the
portable executable.

### 2. Pair

Select your host in Pyrolight to start pairing. If it is missing, add its LAN or
VPN IP manually. Approve the PIN on the host: for Pyroshine, follow its
[pairing guide](https://github.com/karsyboy/pyroshine#4-pair); other hosts use
their own pairing steps.

### 3. Play

On a Pyroshine host, quit Steam on the host's local desktop first. Select a game
or application from the host's list to start streaming. Adjust resolution,
frame rate, and bitrate in **Settings** for your display and network.

## Streaming with PyroWave

1. Pair with a Pyroshine host using the steps above.
2. In **Settings**, set **Video codec** to **PyroWave**. The option appears when
   this GPU passes Pyrolight's decoder probe.
3. Start with SDR and 4:2:0. Enable **HDR** or **YUV 4:4:4** only when the host
   and your display support them.
4. Use **Calibrate network bandwidth** to choose a bitrate your network can
   sustain. PyroWave needs much more bandwidth than standard codecs; the default
   is about 192 Mbps for 4K60 and 384 Mbps for 4K120 in 4:2:0 SDR.

Unsupported modes fail with an error instead of silently changing codecs. See
[PyroWave network tuning](docs/PYROWAVE_NETWORKING.md) for bitrate and buffer
details.

From the command line, pass the same options to the AppImage or `Pyrolight.exe`:

```sh
./pyrolight-v*-linux-x86_64.AppImage stream --video-codec PyroWave --bitrate 200000 \
  --resolution 3840x2160 --fps 60 --hdr --yuv444 HOST Desktop
```

## Variable refresh rate (VRR)

VRR currently requires Linux and the Vulkan renderer.

1. Enable adaptive sync for the display in your compositor. On KDE Plasma, set
   **Adaptive sync** to **Automatic** or **Always**.
2. In Pyrolight's **Settings**, enable **V-Sync** and **Variable refresh rate
   (VRR)**.
3. Pick a **VRR timing** mode: **Smoothest**, **Balanced**, or **Lowest
   latency**.
4. Choose one of the suggested frame rates just below the refresh rate, such as
   116 FPS at 120 Hz. Streaming at the full refresh rate leaves the display no
   room to follow the game and causes periodic stutter.

If a requirement is not met, the performance overlay shows why. See
[VRR presentation](docs/VRR.md) for details.

## Audio quality

**Audio quality** in Settings (or `--audio-quality`) applies to every video
codec. On a Pyroshine host:

| Quality | Stereo | 5.1 | 7.1 |
| --- | --- | --- | --- |
| Standard | 96 kbps | 256 kbps | 450 kbps |
| High | 256 kbps | 512 kbps | 768 kbps |
| Maximum | 512 kbps | 768 kbps | 1024 kbps |

**Host default** uses the host's setting. With other hosts, **High** and
**Maximum** request high-quality surround sound even below the 15 Mbps video
bitrate Moonlight otherwise requires; other settings use Moonlight's usual
audio.

## Microphone

Turn on **Send microphone to the host** in Settings (or `--microphone`) to talk
in games and chat apps on the host. Pyrolight sends your default microphone,
encrypted, to hosts that offer a microphone stream: Pyroshine, Rubylight and
Apollo, the same stream Nightjar uses. Other hosts stream normally without it.

While streaming, **Ctrl+Alt+Shift+U** mutes and unmutes it; the stream shows
**Microphone muted** while it is off. Muting closes the microphone, so the
system's recording indicator shows exactly when it is in use. A microphone that
is unplugged mutes itself; the shortcut tries it again.

**Microphone echo cancellation** keeps the host's sound, played from your
speakers, out of the microphone. **Automatic** and **On** record from the
system's echo-cancelled microphone when there is one: on Linux, load PipeWire's
`libpipewire-module-echo-cancel` or PulseAudio's `module-echo-cancel`. **Off**
sends the default microphone as it is, which suits headphones.

On a Pyroshine host, games and apps in Game Mode use the microphone as their
default input; in Desktop Mode select **Pyroshine client microphone** as the
input.

## Streaming profiles

Use the **Streaming Profile** section of Settings to create, duplicate, rename,
delete, and switch between named profiles. A profile stores stream settings such
as resolution, frame rate, bitrate, codec, HDR, V-Sync, VRR, audio, window mode,
and decoder. Input, interface, and language settings are shared by all profiles.
Settings from before profiles existed become the **Default** profile.

## Steam Deck (PyroDeck)

[PyroDeck](https://github.com/karsyboy/pyrodeck) is a Decky Loader plugin that
adds a **Stream** button to Steam library pages and starts Pyrolight for you. It
uses Pyrolight's pairing and profiles. Install the AppImage on the Deck, then
follow [PyroDeck's setup](https://github.com/karsyboy/pyrodeck#installation).
Developers can find the commands PyroDeck uses in
[client integration](docs/CLIENT_INTEGRATION.md).

## Troubleshooting

- **Burst packet loss on Linux:** high PyroWave bitrates need larger UDP receive
  buffers. Settings shows the current status and offers **Fix it** or **Copy
  command**. See [network tuning](docs/PYROWAVE_NETWORKING.md).
- **Stream stays below the target frame rate:** test with V-Sync disabled to
  see whether presentation is the limit.
- **"The host does not support the selected PyroWave color mode":** the host
  does not offer PyroWave with that HDR/4:4:4 combination. Change those
  settings, or run `pyroshine healthcheck` on the host.
- **Judder with VRR after changing adaptive sync:** some compositors and drivers
  keep a stale VRR state. Sign out and back in (or reboot) after changing the
  display's adaptive sync setting.
- **Linux window or input problems:** check **Settings → Advanced Settings →
  Display backend**, then run `QT_QPA_PLATFORM=xcb pyrolight` to see whether the
  issue is specific to native Wayland.
- **General streaming problems:** see the upstream
  [Moonlight troubleshooting guide](https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting),
  and compare with H.264, HEVC, or AV1 to isolate PyroWave-specific issues.

When [reporting a bug](https://github.com/karsyboy/pyrolight/issues), include
the release, OS, GPU and driver, host version, stream settings, and client and
host logs. On Windows, client logs are in `%TEMP%`; on Linux, run Pyrolight
from a terminal and capture its output.

## Documentation

- [VRR presentation](docs/VRR.md)
- [PyroWave network tuning and calibration](docs/PYROWAVE_NETWORKING.md)
- [Native controllers](docs/NATIVE_CONTROLLERS.md)
- [Linux display backends and AppImage packaging](docs/LINUX_DISPLAY.md)
- [Linux distribution packages](docs/LINUX_PACKAGES.md)
- [Client integration (PyroDeck)](docs/CLIENT_INTEGRATION.md)
- [PyroWave integration architecture](docs/PYROWAVE.md)
- [Building, testing, and releasing](CONTRIBUTING.md)
- [Upstream Moonlight documentation](https://github.com/moonlight-stream/moonlight-docs/wiki)

## AI-assisted development

AI coding tools, including Codex, assist implementation, testing and documentation.
The maintainer directs the project. All changes require the same review,
validation and security standards, whether written by a human or with AI assistance.

## License and credits

Pyrolight is an independent community fork of
[Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt) and is not an
official Moonlight or PyroWave project. It periodically merges upstream client
changes. Its `moonlight-common-c` submodule points to the
[`karsyboy/moonlight-common-c`](https://github.com/karsyboy/moonlight-common-c)
fork, which carries the PyroWave and controller protocol extensions. The codec
is [PyroWave](https://github.com/Themaister/pyrowave), built from Pyroshine's
vendored tree and patch series.

VRR presentation is based on [Nonary](https://github.com/Nonary)'s idea and
reference implementations in [Vibepollo](https://github.com/Nonary/Vibepollo)
and [moonlight-qt](https://github.com/Nonary/moonlight-qt)
([moonlight-stream/moonlight-qt#1956](https://github.com/moonlight-stream/moonlight-qt/pull/1956)).

Licensed under the [GNU General Public License v3](LICENSE). Moonlight's
copyright and license notices are preserved; PyroWave is distributed under its
MIT license, included with packaged runtime components, and bundled dependencies
retain their own licenses.
