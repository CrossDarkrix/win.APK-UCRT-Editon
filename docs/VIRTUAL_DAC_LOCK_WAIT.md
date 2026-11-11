# Virtual DAC Lock Wait Telemetry

- Measures elapsed steady-clock time immediately around acquisition of `g_p20_lock`.
- Records sample count, sum, maximum, and threshold counts over 500 us, 1 ms, 2 ms, and 5 ms.
- Adds current `lockWaitUs`, aggregate maximum, and average to virtual-DAC completion logs.
- Emits the first 16 slow waits, then every 250th slow wait, plus an aggregate every 2500 ioctl calls.
- Keeps all counters under the existing lock and introduces no secondary lock.
- Preserves PCM silence fill, virtual-DAC MMCSS priority, URB pool/generation logic, and DISCARD completion retention.
