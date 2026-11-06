# Phase 20-B2.3.2 Dedicated USB Direct WASAPI Sink and Unity Gain

- Separates normal Android audio and UCRT USB Direct PCM into two independent mixer/WASAPI instances.
- Normal audio stays on the Windows default endpoint.
- USB Direct requires the configured endpoint ID, opens that exact IMMDevice, and refuses default-speaker fallback.
- Verifies the opened endpoint ID equals the configured ID.
- Exclusive failure may fall back only to shared mode on the same IMMDevice.
- USB Direct software gain is fixed to 1.0. Shared-session volume is set to 1.0 and mute is cleared without changing endpoint master volume.
- Preserves B2.3.1 PCM and WASAPI diagnostics.
