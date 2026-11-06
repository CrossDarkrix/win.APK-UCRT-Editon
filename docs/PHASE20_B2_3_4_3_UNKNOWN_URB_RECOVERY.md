# Phase 20-B2.3.4.3 Unknown URB recovery

Observed failure:
- Switching from CD playback back to a local source can leave the old CD-rate URB pool visible to the completion path.
- UCRT has already replaced its source-side URB ownership and reports those late pointers as Unknown URB.

Recovery implementation:
- Every sample-rate or alternate-setting handoff now performs a DAC-only hard recovery.
- Stop and release only the current USB Direct WASAPI sink.
- Mark every pending old-generation URB as cancelled and quarantine it by clearing the compatibility pending queue.
- Increment the generation so blocking, timed and non-blocking REAP calls leave the old epoch.
- Reset the pacing timeline and failure state.
- Recreate the WASAPI sink lazily on the first new-generation URB, using the newly selected/inferred sample rate.
- CD BOT state and HostUsbCdBridge ownership are not modified.
- Virtual DAC soft-close separation from Phase 20-B2.3.4.2 remains in place.

Recovery log:
`Phase20-B2.3.4.3: Unknown URB recovery hard reset ...`

This deliberately replaces the previous preserved-pending handoff. It prevents an old URB pointer from being returned after UCRT has installed the replacement pool.
