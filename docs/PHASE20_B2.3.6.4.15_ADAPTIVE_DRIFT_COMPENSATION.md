# Phase 20-B2.3.6.4.15 Adaptive Drift Compensation + Clickless Concealment

Built from Phase 20-B2.3.6.4.14. This revision attacks the remaining
"occasional noise / dropout during long, quiet listening" symptom at its
root instead of adding more buffer.

## Evidence / root cause

The UCRT playback chain contains two independent clocks and, until now,
no feedback between them:

- Producer: the app submits isochronous URBs; `p20_submit`
  (`windows/src/bionic/stdio.cpp`) paces them from `steady_clock` at the
  *nominal* sample rate.
- Consumer: the WASAPI endpoint's hardware clock, also nominally the
  same rate.

Real crystals differ by tens to hundreds of ppm. `Stream::mix`
(`windows/host/audio_engine.cpp`) consumed the source ring with a fixed
step (`rate_ * speed_ / out_rate`), so the ring level walked
monotonically for the whole session:

- Producer slower: the 1 s preroll lead drains at the ppm error rate
  (at 150 ppm, ~110 minutes; at 1000 ppm, ~17 minutes). When it reaches
  zero, the WASAPI buffer is zero-filled mid-stream and playback later
  resumes at full amplitude: a dropout bounded by two waveform steps,
  heard as a click / noise burst. This matches the reported pattern:
  clean playback, then an occasional glitch while listening at leisure.
- Producer faster: every ring in the chain fills in turn until a whole
  PCM job is dropped (`P20PcmQueue` drop-oldest, or a failed shared-ring
  write in `p20_pcm_worker`), which is the same discontinuity in the
  other direction.

Phases .9-.14 raised buffers and preroll (now 4 s shared ring + 4 s
source ring + 1 s preroll + up to 250 ms endpoint buffer). That only
postpones the walk; at 150 ppm the accumulated headroom is hours, not
infinity, and the stall-recovery / zero-fill behaviour at the end of
the walk was unchanged.

## Changes (windows/host/audio_engine.cpp, `Stream` only)

1. **Adaptive drift compensation (USB Direct streams only).** While
   playing, `mix()` now closes the loop on ring level: a PI controller
   driven by the deviation of queued frames from the 1 s preroll target
   nudges the resampling step (P: 1000 ppm per unit deviation,
   I: 50 ppm per unit deviation per second, total authority
   +/-2500 ppm, smoothed with a 2 s time constant). The integral term
   learns the actual crystal error, so the ring settles at its target
   for any constant ppm offset. This is the software equivalent of a
   real USB DAC's feedback endpoint. Non-USB streams, PCM negotiation,
   ownership, preroll targets, and the shared-mode fallback are
   unchanged. The correction resets on each new playback.
2. **Clickless concealment (all streams).** When a `mix()` call will
   exhaust its input (underrun or end of drain), the rendered tail is
   faded to zero over ~5 ms; the next call with data (and playback
   start at the preroll gate) fades back in over ~5 ms. The fade-out is
   latched per call from the frames actually renderable, so a producer
   that tops up just in time causes no gain modulation. Residual
   underruns from genuine multi-second stalls now sound like a brief
   dip instead of a broadband click.
3. **Diagnostics.** With `WINAPK_VERBOSE_AUDIO=1`, the existing log
   gains at most one line per 30 s while compensation is active:
   `Phase20-B2.3.6.4.15 drift compensation ppm=... desiredPpm=...
   queuedFrames=... targetFrames=...`. A steady non-zero ppm value
   confirms clock drift was present and is being absorbed; queuedFrames
   should stay near targetFrames over long sessions.

## Verification

- The patched translation unit passes a syntax check; an MSYS2 UCRT64
  build (`./build.sh`) on Windows is still required before release, as
  with every phase.
- The controller math was simulated offline (replica of the patched
  formulas; producer offset +/-50 / +/-150 / +/-1000 ppm, WASAPI
  callbacks 10 ms and 250 ms, 2 h sessions): the unpatched consumer
  underruns (producer-slow) or overflows/drops (producer-fast) in the
  large-offset cases, while the patched consumer finishes every case
  with zero underruns and zero overflows, correction converging to the
  producer's own ppm offset.

## Remaining suspects if a glitch is still heard

Check `last-run.log` at the glitch timestamp before assuming this
mechanism: `asyncQueueDrops > 0` in the Phase20-B2.3.6.0 stall summary
means the single PCM worker thread stalled for ~a whole queue of URBs
(scheduling/DPC on low-spec hardware); `overruns` in the audio-host
summary means the shared ring filled (should no longer happen once
drift is absorbed); an underrun log line *without* an active drift
compensation line points at the normal (non-USB-Direct) path.
