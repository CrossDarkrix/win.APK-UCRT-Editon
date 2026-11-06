# Phase 20-B2.3.1 Continuous PCM and WASAPI Diagnostic

- Measures every submitted URB, while logging only the first URB, first non-zero URB, and every 2500th URB.
- Tracks silent URBs, non-zero URBs, and non-zero stereo frames.
- Logs endpoint enumerator creation, configured endpoint resolution, IAudioClient activation, mix format, event handle, buffer size, render service, volume service, and Start HRESULTs.
- Does not change routing, conversion, pacing, cancellation, or fallback behavior.
