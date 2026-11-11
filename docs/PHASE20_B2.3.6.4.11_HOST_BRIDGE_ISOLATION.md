# Phase 20-B2.3.6.4.11 Host Bridge Isolation

## Root cause targeted

Phase 20-B2.3.6.4.10 protects the runtime-side PCM producer. The isolated audio-host main thread that copies PCM from shared memory into the two-second Source ring was still an ordinary thread. A scheduling delay there can starve the WASAPI mixer even while the shared ring contains data.

## Implementation

- The separate winapk-audio-host bridge process opts out of execution-speed process throttling.
- Its shared-ring consumer thread opts out of execution-speed thread throttling.
- The bridge thread registers with MMCSS `Pro Audio` at AVRT High priority.
- MMCSS failure falls back only that bridge thread to THREAD_PRIORITY_HIGHEST.
- Dynamic priority boosts are disabled on the bridge thread.
- Refill quantum grows from 8,192 to 32,768 stereo frames, allowing each wake to move more buffered PCM into the Source ring.
- Event wait fallback is reduced from 5 ms to 2 ms.
- Summary diagnostics now report bridge batches, transferred frames, and short writes.

The existing WASAPI mixer remains MMCSS Critical. Runtime USB submission remains Critical and the runtime producer remains High, preserving the scheduling order Critical -> High -> Critical across the isolated pipeline.
