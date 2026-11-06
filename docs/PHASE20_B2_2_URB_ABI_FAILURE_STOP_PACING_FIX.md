# Phase 20-B2.2 URB ABI Layout, Failure Stop and Pacing Fix

- Corrects x86_64 usbdevfs_urb natural alignment: buffer at offset 16, packet descriptors at offset 56.
- Adds compile-time size and offset assertions.
- Logs all URB fields and the first 56 bytes before validation.
- Latches invalid submissions into FAILED_STOPPED instead of allowing a hot retry loop.
- Cancels queued URBs and stops the PCM sink on alternate setting 0, release, reset, disconnect, invalid submit or WASAPI failure.
- Advances a session generation on reset/stop boundaries.
- Preserves asynchronous submission and 576,000-byte/second completion pacing.
