# Phase 20-B2.3.6.4.5 Exclusive Owner Fix

- Prevents runtime and audio host from opening the same USB Direct WASAPI exclusive endpoint concurrently.
- p20_start now creates the in-process stream only when audio-host startup/configuration fails.
- If the host owns the route, any old runtime stream is stopped and released before PCM transfer.
- Logs the selected exclusive owner explicitly.
