# Phase 20-B2.3.3.6 CD enumeration and retry recovery

Fixes the Windows optical drive not appearing and the application freezing after Retry.

- Keeps SetupAPI GUID_DEVINTERFACE_CDROM enumeration as the primary path.
- Adds Windows DRIVE_CDROM drive-letter fallback paths (for example, `\\.\D:`) and deduplicates them.
- Makes the JNI count/fill enumeration race-safe with up to three bounded resize attempts.
- Runs CD enumeration on one daemon worker and limits each UsbManager.getDeviceList wait to 1200 ms.
- If Windows enumeration is slow, Retry returns the last completed cache instead of blocking the application UI. The in-flight worker remains the only enumeration and later calls consume its result.
- Local JNI references are released while constructing the returned String array.
- USB Direct, URB recovery, audio gain, frame pacing, and SCSI command execution are unchanged.
