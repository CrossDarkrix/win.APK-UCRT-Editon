# Efficiency Mode Off

- Disables PROCESS_POWER_THROTTLING_EXECUTION_SPEED for the launcher and every runtime child process.
- Reapplies HIGH_PRIORITY_CLASS to launcher and runtime child processes.
- Disables THREAD_POWER_THROTTLING_EXECUTION_SPEED on the audio render thread.
- Keeps MMCSS Pro Audio/Critical and TIME_CRITICAL fallback unchanged.
- Uses dynamic API resolution and logs unsupported/failure states instead of preventing startup.
