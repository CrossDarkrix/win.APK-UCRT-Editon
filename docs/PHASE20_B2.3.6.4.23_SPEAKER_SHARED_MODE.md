# Phase 20-B2.3.6.4.23 Speaker Route Uses Shared Mode

Built from Phase 20-B2.3.6.4.22.

## Evidence chain

1. `last-run-5.log`: the app never touches `android.media.audiofx`
   (zero hits) -- the .21 Visualizer hypothesis is refuted. The tap
   stays (the API was dead and is now real), it just was not this bug.
2. `last-run-6.log` (sent after the trace request; the trace env var
   did not reach the process, but the log is decisive anyway):
   **no USB submit traffic at all**, while the runtime process itself
   logs `exclusive endpoint buffer accepted rate=192000` three times,
   interleaved with `AudioTrack` / `AudioTrack$Builder` /
   `AudioTrackCompatKt` / `PlayerController$FileStream` /
   `SpectrumTap` class loads. The speaker route is AudioTrack into
   the runtime's NORMAL mixer, which -- since .14's low-spec ladder --
   opens the endpoint EXCLUSIVE with a 250 ms buffer.

## Root cause

In exclusive mode the render loop must fill the whole endpoint
buffer per event, so `Stream::mix` consumes the source rings in
~250 ms quanta. Everything an app can observe advances in those
steps: the playback-head position jumps 250 ms at a time, and a
writer thread blocked in `AudioTrack.write` is released in 250 ms
bursts. UCRT's spectrum tap is fed from that cadence on this route,
so the analyser moves at ~4 Hz -- the reported コマ送り. On the
USB-DAC route the app taps its own 4 ms URB stream and is smooth.
The bug predates the .15 work because the ladder arrived in .14.

## Fix

The normal (speaker) mixer no longer attempts the exclusive ladder
by default. It goes straight to WASAPI **shared mode** (the existing
fallback path: 40 ms client buffer, event-driven at the ~10 ms
device period), so consumption, positions and write releases advance
in ~10 ms quanta -- the cadence Android's AudioTrack HAL itself
uses. The USB Direct mixer is untouched (always exclusive, .20 cap
applies). The shared-mode log line now states the reason
(`speaker-default-phase23` vs `exclusive-failed`) so future logs are
unambiguous.

Escape hatch: `WINAPK_SPEAKER_EXCLUSIVE_MS` (25..250) restores the
exclusive ladder for the normal mixer, capped at that size, for
low-spec machines that genuinely need the 250 ms reservoir more
than they need a smooth analyser.

## Verification

`audio_engine.cpp` compiles to an object with the stub headers; the
exported-symbol set is unchanged (no new exports in this phase).
The cumulative patch applies cleanly to the pristine .14 tree and
reproduces this tree exactly. Real-hardware check still required:
speaker-route playback should now log `Shared Initialize ...
reason=speaker-default-phase23` and the analyser should track at
frame rate.
