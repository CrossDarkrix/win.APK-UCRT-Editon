# Phase 20-B2.3.6.4.14 Low-Spec Adaptive Preroll

Built from Phase 20-B2.3.6.4.13. This revision adds scheduling margin for low-spec PCs without changing PCM negotiation, ownership, or the shared-mode fallback.

- Exclusive endpoint buffer ladder: 250 ms, 200 ms, 160 ms, 120 ms, 80 ms, 40 ms, two device periods, one device period. Duplicate periods are skipped and the first accepted size is retained.
- USB Direct Source ring: four seconds instead of two seconds.
- USB Direct startup preroll: 1000 ms instead of 500 ms.
- USB Direct bounded startup fallback: 3000 ms instead of 1500 ms. If the producer cannot fill the one-second target, playback still starts with whatever non-empty audio is available at the bound.
- Non-USB paths remain at 30 ms preroll and 150 ms bounded fallback.
- The four-second IPC shared ring, host-bridge QoS, WASAPI startup prefill, format negotiation, and shared-mode fallback remain unchanged.

The extra memory is bounded. At 192 kHz stereo float32, the four-second Source ring is approximately 6 MiB; the existing four-second IPC ring is approximately another 6 MiB.
