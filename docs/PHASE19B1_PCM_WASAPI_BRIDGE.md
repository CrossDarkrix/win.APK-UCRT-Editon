# Phase 19-B1 PCM/WASAPI Bridge

This phase makes the existing host PCM mixer prefer WASAPI exclusive event-driven rendering and fall back to shared event-driven rendering. Android AudioTrack PCM is normalized to interleaved stereo float by the existing Stream mixer, sample-rate converted to the selected endpoint rate, clamped, and packed as the endpoint's 32-bit float or 16-bit integer PCM. UCRT should use its normal PCM output path; direct Linux usbfs/DoP/native-DSD transport is not emulated. DSD content must be decoded to PCM by UCRT before AudioTrack output.

The exclusive path validates the current endpoint format with IAudioClient::IsFormatSupported, uses the endpoint default device period, sets AUDCLNT_STREAMFLAGS_EVENTCALLBACK, and initializes buffer duration equal to periodicity. If any exclusive step fails, a fresh IAudioClient is activated for shared mode.

Diagnostics are emitted through OutputDebugStringA:
- [Phase19-B1] WASAPI exclusive event-driven PCM active
- [Phase19-B1] WASAPI shared event-driven PCM fallback active
