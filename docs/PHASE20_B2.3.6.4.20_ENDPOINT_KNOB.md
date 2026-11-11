# Phase 20-B2.3.6.4.20 Endpoint Buffer Knob

Built from Phase 20-B2.3.6.4.19.

## Why not "just prepend silence to the source"?

A natural idea -- and it cannot work, for a structural reason: the UI
lead is not a property of the *data*, it is the *transit time* of the
pipeline (~1.3 s). Prepending S seconds of silence shifts the app's
timeline and the audible timeline by the same S, so the gap between
"what the UI shows" and "what is heard" stays exactly the pipeline
latency. Numbers, with L = 1.3 s:

- Now: when the sound of track position 1.0 s is heard, the UI shows
  2.3 s. Lead = 1.3 s.
- With 1.3 s of silence prepended: when the sound of track position
  1.0 s (file position 2.3 s) is heard, the UI shows file position
  3.6 s = track position 2.3 s. Lead = 1.3 s. Unchanged.

The lead only shrinks if the transit time itself shrinks. .19 added
the preroll knob for that; this phase adds the second fixed component.

## Change

`WINAPK_USB_ENDPOINT_MS` (25..250, default 250 = unchanged) caps the
exclusive-mode buffer ladder in `Mixer::run_device`
(`windows/host/audio_engine.cpp`). The bare device period is always
attempted, so the cap cannot make negotiation fail by itself. The
accepted-buffer log line gains `capMs=`.

## Combined recipe for minimum lead

- `WINAPK_USB_PREROLL_MS=300` + `WINAPK_USB_ENDPOINT_MS=80`:
  lead ≈ 300 + 80 + ~50 (queues/pool transit) ≈ **0.43 s**.
- Aggressive: `WINAPK_USB_PREROLL_MS=150` + `WINAPK_USB_ENDPOINT_MS=40`:
  lead ≈ **0.24 s**, with correspondingly thin skip margins -- the
  session logs show producer gaps up to ~75 ms and UI stalls up to
  ~100 ms on this machine, so treat this as an experiment, not a
  default.

Both knobs default to the historical behaviour, and both trade the
same currency (stall headroom) that phases .9-.14 spent on the
opposite direction; .15 drift compensation and .16 re-preroll stay
active at every setting.

## Verification

`audio_engine.cpp` passes the stub-header syntax check; the patch
applies cleanly to the pristine .14 tree and reproduces this tree.
