# Linux display backends

Pyrolight runs on native Wayland when a Wayland compositor is usable and on
X11 otherwise, including XWayland. Qt chooses the window system, SDL follows
it, and the Linux AppImage ships both paths. This document describes the
selection, the AppImage packaging boundary for the host graphics stack, and the
diagnostics.

`settings`, `list` (including artwork and network tools), and CLI help/version
queries do not initialize a display. The backend selection below applies to the
GUI, pairing, streaming and interactive quit commands.

## Backend selection

Qt selects its platform plugin when the `QGuiApplication` is created. Qt 6.3+
tries `wayland` before `xcb` whenever `WAYLAND_DISPLAY` is set or
`XDG_SESSION_TYPE=wayland`. Qt 6.0–6.2, which the AppImage ships, puts `xcb`
first on GNOME. `DisplayBackend::preferNativeWayland()` closes that gap: on
Qt 6.0–6.2, when a Wayland session is detected and `QT_QPA_PLATFORM` is unset,
it sets `QT_QPA_PLATFORM=wayland;xcb` (or `wayland` without `DISPLAY`). The list
keeps Qt's own fallback: if the Wayland plugin cannot load or connect, Qt
continues with `xcb`.

After Qt starts, `DisplayBackend::current()` classifies the platform actually
in use, and `main.cpp` forces SDL onto the matching video driver. Qt and SDL
never use different window systems.

| Qt platform | X server | Display backend | SDL video driver |
| --- | --- | --- | --- |
| `wayland` | — | Native Wayland | `wayland` |
| `xcb` | advertises `XWAYLAND` | XWayland | `x11` |
| `xcb` | no `XWAYLAND` extension | X11 | `x11` |
| `eglfs`, `linuxfb` | — | KMSDRM | `kmsdrm` |

Xwayland 21.1 and later advertise the `XWAYLAND` X extension, which is also
how SDL detects it. For older Xwayland, an X server next to a reachable Wayland
compositor is treated as XWayland. `XDG_SESSION_TYPE` alone never decides the
backend.

| Situation | Result |
| --- | --- |
| Wayland session, Wayland plugin works | Native Wayland |
| `WAYLAND_DISPLAY` set but the connection fails, or the Wayland plugin or its libraries cannot load | Qt falls back to `xcb`: XWayland (or X11) |
| X11 session | X11 |
| `QT_QPA_PLATFORM` set by the user | Qt uses it; the classification follows the result |
| Neither Wayland nor X11 reachable | Upstream behavior: EGLFS is requested. The AppImage does not ship EGLFS, so Qt exits listing the available plugins (`wayland-egl, wayland, xcb`). |

Rendering, decoding and input use the same code on both window systems. SDL
creates the Vulkan surface (`VK_KHR_wayland_surface` or
`VK_KHR_xlib_surface`), and libplacebo presents PyroWave and FFmpeg frames on
it. VA-API opens the display through `vaGetDisplayWl()` on Wayland and
`vaGetDisplay()` on X11. On Wayland, the pacer uses Wayland frame callbacks
(`WaylandVsyncSource`); X11 has no V-sync source, as before.

## AppImage packaging boundary

The AppImage is built in Ubuntu 22.04 and runs against the host's EGL, Vulkan
drivers and Mesa. Those drivers are linked against the host's libwayland, so a
bundled, older libwayland that shadows the host copy can make the host
`libEGL_mesa.so` fail to load, breaking EGL even on X11. The packaging therefore
splits libraries as follows.

| Library | Source | Reason |
| --- | --- | --- |
| `libwayland-client`, `-cursor`, `-egl`, `-server` | Host; never in `usr/lib` | Host graphics stack. linuxdeploy's blocklist skips them, `--exclude-library` repeats it, and the Qt Wayland plugins are deployed by linuxdeploy itself because the pinned Qt plugin ignores exclusions. |
| `libwayland-client` fallback | `opt/wayland-fallback`, used only if the host has none | Pyrolight links it directly. `wayland-probe` (linked with `-z now`) checks that the host copy provides every imported symbol; if not, AppRun prepends the staged copy. A host without libwayland-client has nothing that could need a newer one. |
| `libdecor-0`, `libxkbcommon` for SDL | Host (`dlopen`), or Qt's bundled `libxkbcommon` once loaded | SDL loads Wayland, libdecor and X11 libraries at runtime. libdecor plugins are host-specific. |
| `libva`, `libva-x11`, `libva-wayland` | Host when `libva-probe` passes, otherwise `opt/libva-fallback` as one set | `libva-wayland` must match `libva`. It is a separate, often uninstalled package on Debian and Ubuntu GNOME desktops, so Pyrolight loads it with `dlopen()` instead of linking it: a host without it keeps its own libva and only loses VA-API on Wayland. |
| EGL, GL, GBM, DRM, Vulkan ICDs | Host | Unchanged. |
| Qt platform plugins | Bundled | `xcb` (with `xcbglintegrations`), `wayland` (generic), `wayland-egl`, the `wayland-egl` client buffer integration, `xdg-shell` and the `bradient` decorations. |

