# Phase 20-B2.3.4.8 Submit/Reap Identity Gate

Purpose:
Measure and enforce USBDEVFS SUBMITURB to REAPURB pointer identity while testing whether UCRT active-map registration races the first replacement-pool completions.

Implementation:
- Each pending entry stores the original URB pointer, buffer pointer, submit id, generation and pool epoch.
- SUBMIT identity diagnostics record the exact pointer accepted by the compatibility layer.
- REAP identity diagnostics compare the queued pointer with the original submit pointer and return that original pointer only when exactMatch=1.
- DISCARD diagnostics report the matched submit id and pool membership.
- On a replacement boundary, REAP is gated until twelve URBs have been submitted and 20 ms have elapsed.
- Nonblocking REAP receives EAGAIN while the gate is closed. Blocking REAP waits without changing generation or connection state.
- The existing soft boundary, WASAPI recreation and CD separation are preserved.

Diagnostics:
- `Phase20-B2.3.4.8: SUBMIT identity ...`
- `Phase20-B2.3.4.8: REAP identity gate armed ...`
- `Phase20-B2.3.4.8: REAP identity gate opened ...`
- `Phase20-B2.3.4.8: REAP identity ... exactMatch=...`
- `Phase20-B2.3.4.8: DISCARD identity ...`
