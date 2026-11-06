# Phase 20-B2.3.4.10 DISCARD Completion Preservation

## Objective

Make the Win.apk virtual usbfs `DISCARDURB` behavior compatible with UCRT's shutdown/drain sequence. A discarded pending URB is no longer erased. It is converted into an immediately reapable cancelled completion.

## Behavior

- Locate the pending entry whose URB pointer exactly matches the `DISCARDURB` argument.
- Preserve that pending entry in FIFO order.
- Set URB status to `-2`, total actual length to zero and error count to zero.
- Set every isochronous packet descriptor to status `-2` and actual length zero.
- Move the completion ready time to `steady_clock::now()`.
- Wake blocking and polling REAP paths.
- A repeated DISCARD remains successful and does not enqueue a duplicate completion.
- If the pointer has already been reaped, DISCARD remains idempotently successful.
- Successful DISCARD continues to clear guest errno.

## Expected handoff

1. Old UCRT stream issues DISCARD for each in-flight old URB.
2. Win.apk retains each old URB as a cancelled completion.
3. Old UCRT `drain()` reaps the matching old addresses and marks its own `inFlight` entries false.
4. Only after old drain finishes should replacement URB completions be consumed by the new stream loop.

## Diagnostics

- `Phase20-B2.3.4.10: DISCARD completion preservation ... preserved=1`
- `Phase20-B2.3.4.8: REAP identity ... status=-2 cancelled=1`
- `Phase20-B2.3.4.10: DISCARD result ... completionRetained=1`
