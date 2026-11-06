# Phase 20-B2.3.4.6 USB DAC Soft Boundary

Problem addressed:
Phase 20-B2.3.4.5 correctly detected 12-URB pool replacement, but incrementing generation and waking REAP caused UCRT to restart transfer bookkeeping. The replacement pool was then reaped after UCRT discarded its active map, producing twelve `unknown urb reaped` messages and a permanent stall.

Soft-boundary policy:
- Track, source, rate and first-unseen-pointer boundaries still stop/release the USB Direct WASAPI sink and reset pacing.
- Current URB pointers are marked retired and a replacement pool is built.
- Pending completions are preserved.
- Generation is not incremented.
- Blocking/timed REAP is not notified or released with EAGAIN.
- Existing REAP naturally drains scheduled old completions and then receives replacement-pool completions through the same worker lifecycle.
- DISCARD removes matching pending entries and retired bookkeeping without waking the REAP worker.
- Explicit reset/disconnect remains destructive and may increment generation.
- CD BOT and HostUsbCdBridge state remain untouched.

Primary diagnostics:
- `Phase20-B2.3.4.6: USB DAC soft boundary ... reapPreserved=1`
- `Phase20-B2.3.4.6: DISCARD retired ...`
