# CPU Priority Plus 2 for audio stability

- Foreground runtime process: ABOVE_NORMAL_PRIORITY_CLASS -> HIGH_PRIORITY_CLASS.
- Audio render thread fallback when MMCSS Pro Audio is unavailable: THREAD_PRIORITY_HIGHEST -> THREAD_PRIORITY_TIME_CRITICAL.
- MMCSS Pro Audio + AVRT_PRIORITY_CRITICAL remains the preferred path.
- REALTIME_PRIORITY_CLASS is deliberately not used because it can preempt critical operating-system work.
- Background AOT translation remains BELOW_NORMAL_PRIORITY_CLASS.
- Punch default 3 and TLS 1.2 non-ECDH fallback are preserved.
