# Working in Pyrolight

Pyrolight is a fork of the Moonlight Qt game-streaming client. It
keeps upstream Moonlight's behavior and adds a PyroWave decode path, PyroWave
network tools, streaming profiles and DualSense Edge identity, primarily for
[Pyroshine](https://github.com/karsyboy/pyroshine) hosts. This file is a
repository map and engineering contract for coding agents. It points to the
authoritative documents; read the ones relevant to the change.

Related repositories:

- [Pyroshine](https://github.com/karsyboy/pyroshine): the host. Its
  `docs/PYROWAVE.md` and `docs/PYROWAVE_COMPATIBILITY.md` define the server side
  of the PyroWave protocol; changes to capability bits, SDP attributes, framing
  or the bandwidth probe must stay compatible with it.
- [`karsyboy/moonlight-common-c`](https://github.com/karsyboy/moonlight-common-c):
  the protocol library fork used as the `moonlight-common-c/moonlight-common-c`
  submodule.
- [`Themaister/pyrowave`](https://github.com/Themaister/pyrowave): the codec, built from
  Pyroshine's `vendor/pyrowave` (upstream plus a numbered patch series).

## Working approach

- Inspect the relevant code, tests and guide before editing. Treat the current
  implementation as the source of truth; documents can be stale.
- Prefer root-cause fixes over symptom workarounds, and explain the cause.
- Keep changes focused. Avoid unrelated refactors, renames and formatting churn,
  especially in upstream code: every unnecessary edit makes future upstream
  merges harder.
- Preserve existing behavior unless the task explicitly changes it. If you find
  an unrelated defect, report it instead of silently redesigning behavior.

## Repository map

| Path | Responsibility |
| --- | --- |
| `app/streaming/session.cpp` | Stream launch, format selection, decoder creation, PyroWave launch checks |
| `app/streaming/video/pyrowave*.{h,cpp}` | PyroWave decoder, native frame and color rules, record-framed adapter |
| `app/streaming/video/ffmpeg-renderers/plvk.*` | libplacebo Vulkan renderer shared with the PyroWave decoder; Linux VRR presenter |
| `app/streaming/video/ffmpeg-renderers/pacer/vrr*`, `pacer/vrr/`, `app/streaming/vrrratepolicy.*` | VRR timing controller, pacing worker, diagnostics and stream-rate choices |
| `app/streaming/input/gamepad.cpp`, `controllerinputstate.h`, `gamescopeinputfocus.*`, `gamepadidentity.h`, `gamepadfeedback.h` | Controller ownership/rearming, Gamescope global input owner, arrival, button mapping, DualSense Edge identity, host feedback slot bounds |
| `app/settings/streamingpreferences.*`, `bitratecalculator.h` | Preferences, streaming profiles, PyroWave availability and default bitrate |
| `app/backend/networkbuffers.*`, `pyrowavecalibrator.*`, `pyrowavebandwidth.h`, `udpprobe*.{h,cpp}`, `nvhttp.cpp` | Receive-buffer diagnostics/remediation, bandwidth calibration (Pyroshine UDP protocol, HTTPS probe fallback) |
| `app/gui/SettingsView.qml` | Settings UI, including profiles, PyroWave and network tools |
| `app/gui/sdlgamepadkeynavigation.*` | Qt menu controller navigation, shared Gamescope ownership and held-control rearming |
| `app/displaybackend.*` | Linux display backend policy: Qt platform preference, SDL driver matching, Settings diagnostic |
| `app/cli/commandlineparser.cpp`, `settingsapi.*` | CLI options, stable-ID launch and same-user settings IPC; contract in `docs/CLIENT_INTEGRATION.md` |
| `moonlight-common-c/moonlight-common-c/` | Submodule fork: RTSP/SDP, RTP/FEC reassembly, PyroWave dialects, controller protocol |
| `tests/` | Standalone CMake unit tests for fork code |
| `app/tests/` | Renderer smoke test, streaming-preferences, brand translator and UDP calibration Qt tests |
| `scripts/build-pyrowave-appimage.sh` | Pinned PyroWave AppImage build |
| `scripts/build-appimage.sh`, `scripts/check-appimage-display-backends.sh`, `app/deploy/linux/*-probe.c` | AppImage packaging, host-library probes and packaging checks |
| `scripts/build-linux-packages.sh`, `app/deploy/linux/nfpm.yaml`, `scripts/check-linux-icons.sh` | `.deb`/`.rpm`/Arch packages built from the AppImage payload; Linux icon checks |
| `.github/workflows/` | `ci.yml` (tests and Linux build checks on pushes and pull requests), `release.yml` (tag-triggered Windows and Linux release, published only after `ci.yml` passes on the tag; starts the pacman repository update), `build-appimage.yml`, and `upstream-common-c.yml` (daily upstream common-c watch) |
| `wix/`, `app/deploy/`, `scripts/` | Upstream packaging, with fork branding |

Everything else under `app/` is upstream Moonlight code.

## Choose the source of truth

| Change | Consult |
| --- | --- |
| User-visible setup, install, usage | `README.md` |
| Build, tests, release | `CONTRIBUTING.md`; `.github/workflows/` for exact CI steps |
| Decoder, renderer, color, bitrate, PyroWave pin | `docs/PYROWAVE.md` |
| VRR presentation, timing controller, `clientVrrRequested` | `docs/VRR.md` |
| Dialects, receive buffers, calibration | `docs/PYROWAVE_NETWORKING.md` |
| Linux Wayland/X11 selection, AppImage graphics-stack packaging | `docs/LINUX_DISPLAY.md` |
| Linux distribution packages, dependencies, icon name | `docs/LINUX_PACKAGES.md` |
| Common-c protocol extension | `moonlight-common-c/moonlight-common-c/docs/pyrowave.md` |
| Audio quality request (`x-moonshine-audio.quality`) | `moonlight-common-c/moonlight-common-c/docs/audio-quality.md` |
| Client microphone (Apollo's `streamid=mic`, as Nightjar sends it) | `moonlight-common-c/moonlight-common-c/docs/microphone.md`; capture in `app/streaming/audio/microphone.*` |
| Host-side protocol and probe | Pyroshine `docs/PYROWAVE.md`, `docs/PYROWAVE_COMPATIBILITY.md`, `docs/DUALSENSE_EDGE.md` |
| Controller input ownership and Steam menus | `docs/CONTROLLER_INPUT_OWNERSHIP.md` |
| General Moonlight behavior | Upstream code and the [Moonlight wiki](https://github.com/moonlight-stream/moonlight-docs/wiki) |

## Fork functionality that must not regress

- **PyroWave is explicit.** It is offered only when the decoder probe succeeds,
  never chosen automatically, and never falls back to another codec, CPU
  decoding or software Vulkan. Unsupported combinations fail with a launch error.
- **Native wire-v1 is strict.** Native frames never pass through the record
  adapter or framing inference; record mode is used only when negotiated.
- **Decode stays on the GPU.** Decoder and libplacebo share one Vulkan device;
  decoded planes never touch CPU memory; the single-slot mailbox and render
  thread (with VRR: decode mailbox, bounded plane-set pool and pacing worker)
  bound latency; no per-frame allocation in steady state.
- **Visible dimensions** are used for textures and crop; wavelet padding never
  reaches the screen.
- **Network tools are opt-in.** No privileged change (sysctl, NIC settings)
  happens without the user pressing the button; calibration never changes
  settings automatically.
- **VRR presentation** stays bounded: at most 4 waiting frames plus the one
  presented, stale frames replaced rather than buffered, VRR qualified only on
  a known refresh rate, and any unmet requirement keeps synchronized fixed
  pacing. Non-VRR sessions keep the classic pacer unchanged.
- **Streaming profiles** keep stream settings per profile and input, interface
  and language settings global; existing flat settings are captured once as
  the Default profile.
- **DualSense Edge** keeps the PlayStation family, sends capability `0x0200`,
  and maps the paddles in SDL2 canonical order.
- **Fork identity**: application name, release asset names, the disabled
  upstream update checker (`MOONLIGHT_QT_PYROWAVE`), and bundled PyroWave
  runtime and license in every release asset.
- A build without `CONFIG+=enable-pyrowave` must still compile and run the
  standard codecs.
- **Linux display backends**: Qt chooses the platform (native Wayland preferred,
  `xcb` fallback) and SDL always follows it; both are built into the AppImage.
  libwayland and libva are host graphics-stack libraries, never bundled in
  `usr/lib`; `scripts/check-appimage-display-backends.sh` must pass.
- **Linux icon and packages**: the desktop entry uses `Icon=pyrolight` and the
  icon is installed as `pyrolight` (never `moonlight`, which icon themes
  override); `scripts/check-linux-icons.sh` must pass. Every release ships the
  AppImage plus `.deb`, `.rpm` and Arch packages of the same payload.

## Compatibility

- Wire behavior must interoperate with the current Pyroshine release and stay
  compatible with upstream-style hosts (Sunshine, GameStream) for standard
  codecs. Capability bits and SDP attributes are shared contracts; change them
  only together with the host and common-c.
- The PyroWave pin is a Pyroshine commit (`4f1a90b`, C API 1.1.0) whose
  `vendor/pyrowave` every workflow builds; it appears in every workflow,
  `CONTRIBUTING.md` and `docs/PYROWAVE.md`; keep them identical (CI checks).
  Change PyroWave only through Pyroshine's `vendor/pyrowave-patches/`. The host
  may build a newer series; the bitstream family (`186f0393`) must stay decodable.
- Fork code must compile on Windows (MSVC, `min`/`max` macros) and Linux; tests
  include a Windows-macro build of the frame rules.
- Changes in `moonlight-common-c` are made in the `karsyboy/moonlight-common-c`
  fork and then picked up by updating the submodule pointer here. Build the
  application and common-c together; `DECODE_UNIT` layout is shared.

## Validation

See [CONTRIBUTING.md](CONTRIBUTING.md#validation) for full commands. CI
(`ci.yml`) runs the unit, protocol and Qt tests and compile-checks Linux builds
with and without PyroWave; run the relevant suites locally before pushing:

```sh
cmake -S tests -B build/tests && cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

- Protocol or framing changes: also run common-c's suite with
  `-DPYROWAVE_PROTOCOL_TESTS=ON -DCONTROLLER_PROTOCOL_TESTS=ON -DAUDIO_QUALITY_TESTS=ON -DMICROPHONE_TESTS=ON`.
- Preferences or profile changes: run `app/tests/streamingpreferences_test.pro` and `app/tests/settingsapi_test.pro`.
- Decoder or renderer changes: build with `CONFIG+=enable-pyrowave` and run the
  renderer smoke test on a Vulkan-capable display.
- Documentation-only work needs Markdown, relative-link and path checks and
  `git diff --check`, not a full build.
- Hardware behavior (HDR output, sustained bitrate, NIC remediation, reconnects,
  controllers) needs a real host and client. Report tested hardware and checks
  not performed; a build or unit test is not hardware validation.

## Documentation

This repository follows the same documentation standard as Pyroshine:

- **User-facing** (`README.md`): task-oriented, concise, with requirements,
  copyable commands and links to deeper material. Upstream Moonlight usage stays
  in upstream documentation; document only fork behavior here.
- **Technical** (`docs/`, `CONTRIBUTING.md`, this file): current design,
  responsibilities, contracts and validation. Describe the resulting design, not
  its history; release notes are generated from commits by the release workflow.
  Put design first and validation procedures in a final section.
- Style: one `#` title, descriptive `##` headings, short paragraphs, tables for
  reference data, `sh`-tagged code blocks, relative links, upper-case file names
  in `docs/`, and no marketing or unsupported claims. Use the same names as
  Pyroshine for shared concepts (PyroWave, native wire-v1, record-framed, HDR10,
  4:2:0/4:4:4, bitstream family, bandwidth probe).

Update the relevant document with any user-visible or contract change. Update
this file when boundaries, commands, document locations or fork invariants change.

## Branding and compatibility identity

The product is **Pyrolight**, with `pyrolight` on Linux and `Pyrolight.exe` on
Windows. Keep upstream source/project paths (`moonlight-qt.pro`, `wix/Moonlight/`),
submodule names, protocol identifiers, the `MOONLIGHT_QT_PYROWAVE` guard, desktop
and bundle IDs, QSettings organization/application keys and WiX upgrade/state
keys unchanged. These are compatibility and merge boundaries, not display names.

`app/brandtranslator.h` applies the client name after translation lookup, with
an English fallback. Preserve upstream QML/tr() source keys and translation
catalogs; the external Moonlight Internet Hosting Tool keeps its real name.
`assets/logo-no-text.png` is the Pyrolight source mark;
`scripts/generate-branding.py` regenerates all platform icons at their upstream
asset paths. Those paths contain Pyrolight artwork despite their legacy names.
See [CONTRIBUTING.md](CONTRIBUTING.md#branding) for regeneration and checks.

## Upstream synchronization

Upstream is [Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt),
configured as the `upstream` remote (branch `master`); `origin` is the fork.
Upstream releases are merged as merge commits (for example "Merge upstream
moonlight-qt master (v6.2.0)"). The `moonlight-common-c` submodule's upstream is
[moonlight-stream/moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c);
`qmdnsengine` and `SDL_GameControllerDB` are unmodified upstream submodules.
Verify remotes with `git remote -v` and `.gitmodules` rather than assuming.
Dependabot (`.github/dependabot.yml`) proposes submodule and action updates;
`.github/workflows/upstream-common-c.yml` keeps an issue open listing upstream
common-c commits the submodule pin lacks. Neither applies upstream changes.

Staying reasonably compatible with upstream is an ongoing goal. Bug fixes,
security fixes, performance, compatibility and maintenance improvements, useful
features and architectural improvements are worth bringing in when they fit the
fork and do not break, remove or undermine fork-specific functionality.

Synchronization is never automatic:

1. Identify the upstream repository and branch from the remote configuration,
   `.gitmodules` and these documents.
2. Determine how the fork differs from upstream in the affected areas
   (`git merge-base HEAD upstream/master`, `git diff`).
3. Review the upstream changes being considered.
4. Evaluate conflicts and regression risks, including fork branding, workflows,
   settings UI and the submodule pointer.
5. Decide whether the changes provide meaningful value to the fork.
6. Identify fork-specific adaptations required.

**Explicit approval is required before applying anything.** Agents may fetch,
inspect and compare upstream and prepare a recommendation summarizing the
relevant changes, why they are useful, affected areas, expected conflicts,
fork behavior at risk and proposed validation. No agent may merge, cherry-pick,
rebase onto, copy, port or otherwise apply upstream changes into this fork or
its submodule fork without explicit approval from the repository owner, even
when the change is small, documentation-only, conflict-free or obviously
beneficial.

**Upstream repositories are read-only.** Never push to, commit to, open or
update pull requests against, modify branches or settings of, merge into, or
create releases in `moonlight-stream/moonlight-qt`,
`moonlight-stream/moonlight-common-c` or any other upstream repository. All work
stays in the user's forks unless the owner explicitly instructs otherwise.

**Fork functionality takes priority.** Do not remove or weaken the behavior
listed above merely to reduce divergence. When upstream and fork requirements
conflict, understand why the fork differs, preserve intentional behavior, adapt
the upstream change cleanly where possible, and report unavoidable divergence.
