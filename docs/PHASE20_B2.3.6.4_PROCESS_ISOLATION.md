# Phase 20-B2.3.6.4 Process Isolation + Session Ownership

- Adds winapk-audio-host.exe as a HIGH_PRIORITY_CLASS child process.
- Uses Local named file mapping and named events for a two-second SPSC float-stereo PCM ring.
- Child owns USB Direct Mixer, DSP, endpoint selection, volume state, WASAPI client and generation/rate reconfiguration.
- Runtime keeps guest usbfs ABI and URB pointer completion because guest JNA pointers cannot be dereferenced safely by another process.
- Host monitors the parent process and exits when the runtime exits.
- Parent retains the previous in-process stream as an automatic fallback; isolated writes suppress duplicate local playback.
- Every PCM block carries the active generation; stale generations are dropped.
- 30-second child diagnostics expose ring depth, underruns, overruns, consumer gaps and reconfiguration count.
