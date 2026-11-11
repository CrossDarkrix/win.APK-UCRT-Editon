# Phase 20-B2.3.5.8 A-D

## A: hot-path logging suppression
- Routine WASAPI logging is disabled unless `WINAPK_VERBOSE_AUDIO=1`.
- Routine Phase20 informational logging is disabled unless `WINAPK_PHASE20_VERBOSE=1`.
- Routine WASAPI logging no longer calls `fflush(stderr)`. Rare WASAPI errors remain immediate and flushed.
- A compact session summary is emitted on a Phase20 stop/reset.

## B: measurements
- Measures total `g_p20_lock` hold duration with RAII, including all return paths.
- Measures asynchronous `stream_write` duration, average, maximum, completed jobs, stale jobs and queue drops.

## C: WASAPI decoupling
- SUBMIT no longer performs blocking `stream_write` while holding `g_p20_lock`.
- PCM is moved into a bounded 256-job producer queue serviced by one lazy worker.
- Reset/generation changes invalidate stale queued audio.
- Queue overflow drops the oldest queued PCM block, records the event, and keeps USBFS/WASAPI scheduling responsive.

## D: thread activation
- Normal and USB-direct Mixer render threads are not created by endpoint/configuration access.
- Each Mixer starts its render thread only when its first real audio Source is added.
- The asynchronous PCM worker is likewise created only on the first non-empty USB PCM block.

## Preserved
- DISCARD cancelled-completion retention and REAP behavior.
- current/retired URB pools, generations and rate switching.
- PCM silence fill and 44.1-kHz-family fixed-point packet accounting.
- Event-driven WASAPI render loop and MMCSS setup.
