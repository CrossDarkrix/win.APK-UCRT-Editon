# Phase 20-B2.3.6.4.9 Deep Buffer and Preroll

## Evidence

The supplied last-run log reports no asynchronous queue drops and write times below 0.3 ms, so raw memory-copy throughput is not exhausted. It does report producer gaps up to about 108 ms while the previous USB Direct preroll was 120 ms, leaving little scheduling margin.

## Changes

- Shared SPSC PCM ring ABI 2: 768,000 stereo frames, four seconds at 192 kHz.
- Audio-host source ring: two seconds at every selected sample rate.
- USB Direct preroll: 500 ms instead of 120 ms.
- Bounded startup fallback: 1.5 seconds instead of 0.5 seconds.
- No changes to PCM format, rate, generation, exclusive ownership, or bit-perfect data conversion.

The larger buffers trade startup latency and roughly 6 MiB shared PCM storage plus source-ring storage for resilience against scheduler and disk/memory activity spikes.
