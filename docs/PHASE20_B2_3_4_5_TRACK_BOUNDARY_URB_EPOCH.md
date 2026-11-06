# Phase 20-B2.3.4.5 Track-Boundary URB Epoch

Policy:
- USB Direct is more sensitive than the normal headphone path, so recovery is intentionally favored over preserving an old URB pool.
- Every SET_INTERFACE alt 0 is treated as a track/source boundary and forces a DAC-only hard epoch.
- Rate/alternate changes without a preceding alt 0 also force a hard epoch.
- During a new epoch, URB pointers are accumulated until the ring repeats. The first repeated pointer with at least four members establishes the pool.
- After establishment, the first unseen pointer immediately forces another hard epoch. This covers same-rate decoder/file changes without waiting for three unknown pointers.

Hard epoch:
- Cancel and quarantine pending URBs.
- Retire the old pointer pool.
- Stop/release only the USB Direct WASAPI sink.
- Reset pacing, increment generation and pool epoch, and wake REAP waiters.
- Build the replacement pool from subsequent submits and lazily recreate WASAPI.
- CD BOT/HostUsbCdBridge state is untouched.

Diagnostics:
- `Phase20-B2.3.4.5: USB DAC hard epoch ...`
- `Phase20-B2.3.4.5: URB pool established ...`
