# Variable refresh rate presentation

With **Variable refresh rate (VRR)** enabled, Pyrolight decides when each
decoded frame is shown on a VRR display instead of presenting it when decoding
finishes or holding it for a fixed refresh. Frames are scheduled on the host's
own frame timeline, learned from RTP timestamps, plus a small adaptive buffer
that absorbs network, decode and capture jitter. The display then refreshes
when each frame is due.

VRR works with any GameStream-compatible host and with every codec, including
PyroWave. A host that understands `clientVrrRequested` (Pyroshine, Vibepollo)
additionally captures frames as the game presents them; see
[Host interaction](#host-interaction).

## Components

| Path | Responsibility |
| --- | --- |
| `app/streaming/vrrratepolicy.*` | Stream-rate choices and admission |
| `app/settings/vrrtimingoptions.h` | Timing modes and custom values |
| `app/streaming/session.cpp` (`qualifyVrr`) | Session qualification, `clientVrrRequested`, window mode |
| `pacer/vrr/vrrsourceclock.*` | RTP timeline, source-rate fit, clock mapping |
| `pacer/vrr/vrrintervalbuffer.h` | Adaptive playout delay |
| `pacer/vrr/vrrsmoother.h` | Reduce judder |
| `pacer/vrr/vrrtimingcontroller.*` | Per-frame targets, presentation protection, late recovery |
| `pacer/vrrpacingworker.*` | Bounded queue, pacing thread, waits, statistics |
| `pacer/vrrdiagnostics.*` | Overlay and log text |
| `ffmpeg-renderers/plvk.*` | Vulkan presenter |
| `pacer/pacer.cpp`, `pyrowave.cpp` | Integration for FFmpeg codecs and PyroWave |

`pacer/` is `app/streaming/video/ffmpeg-renderers/pacer/`. The controller and
worker depend on no Qt, SDL, FFmpeg or renderer code and are unit tested alone.

## Settings and qualification

| Setting | Values | Effect |
| --- | --- | --- |
| Variable refresh rate (VRR) | off (default) | Requires V-Sync. Stored per streaming profile. |
| VRR timing | Smoothest, Balanced (default), Lowest latency | Preset buffer and quality target, below |
| Customize | four values | Overrides the selected preset |
| Reduce judder | on (default) | Regularizes uneven host frame timing |

| Preset | Largest buffer | On-time target | History | Tolerance |
| --- | --- | --- | --- | --- |
| Smoothest | 4 source frames | 99.99 % | 300 s | 250 µs |
| Balanced | 1 source frame | 99.50 % | 120 s | 500 µs |
| Lowest latency | ½ source frame | 99.00 % | 60 s | 500 µs |

The CLI accepts `--vrr`/`--no-vrr`, `--vrr-timing smoothest|balanced|lowest-latency`
and `--vrr-reduce-judder`/`--no-vrr-reduce-judder` without saving them.

With VRR enabled, the frame-rate list adds two choices per display refresh
rate `r`: `floor(r - r²/3600)` (116 at 120 Hz, 138 at 144 Hz) leaves a margin
that grows with the refresh rate, so a slightly fast source or a late/early
frame pair stays inside the panel's adaptive range; `floor(r/6)*5` (100 at
120 Hz, 120 at 144 Hz) leaves a sixth of the range for late frames to be shown
sooner. Native refresh rates, 30/60 FPS and a saved custom rate stay
available, and toggling VRR never changes the saved frame rate.

A session qualifies when V-Sync is on, the stream window's display reports its
refresh rate (an unknown rate is never assumed to be 60 Hz), the stream frame
rate does not exceed it, and the platform has a VRR presenter (Linux with the
Vulkan renderer). A qualified session prefers the Vulkan renderer (also for
the startup probe, so the color range negotiated with the host matches
playback), streams in desktop fullscreen even when the saved mode is windowed,
and sends `clientVrrRequested=1`. The pacing worker (and, for PyroWave, the
decode thread) must then start; otherwise the session keeps synchronized
presentation with classic frame pacing, enabled even if the frame pacing
checkbox is off. A renderer that already created a Mailbox or Immediate
swapchain recreates it with FIFO first, and the stream fails to start if it
cannot. The log and the performance overlay name the reason. A renderer
recreated after a display change requalifies the VRR request against the new
display, so a request rejected on the launch display (unknown or too low a
refresh rate) qualifies once the window reaches a suitable one, and a
qualified one picks up the new refresh rate. The host keeps the capture mode
negotiated at launch; client pacing works against either. SDL 2 reports no
event for a refresh-rate change on the same monitor, so that case keeps the
previous rate until the renderer is recreated.

