# Phase 20-B2.3.4.1 USB DAC rate handoff

The 3.4.0 REAP epoch guard did not change the failure because UCRT uses repeated non-blocking REAP calls. The new log proves that exactly the old 12-URB pool becomes `unknown urb reaped` immediately after 96 kHz to 44.1 kHz switching, then the completion loop stalls and later 96 kHz playback also fails.

Root cause addressed:
- SET_INTERFACE alt 0 and inferred-rate synchronization called the destructive session cancel routine.
- That routine cleared every pending URB before UCRT could DISCARD or REAP the old pool.
- The old completion worker therefore remained alive and could consume completions belonging to the replacement stream.

Implementation:
- Added a non-destructive stream handoff path for sample-rate and alternate-setting changes.
- Rate handoff stops/releases only the current WASAPI sink and resets its timeline.
- Pending URBs, their pointers, queue order and generation are preserved during 96/44.1/96 kHz transitions.
- UCRT can now DISCARD or REAP its own old URBs instead of losing them inside the compatibility layer.
- Destructive cancellation remains unchanged for explicit interface release, reset, disconnect, invalid submission and write failure.
- CD BOT, READ CD, eject, USB gain and pacing are unchanged.