The `.deb`, `.rpm` and Arch packages install this same payload in
`/opt/pyrolight`, so the boundary and probes are identical; see
[Linux packages](LINUX_PACKAGES.md).

AppRun checks libwayland-client before libva because `libva-wayland` needs it.
`scripts/check-appimage-display-backends.sh` verifies this boundary on every
release build.

## Diagnostics

Startup logs the decision, for example:

```text
Display backend: Native Wayland (Qt platform: wayland, SDL video driver: wayland)
Display backend: XWayland (Qt platform: xcb, SDL video driver: x11)
```

Qt 6.0–6.2 also logs `Preferring native Wayland over the Qt 6.2.4 default`
when it applies `QT_QPA_PLATFORM`. A stream logs `SDL video driver:`,
`Vulkan presentation surface:` and, with frame pacing on Wayland,
`Frame pacing V-sync source: Wayland frame callbacks`.

**Settings → Advanced Settings → Display backend** shows Native Wayland, X11,
XWayland or KMSDRM, read from `SystemProperties.displayBackend`. It uses the
same `DisplayBackend::current()` value that selected the SDL driver, so it
reports the backend in use after any fallback. It is hidden on Windows and
macOS.

To force X11/XWayland, run with `QT_QPA_PLATFORM=xcb`; to force Wayland, use
`QT_QPA_PLATFORM=wayland`.

With `--integration-events`, `transport_ready` precedes SDL creation. Window
observations and `streaming` report SDL shown/focus and initialized-renderer
state separately, including a native X11 ID and inherited SteamGameId where
available. Integrations can correlate these with compositor focus without
forcing XWayland or claiming transport readiness proves visible video. See
[Local client integration](CLIENT_INTEGRATION.md).

## Known limitations

- The AppImage ships Qt 6.2. Its Wayland plugin has no fractional-scale
  protocol, so the Qt UI renders at the next integer scale and the compositor
  downsamples it on fractional scales. On GNOME, Qt draws its own (bradient)
  window decorations. The SDL stream window is unaffected.
- The bundled libxkbcommon 1.4 logs `unrecognized keysym` errors for newer host
  Compose files under native Wayland. Stream keyboard input is unaffected.
- With `opt/wayland-fallback` in use on a host whose Mesa needs a newer
  libwayland (an artificial setup), EGL fails while Vulkan rendering still
  works. Real hosts without libwayland-client have no such Mesa.

## Validation

Unit tests cover the selection policy, classification and display names
(`tests/display_backend.cpp`):

```sh
cmake -S tests -B build/tests && cmake --build build/tests
ctest --test-dir build/tests --output-on-failure -R display-backend
```

Check a built AppDir or an extracted AppImage:

```sh
./Pyrolight-*.AppImage --appimage-extract
scripts/check-appimage-display-backends.sh squashfs-root
```

Manual runtime checks (run the AppImage from a terminal and read the log and
**Settings → Advanced Settings → Display backend**):

| Environment | Command | Expected |
| --- | --- | --- |
| Wayland session (KDE, GNOME, wlroots) | `./Pyrolight-*.AppImage` | Native Wayland; `SDL video driver: wayland` |
| Wayland session, forced X11 | `QT_QPA_PLATFORM=xcb ./Pyrolight-*.AppImage` | XWayland |
| Broken Wayland socket | `WAYLAND_DISPLAY=missing ./Pyrolight-*.AppImage` | Qt cannot load `wayland`, continues with `xcb`: XWayland |
| X11 session | `./Pyrolight-*.AppImage` | X11 |

For streaming, test H.264, HEVC, AV1 and PyroWave (SDR/HDR × 4:2:0/4:4:4) in
windowed and fullscreen modes on native Wayland and X11, with V-Sync and frame
pacing on and off, and check mouse capture, keyboard and controllers. Confirm
`Vulkan presentation surface: VK_KHR_wayland_surface` and, with frame pacing,
`Frame pacing V-sync source: Wayland frame callbacks`. HDR presentation on
Wayland depends on the compositor's color-management support.
