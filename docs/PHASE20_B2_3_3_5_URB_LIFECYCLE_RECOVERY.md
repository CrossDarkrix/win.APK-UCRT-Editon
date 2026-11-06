# Phase 20-B2.3.3.5 URB lifecycle recovery

This build fixes the USB Direct transfer freeze where the guest repeatedly logged `unknown urb reaped`, then entered a 303 ms no-completion restart while the PCM write count stopped advancing.

Implemented changes:

- A pending URB now owns a generation and monotonically increasing submit ID.
- Reset, alternate-setting changes, rate changes, release, and disconnect cancel and remove every pending completion atomically before the generation advances.
- REAP never exposes an older-generation completion to the guest. Stale entries are quarantined and counted.
- Blocking REAP rechecks readiness after every wake, including spurious wakeups and notifications caused by later submissions.
- Re-submission of an already-pending URB pointer replaces the obsolete ownership entry instead of allowing duplicate completions for one guest pointer.
- DISCARD is idempotent, removes every matching ownership entry, and leaves the URB cancelled even when a restart has already drained it.
- Diagnostics report submit IDs, stale completion drops, and duplicate-pointer recovery.

The PCM conversion, 192 kHz packet sizing, USB Direct +18 dB gain path, Windows adaptive frame pacer, and all unrelated files remain unchanged.
