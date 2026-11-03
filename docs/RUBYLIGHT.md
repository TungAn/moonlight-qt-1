# Rubylight control messages

Rubylight hosts list the control messages they take in their RTSP `DESCRIBE`
answer (`a=x-rl-control:0x5530,...`, Rubylight's
[control messages](https://github.com/RamazanKara/Rubylight/blob/main/docs/control-messages.md)).
Pyrolight sends a message only when its id is listed, so other hosts see
nothing new. The wire formats follow Nightjar's `rubylight-protocol` crate
(`app/streaming/rubylightprotocol.h`, tested by `tests/rubylight_protocol.cpp`
with Nightjar's own phase lock cases). The log names the count:
`Host takes N Rubylight control messages`.

`moonlight-common-c` parses the lines, offers `LiHostAcceptsRubylightMessage()`
and `LiSendRubylightMessage()`, and hands host messages (`0x5536`, `0x5537`,
`0x5538`, `0x553D`) to the `rubylightMessage` connection callback on the
control receive thread. `app/streaming/rubylightlink.cpp` uses them.

## Phase lock (0x5530)

**Phase lock with Rubylight hosts** (Settings, under network calibration; off
by default) reports every half second how long before the display's latch the
frames shown were ready: the 10th percentile and the 10th to 90th percentile
spread of the slack, and the display's refresh period. The host then captures
at the client's exact refresh period, at a phase that keeps the early edge of
that slack 1.5 ms before the latch. Without it, a host and a display on separate
clocks drift: on the Retroid Pocket 6, a 59.750 fps host against a 119.50 Hz
panel moves a whole frame against the latch about every three minutes. The
host locks only when a frame lasts a whole number of refreshes (within 2%), and
lets go three seconds after the last report.

The latch clock is Linux only: a thread waits for DRM vblank events on
`/dev/dri/card0` to `card3` (or `PYROLIGHT_DRM_CARD`), CRTC 0 (or
`PYROLIGHT_DRM_CRTC`), which any client may do while a compositor holds DRM
master. The period is the median of the last 64 vblank intervals. A frame is
ready when its present call returns; the latch is the vblank minus
`PYROLIGHT_PHASE_LATCH_LEAD_US` (default 4000), how long before the vblank the
compositor takes a frame plus the GPU work still left after the present call.
Raise it when frames still miss refreshes with phase lock on, lower it to gain
latency. Frames are sampled from the PyroWave render thread and the FFmpeg
pacer; the VRR worker does not report, since a VRR display has no fixed latch.
With no vblank clock the log says so and nothing is sent.

## Diagnostics

With a Rubylight host the performance overlay adds:

- **Rubylight host present to sent** (`0x5536` host timing): the median time
  from the game's present to the last packet leaving the host, and its queue
  (capture and conversion), encode and send parts.
- **Rubylight host FPS** (`0x553D` pipeline rates, every second): the rate the
  game presented, and the rates captured, encoded and sent, and the repeats.
  This tells a host that sends fewer frames from a network that loses them.
- **Host present to client present** (`0x5537` clock sync, four probes a
  second): the frame's RTP timestamp against the host clock, mapped with the
  shortest-round-trip exchange of the last 32, median and 95th percentile.
- **Phase lock**: the last reported lead and spread.
- **Host bitrate lowered for the network** (`0x5538` link notice), when the
  host's link-rate controller has cut the bitrate; the log records each notice.

Host timing, pipeline rates and link notices are requested once per stream,
reliably; clock sync probes and phase reports are unsequenced, since a lost one
is replaced by the next. Nothing is sent from the render thread.
