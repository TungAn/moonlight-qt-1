# Controller input ownership

Steam Deck system menus can retain Pyrolight's local SDL/Qt window focus.
[Gamescope publishes global input and graphics ownership](https://github.com/ValveSoftware/gamescope/blob/master/src/steamcompmgr.cpp)
on its root Xwayland server, which can differ from the game's `DISPLAY`.
Both streaming and Qt menu navigation observe that ownership before handling
controller input, including when Steam Input is disabled.

## Design

- `streaming/input/controllerinputstate.h` defines eligibility, stale-event
  filtering and held-control rearming. Desktop streaming honors background
  gamepad preferences; Qt navigation always requires window focus. Gaming Mode
  also requires stream window focus/visibility.
- `streaming/input/gamescopeinputfocus.*` discovers a matching local root using
  Gamescope launch hints, authenticated X sockets and the process/Steam app ID.
  It reads `GAMESCOPE_FOCUSED_APP` and `GAMESCOPE_FOCUSED_APP_GFX` on a worker
  thread, sleeping on property notifications. A loss counter preserves rapid
  open/close transitions. Socket cancellation bounds shutdown and interrupts
  stalled X setup; the input thread never waits for X replies.
- `streaming/input/input.*`, `gamepad.cpp` and `session.cpp` gate controller
  buttons, axes, motion, touch, shortcuts and emulated mouse input. Suspension
  sends neutral state without changing attached slots/masks. Mouse timers only
  post SDL events; transmission stays on the session thread. Axis batching
  preserves queue ordering across ownership events.
- `gui/sdlgamepadkeynavigation.*` uses the same observer for host/app/settings
  navigation, including Linux menus where Qt owns video and SDL video is not
  initialized. Its existing 50 ms timer continues controller bookkeeping while
  compositor ownership is suspended. Accepted navigation actions send paired
  Qt key events, preventing partially pressed controls across menu transitions.

On resume, held buttons require release and sticks/triggers require centering
before fresh activity. Old queued events are discarded. Hotplug and host
feedback remain available while suspended. UI observers stop before streaming
starts and are recreated when returning to the menus.

Gamescope discovery applies to Linux sessions with `SteamGamepadUI=1`,
`GAMESCOPE_WAYLAND_DISPLAY`, `STEAM_MULTIPLE_XWAYLANDS` or
`XDG_CURRENT_DESKTOP=gamescope`. Native Wayland still needs access to Gamescope's
root X socket and a usable launch identity. `xcb`/`xau` development packages
provide build support. Missing support, inaccessible roots or unmatched identity
produce an explicit fallback warning; SDL/Qt focus checks remain available.
Once adopted, an invalid/disconnected observer suspends input rather than using
stale permission. These compositor properties are not a guaranteed Steam API.

`PYROLIGHT_INPUT_DEBUG=1` enables receipt/transmission and SDL window diagnostics.
The normal logs identify the selected Gamescope display/app and ownership changes.
`PYROLIGHT_GAMESCOPE_DISPLAY=:N` can prioritize a known local root display.

## Validation

```sh
cmake -S tests -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests --output-on-failure
```

Qt6 Core/Gui and SDL2 enable the production input fixtures. Linux compositor/UI
fixtures additionally need `xcb`, `xau`, Python3 and Xvfb. They use two disposable,
authenticated X servers; they never modify the user's compositor properties.

- `controller-input` checks eligibility, timestamps/tick wrap and rearming.
- `controller-input-sdl` intercepts production Limelight calls with virtual
  controllers: neutralization, merged/independent slots, held controls, queue
  ordering, hotplug, mouse, sensors, touch and shutdown.
- `gamescope-input-focus` checks discovery, ownership changes without local focus
  loss, rapid transitions, invalid properties, disconnects and cancellation.
- `gamepad-ui-navigation` checks actual Qt key delivery for host/app/settings
  mappings, ownership suspension, rearming, stale input, hotplug and UI/stream
  handoff. It deliberately leaves SDL video uninitialized.

The owner confirmed the fix works on their Steam Deck in the stream and all
menus with Steam Input disabled. Local fixtures also passed with the bundled
SDL2/SDL3 runtime. The packaged GUI was smoke-tested on isolated X11 with software
rendering. This does not establish every external controller, display backend or
performance scenario.

For hardware regression checks, open Steam and Quick Access and switch apps
while on the host list, app grid, settings and an active stream. Navigate using
buttons/sticks/triggers and confirm the covered screen receives no actions.
Return with controls held, then release/center and verify fresh input works.
Repeat after leaving a stream, with Steam Input enabled/disabled, and with
external/multiple controllers. Check feedback, mouse/touch/motion and stream
continuity. Desktop background-input behavior should remain unchanged.
