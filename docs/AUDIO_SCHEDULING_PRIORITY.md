# Audio scheduling priority

- The interactive Win.apk runtime requests `ABOVE_NORMAL_PRIORITY_CLASS`.
- Java helper and AOT translation modes return before that request and keep their existing scheduling policy.
- The WASAPI mixer thread joins the MMCSS `Pro Audio` task and requests `AVRT_PRIORITY_CRITICAL`.
- If MMCSS is unavailable, the mixer thread falls back to `THREAD_PRIORITY_HIGHEST`.
- The previous unconditional `THREAD_PRIORITY_TIME_CRITICAL` fallback was removed to avoid starving unrelated system work.
- Startup logs report process-priority, MMCSS and fallback results.
