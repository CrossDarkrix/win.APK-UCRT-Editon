# Phase 20-B2.3.6.4.6 Audio Host Warmup

- Starts winapk-audio-host.exe asynchronously while the runtime/Bionic layer initializes, before the UCRT UI begins playback.
- The host remains in Stopped state and does not open WASAPI until rate/alt configuration arrives.
- Adds a mutex around process startup so warmup and first playback cannot create duplicate host processes.
- Playback still falls back to the in-process route if warmup or host startup fails.
