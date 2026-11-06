# Phase 20-B2.3.3 USB Direct Digital Gain and Soft Limiter

- Adds 0,+3,+6,+9,+12,+15,+18 dB USB Direct-only gain choices.
- Default is 0 dB; +12 dB is labeled as the recommended test value.
- Persists UsbDirectGainStep under HKCU\Software\Win.apk.
- Applies gain only in the dedicated USB Direct mixer, in exclusive and shared modes.
- Uses an approximately 100 ms gain ramp.
- Applies a smooth rational soft limiter with a 0.98 ceiling and emits 10-second peak/limiter diagnostics.
- Does not change Windows endpoint/master volume or normal Android audio.