## Timing model

```text
target = sourceTime + smoothing + playoutDelay + typicalPreparation
```

- **Source time.** RTP timestamps are unwrapped (32-bit wrap) and mapped to the
  client clock through the earliest recent arrival: the windowed minimum of
  `ready - rtp` over three seconds, adopted at once while an epoch warms up
  (64 frames) and afterwards slewed at 2.4 ms/s, at most 100 µs per frame.
  Network, decode and capture jitter therefore move frames inside the playout
  delay, not the schedule. A phase discontinuity drops the window but keeps the
  applied offset. When every frame of at least a second maps later than its
  slot by more than the playout delay plus a period, the path latency stepped
  up and the mapping re-anchors at once.
- **Source rate.** The period is fitted from the endpoint span of recent RTP
  times over their frame-number span (Q16, 350 ms to 1 s of history, longer
  near the refresh ceiling), so lost or dropped frames do not look like a
  slower source. The negotiated rate bounds it. An interval more than 3.5×
  away from the period starts a provisional candidate, accepted after three
  samples spanning 200 ms and cancelled by an interval within 5:4 of the old
  period, so a single hitch does not resize every budget.
- **Rebase.** A frame-number reset, a backwards or zero RTP step, or a jump of
  more than a second (reconnect, host restart) starts a new timeline; learned
  rate, delay and preparation budgets survive. Three consecutive frames that
  map far into the future reseed the mapping.
- **Playout delay.** The interval-quality buffer starts at
  `min(max(6 ms, 0.95 period), max(display period, preparation lead))` and stays
  between 1 ms and the preset's share of a source frame, also bounded by queue
  capacity (`4 × period - preparation lead - smoothing allowance`). Every pair
  of adjacent presents is scored by how far the client moved their spacing
  from the intended source spacing; the excess over the tolerance, relative to
  the interval, is weighted by time over the preset's history. The delay grows
  (at most 250 µs per 250 ms, applied at most 125 µs per frame) only while the
  score is below target, the interval error is fresh, the late frame was late
  against its own deadline, and decode and preparation fitted their intended
  time over the last second: more buffering cannot fix a pipeline that cannot
  keep up. It releases at 250 µs/s after eight seconds of clean evidence.
  Frames that lost packets (PyroWave) present on schedule but never teach the
  buffer. Delay learned for a source rate is restored, downward only, when that
  rate returns.
- **Reduce judder.** The smoother tracks the source period and places each
  frame 85 % of the way toward the predicted slot, within a 6 ms positive
  allowance that the queue capacity reserves, never earlier than the frame can
  be ready. A learned reserve (p98 of the lateness the smoother itself caused,
  at most 3 ms) covers advanced slots without growing the playout buffer. It
  resets on rebases, rate changes, phase discontinuities, stalls and bursts.
- **Late frames.** A late frame is clamped to "now"; the next frame returns to
  its own slot, so lateness is never carried forward. After a late present,
  recovery spaces the following frames at the source period minus part of the
  display headroom (2 % of the period at first, up to the whole headroom as
  queued work ages), delaying a frame by at most `min(4 ms, period/2)`, instead
  of presenting them back to back.
- **Presentation protection.** A frame whose target is within one display
  period plus a guard (1/96 of the period, 100–250 µs) of the previous flip, or
  more than 20 ms after it (the panel may be repeating a frame below its VRR
  range), is latched when the presenter synchronizes natively: it flips at the
  next refresh and never tears, without a software floor that could not
  sustain a source at the refresh rate. Presenters that would queue (Vulkan
  FIFO) or may tear (Vulkan Immediate) enforce `previous flip + display
  period + guard` in software. A
  latched flip's spacing anchor is `max(call, previous anchor + display period)`.
