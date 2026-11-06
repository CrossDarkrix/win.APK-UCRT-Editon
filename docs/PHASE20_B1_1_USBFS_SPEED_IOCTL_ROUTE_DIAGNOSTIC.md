# Phase 20-B1.1 USBFS Speed and ioctl Route Diagnostic

- Adds USBDEVFS_GET_SPEED (0x551F) and returns USBDEVFS_SPEED_HIGH (3).
- Logs every ARM32 and ARM64 by-name ioctl entry.
- Logs every virtual-session ioctl with fd, request, and argument.
- Routes direct ARM64 syscall number 29 (ioctl) for the reserved virtual FD.
- Retains CONNECTINFO and all Phase 20-B1 management operations.
- SUBMITURB and PCM transport remain unsupported and return ENOTTY.
