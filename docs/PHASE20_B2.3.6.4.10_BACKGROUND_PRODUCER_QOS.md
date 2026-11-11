# Phase 20-B2.3.6.4.10 Background Producer QoS

## Evidence

The supplied last-run log shows audio-host startup succeeds, queueDrops remains zero, and producerGapMaxUs reaches about 130124 us. The remaining symptom is associated with the runtime window moving to the background, so this phase protects the runtime-side PCM producer rather than increasing buffers again.

## Implementation

- The asynchronous PCM producer worker opts the runtime process out of execution-speed power throttling.
- The PCM producer worker opts itself out of execution-speed power throttling.
- The worker registers with MMCSS task `Pro Audio` and requests AVRT High priority.
- If MMCSS is unavailable, only that worker falls back to THREAD_PRIORITY_HIGHEST.
- Dynamic priority boosts are disabled on the producer worker to avoid foreground/background boost variance.
- The 30-second USB stall summary now records whether the runtime owns the foreground window.
- The existing USB submit thread remains MMCSS Critical; the producer worker is High to preserve ordering and avoid making every stage Critical.

Deep buffers, isolated ownership, warmup, generation checks, staging, and runtime fallback remain unchanged.
