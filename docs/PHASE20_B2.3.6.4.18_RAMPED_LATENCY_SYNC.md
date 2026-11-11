# Phase 20-B2.3.6.4.18 Ramped Latency Sync (fixes .17 startup failure)

Built from Phase 20-B2.3.6.4.17. Regression report with `last-run-3.log`:
**music no longer plays at all.**

## Evidence

The .17 session starts normally -- audio host ready, configure at
192 kHz, exclusive endpoint accepted -- then dies: **15 submits,
3 reaps, and nothing ever again.** No teardown ioctls, no errors; the
app's stream thread simply never comes back, and a later
`createVirtualDac` (the user retrying) produces no new stream.

## Root cause

.17 applied the full measured completion delay (~1.3 s) from the very
first URB of the stream. UCRT's USB supervision does not tolerate a
~1.3 s completion gap at startup: after the first few completions
(which raced through while the host's first latency publication was
still 0), the next completion suddenly lands ~1.3 s late and the
stream thread gives up / never returns. The delay itself is sound --
applying it instantaneously at full size is not.

## Fix (windows/src/bionic/stdio.cpp, `p20_latency_offset`)

1. **Ramp-in per pool epoch.** The allowed offset starts at 48 ms
   (exactly the latency the 12-deep URB pool already implies, so the
   first completions behave like an unpatched build) and grows at
   0.2x realtime. Completions therefore keep arriving every ~5 ms --
   there is never a gap for the app's supervision to trip on -- while
   the app-visible clock slides back onto the audible output,
   reaching the full capped offset after a few seconds. The ramp
   restarts whenever the URB pool is rebuilt (new stream, seek),
   which is exactly when the app re-runs its startup expectations.
2. **Cap.** The offset is capped at **800 ms** by default (the
   measured latency is ~1.3 s; 800 ms is safely below the gap size
   that killed .17, at the price of the UI still leading the sound
   by up to ~0.5 s). `WINAPK_USB_LATENCY_SYNC_MS` now sets the cap
   in ms: `0` disables the sync entirely, a larger value (e.g.
   `1300`) permits full sync -- use it to probe how much completion
   latency this app build actually tolerates; if playback survives
   `1300`, the default cap can be raised.
3. The 30 s summary line now reports the *target*:
   `Phase20-B2.3.6.4.18: latency sync targetMs=800`.

Everything else from .15-.17 (drift compensation, re-preroll, boxcar
downsampling, audio-host logging, measured latency publication,
cancelled URBs never delayed) is unchanged.

## Verification

- The rewritten helper was extracted and run in a harness: measured
  249,600 frames at 192 kHz -> target 800 ms (cap applied); offset at
  pool-epoch start = 48 ms, growing at 0.2x realtime; cap override
  `WINAPK_USB_LATENCY_SYNC_MS=0` yields 0.
- If .18 still fails to start on some other app build, set
  `WINAPK_USB_LATENCY_SYNC_MS=0`: playback then behaves exactly like
  .16 (sync off) while the anti-skip fixes stay in place.
