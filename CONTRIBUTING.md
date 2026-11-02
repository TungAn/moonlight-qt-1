# Contributing to Pyrolight

Bug fixes, documentation improvements, and focused features are welcome. Report
bugs with the issue template; include the client release, OS, GPU and driver,
host version, stream settings, and client and host logs. Remove credentials,
host addresses, and other private data before sharing logs.

Keep changes focused and explain the resulting behavior and validation in your
pull request. Preserve Moonlight's GPL-3.0 copyright and license notices.
General Moonlight behavior should stay as close to upstream as practical; see
[AGENTS.md](AGENTS.md) for the fork and upstream policy.

## Building from source

Clone with submodules:

```sh
git clone --recurse-submodules https://github.com/karsyboy/pyrolight.git
cd pyrolight
```

### PyroWave dependency

PyroWave support requires the C API 1.1 that Pyroshine's patch series adds to
[upstream PyroWave](https://github.com/Themaister/pyrowave). Build it from the
`vendor/pyrowave` tree of the pinned [Pyroshine](https://github.com/karsyboy/pyroshine) commit:

| Item | Value |
| --- | --- |
| Pyroshine commit | `4f1a90b0cc6d2d58663ee29c8a3b8a80e55f3cc2` |
| C API | `pyrowave-shared` 1.1.0 (checked by `app/app.pro`) |
| Granite | `53090429539bf3404250d18fe58db971f0907c04`, from `checkout_granite.sh` |

Do not substitute unpatched upstream PyroWave or another ABI. Build and install
it without the development applications:

```sh
git clone --filter=blob:none --no-checkout https://github.com/karsyboy/pyroshine.git deps/pyroshine
git -C deps/pyroshine sparse-checkout set vendor/pyrowave
git -C deps/pyroshine checkout 4f1a90b0cc6d2d58663ee29c8a3b8a80e55f3cc2
cp -a deps/pyroshine/vendor/pyrowave deps/PyroWave
(cd deps/PyroWave && ./checkout_granite.sh)
cmake -S deps/PyroWave -B build/pyrowave \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/pyrowave-install" \
  -DPYROWAVE_DEVEL=OFF -DPYROWAVE_UTILS=OFF
cmake --build build/pyrowave --parallel
cmake --install build/pyrowave
```

The patch series carries full-resolution 4:4:4 payload allocation and 200-nit
normalization when converting PQ/scRGB into SDR; `checkout_granite.sh` applies
its Granite patches. The Pyroshine host pins a newer revision of the same fork; the
two pins need not match, but the client must decode the host's block-format
family (`186f0393`). See [PyroWave integration](docs/PYROWAVE.md#dependency-and-build-contract)
before changing the pin.

### Linux

Install Qt 6, CMake, Vulkan development tools, FFmpeg, libplacebo, SDL2,
SDL2_ttf, OpenSSL, Opus, and the VA-API/VDPAU/X11/Wayland development packages
for your distribution. With PyroWave installed as above:

```sh
export PKG_CONFIG_PATH="$PWD/build/pyrowave-install/lib/pkgconfig:$PKG_CONFIG_PATH"
qmake6 moonlight-qt.pro CONFIG+=enable-pyrowave
make -j"$(nproc)" release
```

Omit `CONFIG+=enable-pyrowave` to build without PyroWave.

The release AppImage is built in an Ubuntu 22.04 userspace; the complete
dependency list is in [build-appimage.yml](.github/workflows/build-appimage.yml).
In that environment, build PyroWave and the AppImage with:

```sh
PYROWAVE_SOURCE="$PWD/deps/PyroWave" scripts/build-pyrowave-appimage.sh
```

The AppImage includes native Wayland and X11. It never bundles libwayland or
libva in `usr/lib`; the build stages host-preferred fallbacks and ends with
`scripts/check-appimage-display-backends.sh`. See
[Linux display backends](docs/LINUX_DISPLAY.md) for the packaging boundary.

The `.deb`, `.rpm` and Arch packages are built from the extracted AppImage with
[nfpm](https://nfpm.goreleaser.com/) (the version pinned in
[build-appimage.yml](.github/workflows/build-appimage.yml)):

```sh
./Pyrolight-*.AppImage --appimage-extract
VERSION=6.2.1 scripts/build-linux-packages.sh squashfs-root build/packages
```

See [Linux packages](docs/LINUX_PACKAGES.md).

### Pacman repository

The [`[pyrowave]` pacman repository](https://github.com/karsyboy/pyrowave-packages) publishes
`pyrolight-bin`, which repackages the newest stable release's Arch package, and
`pyrolight-prerelease-bin`, which follows the newest release including
prereleases. After a release or prerelease is published, the release workflow's
`pacman` job starts that repository's **Publish** workflow, so a release needs
no further step. The job needs the
`PACKAGES_DISPATCH_TOKEN` secret: a fine-grained token with access to
`pyrowave-packages` only and the **Actions: Read and write** permission. If the
job fails, run **Publish** in `pyrowave-packages` by hand.

When changing the `archlinux` dependencies in `app/deploy/linux/nfpm.yaml`,
update both PKGBUILDs there to match.

### Windows x64

Install Visual Studio 2022, Qt 6.10.2 with the MSVC 2022 x64 kit, CMake, Git
Bash, and 7-Zip. From a Qt-enabled PowerShell, download dependencies and build
PyroWave as above (run the clone, Granite checkout and patch steps in Git Bash),
adding `-A x64` to the CMake configure step and `--config Release` to the build
and install steps. Then:

```powershell
./setup-deps.ps1 -Architecture x64
$env:PYROWAVE_PREFIX = "$PWD/build/pyrowave-install"
$env:PYROWAVE_SOURCE = "$PWD/deps/PyroWave"
cmd /c scripts\build-arch.bat Release
cmd /c scripts\generate-bundle.bat Release
```

`build-arch.bat` enables the integration when `PYROWAVE_PREFIX` is set and
stages the runtime DLL and license. See [release.yml](.github/workflows/release.yml)
for the exact CI sequence.

## Validation

[CI](.github/workflows/ci.yml) runs the client unit tests, the common-c
protocol tests and the Qt tests below on every push and pull request. It also
compile-checks Linux builds with and without PyroWave (plus the renderer smoke
test) against Ubuntu 24.04 packages, and checks that every copy of the PyroWave
pin agrees. Run the relevant suites locally before pushing; CI does not run the
renderer smoke test, the HTTPS harness or a Windows build.

Client unit tests use SDL2 and libplacebo through pkg-config. Qt6 Core/Gui enables
production input fixtures; Linux compositor/UI tests also use xcb, xau, Python3
and Xvfb. CI installs these dependencies:

```sh
cmake -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

They cover [controller ownership and Qt navigation](docs/CONTROLLER_INPUT_OWNERSHIP.md#validation),
native controller identities, protocol capabilities and normalized paddle press/release, PyroWave color
mapping, frame validation, record parsing and recovery, bandwidth math,
Linux display backend selection, golden bitstreams from both the client and
host forks, and VRR presentation (`vrr-timing` simulates the timing controller
under jitter, stalls, loss, rate changes, RTP wrap and reconnects; `vrr-worker`
runs the threaded pacing worker against a fake presenter). When Qt5, OpenSSL and a
built `moonlight-common-c` library are available, the project also builds
`pyrowave-https`; run its TLS calibration harness with:

```sh
python3 tests/pyrowave_https.py build/tests/pyrowave-https
```

Protocol tests live in the `moonlight-common-c` submodule:

```sh
cmake -S moonlight-common-c/moonlight-common-c -B build/common-c-tests \
  -DPYROWAVE_PROTOCOL_TESTS=ON -DCONTROLLER_PROTOCOL_TESTS=ON -DAUDIO_QUALITY_TESTS=ON \
  -DMICROPHONE_TESTS=ON
cmake --build build/common-c-tests
ctest --test-dir build/common-c-tests --output-on-failure
```

Streaming-profile persistence and settings registry validation have a Qt test:

```sh
qmake6 app/tests/streamingpreferences_test.pro -o build/prefs-test/Makefile
make -C build/prefs-test
./build/prefs-test/streamingpreferences-test
```

Local integration IPC and cross-process revision checks have a Qt test:

```sh
qmake6 app/tests/settingsapi_test.pro -o build/settingsapi-test/Makefile
make -C build/settingsapi-test
./build/settingsapi-test/settingsapi-test
```

The UDP calibration receiver (reply parsing, the control-datagram vector shared
with Pyroshine, loss/gap/FEC/queueing statistics) has a Qt test:

```sh
qmake6 app/tests/udpprobe_test.pro -o build/udpprobe-test/Makefile
make -C build/udpprobe-test
./build/udpprobe-test/udpprobe-test
```

The PyroWave renderer smoke test needs a Vulkan-capable display; see
[renderer smoke test](docs/PYROWAVE.md#renderer-smoke-test). Run it with
`SDL_VIDEODRIVER=wayland` and `SDL_VIDEODRIVER=x11` after window-system changes.

To check the display backend packaging of an AppImage, extract it and run
`scripts/check-appimage-display-backends.sh squashfs-root`. Runtime checks for
native Wayland, X11 and XWayland are listed in
[Linux display backends](docs/LINUX_DISPLAY.md#validation).

Automated tests do not establish live HDR output, sustained bitrate, NIC
behavior, reconnects or controller hardware. For decoder, renderer, network or
protocol changes, test against a real Pyroshine host and report the hardware,
platforms and checks you could not run.

## Branding

Pyrolight uses Pyroshine's orange palette: `#C43E0C` for the toolbar and
`#FFB59E` for controls and section headings on dark surfaces. Defaults live in
`app/main.cpp`; headings follow `Material.accent`. The existing Material primary
and accent environment overrides remain supported.

Product names and release assets use Pyrolight. Upstream project paths, library
names, translation catalogs and compatibility identities stay intact to keep
upstream merges small and retain settings, paired hosts and installer upgrades.
`app/brandtranslator.h` rebrands translated client references without rewriting
upstream source strings. Keep upstream help links and external tool names.

On Linux the icon is installed as `pyrolight` (`Icon=pyrolight` in the desktop
entry), not `moonlight`: icon themes such as Papirus ship a `moonlight` icon
that would otherwise show Moonlight's logo. `scripts/check-linux-icons.sh`
verifies an AppDir or package staging tree.

The logo follows Pyroshine's flame-and-orbit style with a crescent center. To
regenerate Windows, macOS, Steam Link and Linux/Qt icons from the source PNG:

```sh
# Requires Pillow.
python3 scripts/generate-branding.py
qmake6 app/tests/brandtranslator_test.pro -o build/brand-test/Makefile
make -C build/brand-test
./build/brand-test/brandtranslator-test
```

The SVG embeds the icon PNG so Qt's stream-window renderer and installed desktop
icons use the same artwork. Legacy `moonlight` asset filenames are intentional.
The branding test checks the English fallback, an upstream French catalog and
Qt SVG rendering. Check installer upgrades on Windows when changing packaging.

## Publishing a release

Push an annotated tag in the form `vMAJOR.MINOR.PATCH` (optionally `.BUILD`):

```sh
# Example only: replace with the version being released.
git tag -a vX.Y.Z -m "Pyrolight vX.Y.Z"
git push origin vX.Y.Z
```

The [release workflow](.github/workflows/release.yml) derives the application
version from the tag, runs the [CI](.github/workflows/ci.yml) suites on the
tagged revision, builds the Windows installer and portable ZIP and the
Linux AppImage against the pinned PyroWave revision, verifies the PyroWave
runtime is bundled, and, only after the tests pass, publishes a GitHub Release
with `SHA256SUMS` and generated notes. Test installation of each asset afterwards.

### Prereleases

Prerelease tags take the form `vMAJOR.MINOR.PATCH-prerelease` or
`vMAJOR.MINOR.PATCH-prerelease.N` (no `.BUILD` component). Set
`app/version.txt` to the same version, for example `6.2.8-prerelease`:

```sh
git tag -a v6.2.8-prerelease -m "Pyrolight v6.2.8-prerelease"
git push origin v6.2.8-prerelease
```

The [prerelease workflow](.github/workflows/prerelease.yml) runs the release
workflow for these tags (the release workflow itself ignores them) and marks
the GitHub release as a prerelease. The Linux packages carry the version in
each format's prerelease form (`6.2.8~prerelease` for deb/rpm,
`6.2.8prerelease` for pacman), so the release that follows upgrades them. Windows
version resources hold only the numeric part (`6.2.8`); the installer upgrades a
prerelease of the same version.
