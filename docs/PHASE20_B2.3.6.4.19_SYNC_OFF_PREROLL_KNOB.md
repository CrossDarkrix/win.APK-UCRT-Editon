# Phase 20-B2.3.6.4.19 Latency Sync Off by Default + Preroll Knob

Built from Phase 20-B2.3.6.4.18. Regression report with `last-run-4.log`:
**playback now plays ~1 second, stops, and repeats.**

## What the experiments established

- .17 (full 1.3 s completion delay from the first URB): the stream
  thread never comes back -- 15 submits, 3 reaps, silence. A long
  completion *gap* at startup is fatal.
- .18 (delay ramped in at 0.2x realtime, capped at 800 ms): playback
  starts, but dies about once a second in a stop/restart cycle.
  During the ramp, completions necessarily arrive at 0.8x realtime,
  so UCRT's stream supervision evidently also validates the
  consumption *rate*: a DAC that consumes 20% slow looks broken and
  gets reset on the supervision's ~1 s cadence.

Conclusion: with this app, **any** distortion of URB completion timing
(gap or rate) is unusable. The app-visible clock cannot be moved by
lying about consumption; the only sound way to shrink the UI lead is
to shrink the real buffering that causes it.

## Changes

1. **Latency sync is opt-in now** (`windows/src/bionic/stdio.cpp`).
   Without `WINAPK_USB_LATENCY_SYNC_MS`, the completion offset is 0
   and REAP timing is exactly the .16 behaviour -- the build that
   played cleanly for full sessions. Setting the variable to a cap in
   ms re-enables the .18 ramped sync for experimentation only.
2. **Preroll knob** (`windows/host/audio_engine.cpp`). The USB Direct
   preroll target -- ~1000 ms of the ~1300 ms total UI lead -- is now
   read from `WINAPK_USB_PREROLL_MS` (50..3000, default 1000 =
   unchanged). It applies consistently to the preroll gate, the .15
   drift controller's target, the re-preroll cap (min(250, value)),
   and latency reporting. Setting e.g. `WINAPK_USB_PREROLL_MS=300`
   cuts the UI lead to roughly 300 + 250 (endpoint) + ~50 (queues)
   = ~0.6 s with completion timing untouched. The trade-off is skip
   margin: observed producer gaps in the session logs are <= ~75 ms,
   so 300 ms still holds ~4x the worst observed gap, and the .15
   drift compensation + .16 re-preroll remain active. Go lower
   (150 ms -> ~0.45 s lead) only if the machine is idle while
   listening.

Everything else from .15-.18 is unchanged.

## Verification

- `audio_engine.cpp` passes the stub-header syntax check.
- The latency helper was re-run in its harness: unset env -> offset 0
  (target and ramped); `WINAPK_USB_LATENCY_SYNC_MS=1300` -> target
  1300 ms with the ramp still applied to live completions.
