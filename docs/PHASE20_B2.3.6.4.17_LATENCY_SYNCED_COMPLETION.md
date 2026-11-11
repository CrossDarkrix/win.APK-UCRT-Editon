# Phase 20-B2.3.6.4.17 Latency-Synced URB Completion

Built from Phase 20-B2.3.6.4.16. Two inputs:

1. `last-run-2.log` (the .16 build): the producer side is cleaner than
   ever (`queueDrops=0`, `queueDepthMax=4`, producer gaps >10 ms only
   60 times in 5.5 minutes, one 74.9 ms maximum) and only two
   FramePacer stalls >=10 ms occurred during playback -- yet a few
   skips remain. WASAPI-side evidence now lives in
   `winapk-audio-host.log` (created by .16 next to the executable);
   the bridge summary there is the next thing to inspect.
2. A visible sync defect: the spectrum analyser and the elapsed /
   remaining time run *ahead* of the audible sound by the pipeline
   delay. This phase fixes that properly, on the host side.

## Why the UI ran ahead

UCRT drives its progress position and spectrum from its USB pipeline:
audio is "played" from the app's point of view when it has been
submitted / its URBs complete. Audibly, though, the samples still have
to cross the PCM worker queue, the shared ring, the source ring
(~1 s preroll target), and the WASAPI endpoint buffer (250 ms) --
about 1.3 s in the 192 kHz -> 48 kHz configuration. So every
app-visible clock tied to the USB stream was ~1.3 s early.

## Change: delay completions by the measured output latency

A real USB DAC consumes isochronous data at its own pace and the host
controller completes URBs as the data leaves; the emulation now does
the same thing in the only place it can -- the REAP path
(`windows/src/bionic/stdio.cpp`, `kP20Reap`): a normally completing
URB becomes reapable at `ready + output_latency` instead of `ready`.
Production is unaffected (PCM is decoded and enqueued at SUBMIT, as
before, and completions still stream at the nominal rate, just shifted),
so no buffer shrinks and the anti-skip work of .15/.16 is untouched.
The app's submit->reap loop, its decoder ring drain, and therefore its
progress / spectrum position all shift by the same offset and land on
the audible output.

- The offset is **measured**, not a constant. The audio host publishes
  its current output latency (source-ring occupancy floored at the
  preroll target while playing, plus the endpoint buffer rescaled to
  the source rate) into the IPC header at ~10 Hz
  (`Header::output_latency_frames`, ABI 2 -> 3;
  `audio_host_main.cpp`, `audio_engine.cpp::stream_output_latency_frames`).
  In the runtime-fallback owner mode the local stream is queried
  directly. The value is clamped to <= 2.5 s.
- Cancelled URBs (`status == -2`, e.g. DISCARD on seek/pause) are
  never delayed, so seeks and stops stay immediate.
- Override / kill switch: `WINAPK_USB_LATENCY_SYNC_MS` -- `0`
  disables the sync, any other value pins a fixed delay in ms
  (max 5000). One startup line logs an override; the 30 s stall
  summary gains a companion line `Phase20-B2.3.6.4.17: latency sync
  offsetMs=...` so the active offset is visible in `last-run.log`.

## Caveats

- This syncs everything the app derives from its USB stream. If some
  widget used an independent wall clock instead, it would not move;
  the player position and spectrum in UCRT are stream-driven.
- At playback start the first completion now arrives after
  ~pool (48 ms) + reported latency (~1.3 s) instead of ~48 ms, which
  is exactly when the first samples become audible. If the app ever
  treats that initial wait as a stall, set
  `WINAPK_USB_LATENCY_SYNC_MS=0` to compare.

## Verification

- `audio_engine.cpp` passes the stub-header syntax check; the new
  `P20AudioHostClient::output_latency_frames` and the
  `p20_latency_offset()` helper were extracted and syntax-checked the
  same way. MSYS2 UCRT64 build + listening check on Windows as usual.
- ABI note: runtime and audio host must be rebuilt together (they are,
  via `./build.sh`); the header magic/ABI check rejects a stale
  audio-host binary instead of misreading the shared header.
