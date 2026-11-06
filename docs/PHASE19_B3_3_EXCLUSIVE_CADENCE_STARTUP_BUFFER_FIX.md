# Phase 19-B3.3 Exclusive Cadence / Startup Buffer Fix

- Primes the full exclusive endpoint buffer with silence before IAudioClient::Start.
- Elevates the render thread with MMCSS Pro Audio when available and time-critical thread priority.
- Replaces synchronous per-period cadence logging with one 64-interval aggregate summary.
- Fixes the blockAlign varargs format mismatch.
- Preserves B3.2 exclusive software gain and volume persistence.
