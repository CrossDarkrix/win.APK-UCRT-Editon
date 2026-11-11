# UCRT Self Performance Guard

- Applies ProcessPowerThrottling off inside winapk-runtime before helper/AOT/JVM/APK paths.
- Applies HIGH_PRIORITY_CLASS inside winapk-runtime itself.
- Reapplies both policies every 2 seconds to recover from later changes.
- Logs initial policy results and repairs.
- Preserves launcher-side policy and the audio thread MMCSS/power-throttling policy.
