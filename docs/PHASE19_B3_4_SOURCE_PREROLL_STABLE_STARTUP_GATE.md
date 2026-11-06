# Phase 19-B3.4 Source Preroll / Stable Startup Gate

- Arms a source-side startup gate whenever an AudioTrack stream enters playback.
- Holds the stream silent until 30 ms of complete source PCM frames are queued.
- Uses a bounded 150 ms fallback when at least one frame is queued, preventing short streams from waiting forever.
- Uses frame count only; quiet audio is never discarded based on amplitude.
- Resets the gate on stop and flush, while preserving drain behavior.
- Preserves B3.3 startup prefill, MMCSS, cadence aggregation, and B3.2 software gain.
