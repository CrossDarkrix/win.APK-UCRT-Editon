# Phase 20-B2.3.6.4.16 Log-Guided Stall Recovery

Built from Phase 20-B2.3.6.4.15. Guided by a real `last-run.log`
(2026-11-12, UCRT v0.8.0, FLAC playback) and the report that the symptom
is a *skip, like a scratched CD*, occurring occasionally and also while
the app is in the background under load.

## What the log establishes

- Session: 132,500 URBs x 4 ms = ~8.8 minutes, virtual DAC at
  **192 kHz** (alt=6), WASAPI endpoint in **exclusive 48 kHz**,
  24-in-32, 250 ms buffer.
- Producer side is healthy: `queueDrops=0`, `queueDepthMax=8`,
  `asyncWriteMaxUs=181`, producer gaps >10 ms on ~1% of URBs, >25 ms
  only 6 times, max 56.8 ms. Nothing is dropped before the shared ring.
- UI/main-thread stalls of 28-102 ms (`FramePacer worstLateMs`) cluster
  in the second half, while `foreground=0` -- load/background related,
  matching the report.
- **The audio host process is invisible in `last-run.log`.** It is
  created with `bInheritHandles=FALSE` and no standard handles, so its
  stderr -- the WASAPI underrun line, the 30 s bridge summary
  (`readable / underruns / overruns / bridgeShortWrites`),
  `CONFIGURE_COMMIT`, the host-bridge QoS line -- appears nowhere.
  8.8 minutes of playback produced zero audio-host lines. Every past
  diagnosis was therefore done blind on the output side.

## Changes

1. **Audio-host log file** (`windows/src/bionic/stdio.cpp`,
   `P20AudioHostClient::start`). The child is now created with
   inheritable standard handles redirecting stdout/stderr (append) to
   `winapk-audio-host.log` next to the executable (stdin from `NUL`).
   Falls back to the previous behaviour if the file cannot be opened.
   The next `last-run.log` also gains one line naming the log path.
2. **Re-preroll after underrun** (`windows/host/audio_engine.cpp`,
   `Stream::mix`). Previously `preroll_ready_` stayed true forever:
   after the first underrun spent the lead, the stream kept playing at
   ~zero lead, so every routine producer jitter (10-50 ms gaps above)
   became another audible micro-skip -- the scratched-CD pattern of
   repeated skipping. After an underrun the gate now re-arms with a
   shorter 250 ms target (initial gate stays 1000 ms), and the gate
   timer restarts at the underrun (it is measured from `play_nanos_`,
   which is reset), so the existing bounded fallback cannot reopen the
   gate instantly on an empty ring. Combined with .15's fade-out /
   fade-in, a stall becomes one brief faded pause while the producer's
   burst catch-up refills the ring, instead of a cluster of skips.
   The underrun log line is also no longer verbose-gated: it is printed
   (throttled to one per 5 s) as
   `Phase20-B2.3.6.4.16 underrun ... action=fade+repreroll`.
3. **Anti-aliased downsampling for step >= 2** (`Stream::mix`). At
   192 kHz -> 48 kHz the resampling step is exactly 4 and the linear
   interpolator degenerates to taking every 4th sample (fraction is
   always 0): decimation with no anti-alias filter, folding all content
   above 24 kHz into the audible band. For steps in [2, 64] the mixer
   now averages the input span one output frame covers (boxcar; nulls
   at multiples of the output rate, the correct cheap filter for
   integer ratios). Steps below 2 keep the existing linear path.
   Note `Voice::mix` (MediaPlayer/SoundPool clips) still uses the
   linear path; it is not on the USB Direct route.

## What .15 + .16 together cover, and what to check next

- .15 (drift compensation) handles the slow, monotonic ring walk from
  the ppm difference between the app's pacing clock and the DAC clock.
- .16 (re-preroll) handles the bursty case the log actually shows:
  load/background stalls spending the lead, then jitter at zero lead.
- Next evidence to collect: reproduce with .16 and inspect
  `winapk-audio-host.log` — its bridge summary separates the remaining
  possibilities cleanly (`underruns` there counts empty polls of the
  shared ring, `bridgeShortWrites` counts a full source ring, and the
  WASAPI underrun line is now in `last-run.log` proper).

## Verification

- `audio_engine.cpp` passes the same stub-header syntax check as .15
  (original and patched both compile; MSYS2 UCRT64 build on Windows is
  still required before release).
- The `P20AudioHostClient::start` change is small and self-contained;
  it was reviewed against the Win32 contract for
  `STARTF_USESTDHANDLES` (inheritable handles, valid stdin).
