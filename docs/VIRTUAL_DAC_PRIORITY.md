# Virtual DAC Priority

- Applies ThreadPowerThrottling off only to threads entering the Phase 20 virtual USB DAC ioctl path.
- Registers each virtual-DAC usbfs thread with MMCSS `Pro Audio`.
- Applies MMCSS relative `Critical` priority.
- Uses `THREAD_PRIORITY_TIME_CRITICAL` only when MMCSS registration is unavailable.
- Disables dynamic priority boosting/deboosting on that dedicated thread.
- Logs pending queue depth and completion lateness over 2 ms.
- Preserves the proven URB ABI, pacing, generation reset, pool ownership, and DISCARD completion retention behavior.
- Does not use REALTIME_PRIORITY_CLASS and does not bypass the Windows USB stack.