- **Preparation lead.** Preparation starts when the frame is dequeued, inside
  the playout interval. The typical (p50) and p99 preparation times over the
  last 96 frames (the first frame of an epoch is excluded) set the target's
  render offset and the preparation lead (at least 3 ms).

## Pacing worker

The worker owns a bounded queue of 4 waiting frames plus the one being
prepared or presented; decoder surface pools reserve these frames. A full
queue evicts its oldest frame. A waiting frame older than
`max(2 × max(period, next RTP interval), playout delay + preparation lead + period)`
is replaced when a newer frame waits behind it; the only frame is always kept.
The preparation lead is part of the horizon because preparation starts after
a frame leaves the queue, so an on-time frame's age already includes it. A
prepared frame is always presented, even late: replacing it would spend a
second preparation on the next frame and make that one late too. Sustained
pressure therefore skips frames before any decode or render work, close to
the actual throughput deficit, instead of building latency, while a host
stall stays visible rather than being buffered.

For each frame the thread asks the controller for a decision, prepares the
frame, waits for the target (an interruptible wait to 1 ms before, a sleep to
150 µs before, then a bounded spin), re-reads the spacing floor, and presents.
Stopping wakes every wait; the pacer stops the worker before the renderer is
destroyed, and every queued frame is released exactly once. A presenter call
already in progress (a swapchain acquire or GPU wait inside libplacebo) cannot
be interrupted; stop joins it rather than abandoning a thread that uses
renderer state, and logs which call it is waiting for once the join exceeds
2 s, so a window system or driver that stops making progress is named in the
log instead of looking like a client hang.

## Presenters

