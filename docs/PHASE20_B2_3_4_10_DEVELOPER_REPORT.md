# USB Direct rate-change failure report

## Report purpose

This report is ready to send to the UCRT developer if Phase 20-B2.3.4.10 does not resolve the rate-change failure.

## Environment

- UCRT version: `v0.7.1.test0.8.3`
- Win.apk virtual DAC: VID:PID `1209:A20A`
- USB Audio profile: UAC1-equivalent, interface 1, isochronous OUT endpoint `0x01`
- Supported rates: 44.1, 48, 88.2, 96, 176.4 and 192 kHz
- URB pool: 12 URBs, 32 packets per URB
- Initial playback at 96 kHz succeeds.
- After switching to 44.1 kHz, UCRT reports `unknown urb reaped` twelve times, followed by `stall (no completion for ~302 ms), restarting transfers` and `head=0`.
- Switching back to 96 kHz reproduces the same failure.

## UCRT source paths involved

- `app/src/main/java/com/uyatame/ucrt/player/usb/UsbDac.kt`
- `app/src/main/java/com/uyatame/ucrt/player/usb/Usbfs.kt`
- Equivalent legacy sources are also present under `legacy/src/main/java/com/uyatame/ucrt/legacy/usb/`.

## Relevant UCRT behavior

`UsbDac.kt` identifies a completion by comparing the returned native address with the current stream's URB array:

```kotlin
private fun urbAt(addr: Long): IsoUrb? {
    for (i in urbs.indices) if (urbs[i].address == addr) return urbs[i]
    return null
}
```

The normal loop logs an unknown completion and continues without clearing any current URB:

```kotlin
val u = urbAt(reapedAddress(ptr))
if (u == null) {
    AudioEngine.log("usb dac: unknown urb reaped")
    continue
}
u.inFlight = false
```

The shutdown path first discards every old in-flight URB and then performs blocking REAP until the old array is drained:

```kotlin
for (u in urbs) if (u.inFlight) {
    runCatching { Usbfs.ioctl(fd, Usbfs.DISCARDURB, u.urb) }
}
while (urbs.any { it.inFlight } && guard++ < 64) {
    if (Usbfs.ioctl(fd, Usbfs.REAPURB, ptr) < 0) break
    urbAt(reapedAddress(ptr))?.inFlight = false
}
```

## Win.apk defect addressed in Phase 20-B2.3.4.10

Before this phase, Win.apk handled `DISCARDURB` by erasing the matching pending completion. UCRT then entered blocking `REAPURB` waiting for old completions that could no longer be returned. When replacement transfers were submitted, the old drain could consume replacement completions. Their addresses were absent from the old `urbs` array, so the old drain ignored them. The replacement loop then observed foreign or missing completions and stalled.

Phase 20-B2.3.4.10 changes Win.apk to preserve discarded pending URBs as immediately reapable cancelled completions with URB and ISO packet status `-2` and zero actual lengths.

## Previous diagnostic phases and results

- B2.3.4.3: Unknown-URB recovery. Recovery triggered but did not restore playback.
- B2.3.4.4: URB pool epoch detection. New 12-URB pools were detected, but playback still stalled.
- B2.3.4.5: Hard epoch on track/rate boundary. Caused transfer bookkeeping restart and did not fix the issue.
- B2.3.4.6: Soft boundary without generation change. Unknown URBs remained.
- B2.3.4.7: Per-connection pseudo-FD. Logs proved only one connection and one FD were used during rate changes.
- B2.3.4.8: Submit/Reap identity gate. Returned pointer exactly matched the submitted pointer; a 12-URB plus 20 ms gate did not fix the issue.
- B2.3.4.9: Success errno hygiene. SUBMIT returned 0 with errno 0; successful REAP cleared stale EAGAIN, but the failure remained.

## Evidence already established

- Initial 96 kHz playback works.
- SUBMIT and REAP pointer identity is exact in the compatibility layer.
- The replacement pool contains 12 valid URBs.
- The issue is not caused by stale success errno.
- The issue is not caused by a second virtual USB connection.
- The failing sequence is associated with old-transfer cancellation/drain and replacement-pool startup.

## Expected Phase 20-B2.3.4.10 logs

A successful old-pool shutdown should show twelve pairs similar to:

```text
Phase20-B2.3.4.10: DISCARD completion preservation ... preserved=1 status=-2
Phase20-B2.3.4.8: REAP identity ... exactMatch=1 status=-2 cancelled=1
```

The replacement stream should then run without:

```text
usb dac: unknown urb reaped
usb dac: stall (no completion ...)
```

## Request to UCRT developer if failure remains

Please serialize USB DAC stream replacement so the old stream has completed `DISCARDURB` plus `drain()` and its worker thread has terminated before a replacement stream submits URBs on the same usbfs file descriptor.

Recommended defensive diagnostics or changes:

1. Log the old and new `StreamThread` identities, native URB addresses and thread names around stop, drain, start and submit.
2. In `drain()`, log any reaped address that is absent from the old `urbs` array instead of silently ignoring it.
3. Prevent a new stream from entering `loopBody()` until the previous stream's `drain()` has completed.
4. If overlapping streams are intentional, provide per-stream completion ownership instead of allowing both loops to consume the same REAP queue.
5. Apply the same lifecycle correction to the equivalent `legacy` implementation if it is still built or selectable.

## Regression scope

Please preserve:

- Normal non-USB headphone output.
- Initial USB Direct playback.
- Six-rate switching.
- Feedback and no-feedback DAC paths.
- CD drive enumeration, BOT commands, READ CD and eject behavior.
- Existing volume and ReplayGain processing.
