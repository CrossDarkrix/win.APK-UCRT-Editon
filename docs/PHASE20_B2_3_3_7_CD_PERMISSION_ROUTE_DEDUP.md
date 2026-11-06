# Phase 20-B2.3.3.7 CD permission route separation and optical deduplication

Fixes the retry crash caused by routing a Windows optical device through HostUsbDirectBridge.hasPermission.

- Optical devices are identified by the USB Mass Storage / SCSI transparent / Bulk-Only tuple: class 8, subclass 6, protocol 0x50, checking both device and interfaces.
- Optical permission is handled by the Windows CD host path and never calls HostUsbDirectBridge.hasPermission or setPermission.
- Optical open always uses HostUsbCdBridge, including drive-letter fallback paths such as `\\.\D:`.
- UnsatisfiedLinkError is contained for non-optical USB Direct permission calls instead of escaping to the UI thread.
- The SetupAPI CD interface remains authoritative. Drive-letter enumeration now runs only when SetupAPI returns zero devices, preventing one physical drive from appearing twice under two different paths.
- The Win.apk virtual DAC remains present for USB audio compatibility, but its class is audio rather than optical and it is not counted as a CD drive.
- CD SCSI I/O, USB Direct audio, URB recovery, +18 dB gain, and frame pacing are unchanged.
