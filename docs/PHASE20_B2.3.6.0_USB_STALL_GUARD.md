# Phase 20-B2.3.6.0 USB Stall Guard
- USB Direct source preroll: 30 ms to 120 ms. USB bounded startup fallback: 150 ms to 500 ms. Normal streams unchanged.
- Counts source underruns and confirms unfilled mixer frames are zero because Mixer::mix clears every destination buffer first.
- Adds 30 second producer-gap, queue and async-write telemetry.
- Keeps CD Event Gate and DISCARD completion preservation.
