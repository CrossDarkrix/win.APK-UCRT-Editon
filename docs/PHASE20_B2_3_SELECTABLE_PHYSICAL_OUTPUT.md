# Phase 20-B2.3 Selectable Physical Output and Audio Verification

- Adds a Settings > Audio output dropdown populated from active WASAPI render endpoints.
- Persists the opaque endpoint ID and friendly name in HKCU\Software\Win.apk.
- Adds a same-device shared-mode fallback toggle.
- The runtime opens the saved endpoint ID; a configured missing endpoint is not silently replaced.
- Adds a first-URB PCM peak/RMS/non-zero probe.
- Limits ABI/raw logging to the first URB and periodic Submit statistics to 10-second intervals.
- Keeps the Phase 20-B2.2 ABI, failure-stop, cancellation and pacing behavior.
