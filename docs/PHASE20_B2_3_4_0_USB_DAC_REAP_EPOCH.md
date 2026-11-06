# Phase 20-B2.3.4.0 USB DAC REAP epoch ownership recovery

Observed failure: switching the virtual USB DAC to 44.1 kHz for CD playback is followed by repeated `unknown urb reaped`, after which both CD playback and USB DAC playback stop.

Root cause fixed:
- A blocking USBDEVFS_REAPURB call could survive an alt-setting/rate generation reset.
- After the reset, that old waiter could consume a URB submitted by the new 44.1 kHz session.
- The new session then received a completion through an old ownership context and reported `unknown urb reaped`. Repeated surviving waiters could drain the new session and permanently stall the DAC.

Implementation:
- Every REAP/REAPNDELAY captures the current USB session generation on entry.
- A generation change releases the old waiter with EAGAIN instead of allowing it to consume a new-session URB.
- The output URB pointer is cleared before waiting, so an error never exposes a stale pointer.
- Generation is rechecked after both condition-variable waits and paced waits.
- Existing rate switching, URB pacing, DISCARD idempotence, CD BOT, CD read/eject, +18 dB gain and WASAPI paths are unchanged.
