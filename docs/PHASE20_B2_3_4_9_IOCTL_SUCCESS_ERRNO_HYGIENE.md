# Phase 20-B2.3.4.9 ioctl Success Errno Hygiene

Purpose:
Eliminate stale guest errno values after successful virtual USBFS operations and expose the before/after state for UCRT registration diagnostics.

Implementation:
- Successful SUBMITURB explicitly sets guest errno to zero before returning 0.
- Successful REAPURB/REAPURBNDELAY explicitly sets guest errno to zero before returning the original submitted URB pointer.
- Successful DISCARDURB explicitly sets guest errno to zero.
- GET_SPEED, CLAIM_INTERFACE, RELEASE_INTERFACE, SET_INTERFACE, RESET and DISCONNECT success paths use a common p20_success helper.
- Failure and EAGAIN paths retain their existing nonzero errno behavior.
- Submit/Reap identity gate, soft boundary, per-connection diagnostics, WASAPI recreation and CD separation remain enabled.

Diagnostics:
- `Phase20-B2.3.4.9: SUBMIT result ... guestErrnoBefore=... guestErrnoAfter=0`
- `Phase20-B2.3.4.9: REAP result ... guestErrnoBefore=... guestErrnoAfter=0`
- `Phase20-B2.3.4.9: DISCARD result ... guestErrnoBefore=... guestErrnoAfter=0`
- `Phase20-B2.3.4.9: ioctl success ... guestErrnoBefore=... guestErrnoAfter=0`