**Vulkan (Linux).** The swapchain's present mode is chosen once per renderer
and stays synchronized, because VRR requires V-Sync and nothing the client can
query shows whether the output is actually scanning out adaptively: a
fixed-refresh panel, adaptive sync disabled in the compositor or display, and
a windowed surface all look the same to the swapchain. Mailbox is used where
offered (native protection; Gamescope's WSI layer offers it), else FIFO
(software floor, which also keeps FIFO from queueing presents). Both follow
the refresh adaptively when the compositor enables VRR for the window and
stay tear-free when it does not. Immediate, which tears unless the panel is
in adaptive mode, is used only with `PLVK_VRR_ALLOW_TEARING=1`, a diagnostic
opt-in for outputs the user knows are adaptive (for example X11 with
`VariableRefresh`), which trades tear safety for the lowest present latency.
Preparation renders the frame and overlays into a renderer-owned
image in the swapchain's format and colorimetry and waits for its GPU
completion (at most 50 ms), so the decoder surface or PyroWave planes are
released before the target wait and preparation time includes GPU work. At
the target the worker acquires a swapchain image, copies the prepared image
into it and presents: holding an acquired image across the wait would let the
next frame's GPU work delay the flip. The first frame, and the first after a
colorspace change, is rendered at present time because the swapchain's
colorimetry is not yet known. Resizes scale the prepared image for one frame.
A failed render is never presented as a healthy frame: a failed preparation
drops the frame (after the GPU finished any work that read the source), and
a failure at present time still submits the acquired image, cleared to the
background, but counts the frame as not presented. Three consecutive render
failures request the usual renderer reset, once per renderer, so a failure
that survives recreation cannot reset on every frame. GPU loss and swapchain
failures request the reset at once.

Submission is not proof of an adaptive refresh: whether the panel varies its
refresh depends on the compositor's VRR policy (for example KDE's "Adaptive
sync: Automatic" for fullscreen windows), the driver and the display.

**PyroWave.** A decode thread replaces the decoder's render thread. Frames
arrive on the network receive thread (direct submit), which only copies them
into recycled buffers and hands them over through a single-slot mailbox; a
frame the decode thread has not started is replaced by a newer one, so an
overloaded decoder skips frames before spending work on them. The decode
thread validates and parses each frame and submits its GPU decode into one of
a pool of plane sets, then queues it to the worker like a hardware decoder's
output (ready when the decode is submitted). Preparation renders the planes,
waiting for the GPU decode and render to complete. Packet parsing and
validation cost several milliseconds of CPU per frame at high bitrates (for
example 2880×1920 4:4:4 at 650 Mbps), so it overlaps the previous frame's
presentation instead of adding to it. The pool grows on demand to at most one
set more than the worker can own (6) and is kept for the session; each set
holds three 16-bit planes (about 33 MB at 2880×1920 4:4:4).

Windows (D3D11) and macOS (Metal) presenters are not implemented; those
platforms report "not supported on this platform" and keep fixed pacing.

## Host interaction

`clientVrrRequested=1` on `/launch` and `/resume` tells the host that the
client paces playback from RTP timestamps. Hosts that do not know it ignore it.
Pyroshine then captures each frame as the game presents it, rate-limited to the
stream frame rate, and stamps RTP with the content time; Vibepollo raises its
Windows virtual display refresh for precise capture timestamps. Client VRR
needs no host support: against a host that captures on a fixed clock, the
controller learns that cadence (including skipped frames) from RTP and still
removes delivery jitter.

## Diagnostics

The performance overlay adds:

- pacing state ("VRR pacing: active"), present mode, display refresh and
  stream frame rate, or the fallback reason. The overlay states that the
  panel's adaptive sync is not verified: an active pacing path is not proof
  that the display varies its refresh;
- source frame rate, playout delay and its maximum, queue depth;
- interval quality (the buffer's score), share of presented intervals more than
  2 ms from their intended spacing, mean submit error against the target;
- late presents (more than 1 ms after the intended target), latched share,
  drops, and arrival-to-present time.

The existing statistics count VRR drops as client frame-queue drops, queue
delay as arrival to submission without preparation, and rendering time as
preparation plus the present call. The log records a summary every 10 s and
one for the whole session at teardown, with the buffer's state and reason,
preparation lead, judder reserve, floor-delayed and catch-up counts, mean
preparation and present-call times, rebases, rate changes and phase resets. Submission times are measured on the client clock; they are not
display scanout times.

## Validation

Unit tests (`ctest --test-dir build/tests`):

- `vrr-timing`: rate policy and presets; jitter does not reach presentation;
  the buffer grows for jitter and releases; Lowest latency's bound; late-frame
  recovery without compressed presents under both protection kinds; host stall
  and latency step; source-rate changes; a stream at the refresh rate;
  preparation close to the source period (no prepared frame discarded, drops
  near the throughput deficit); Reduce judder; RTP wrap; reconnect rebase;
  lossy PyroWave frames.
- `vrr-worker`: threaded worker with a fake presenter: ordering and release of
  every frame, bounded queue under a stalled presenter, release on stop,
  presents on target with arrival jitter, and a stop during a blocked present
  that reports the stall, still joins and releases every frame once.
- `vrr-timing` also covers the present-mode policy: Mailbox, else FIFO, and
  Immediate only when tearing is explicitly allowed.
- `app/tests/streamingpreferences_test.pro`: defaults, per-profile persistence,
  presets and custom values.

Hardware checks, which unit tests cannot replace:

1. With VRR enabled on the display and in the compositor, stream to a host
   running a game at roughly 60, 100 and 116–120 FPS on a 120 Hz panel, and at
   several rates on 144/165/240 Hz panels. The overlay should show the source
   frame rate, high interval quality and no growing arrival-to-present time;
   the panel's refresh readout should follow the game.
2. Change the game's frame limiter during the stream, load the host briefly,
   and add network jitter: no burst after late frames, recovery within seconds.
3. Disconnect and reconnect, move the window between displays with different
   refresh rates (including from a 60 Hz display that rejected a 120 FPS
   request to a 120 Hz one), resize, and toggle HDR: no persistent stutter or
   stale timing.
4. Repeat on a fixed-refresh display and with adaptive sync disabled in the
   compositor: no tearing (FIFO or Mailbox), and the overlay still reports
   adaptive sync as not verified.
5. Repeat against a standard Moonlight host (Sunshine) and with PyroWave,
   H.264, HEVC and AV1.
