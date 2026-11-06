# Phase 20-B2.3.4.7 Per-Connection USBFS Session

Root cause addressed:
- Every virtual UsbDeviceConnection previously returned the same pseudo-fd 0x5A20.
- An old StreamThread could therefore REAP completions submitted by the replacement StreamThread and report all twelve replacement pointers as unknown.

Implementation:
- Allocate a monotonically increasing pseudo-fd for every openVirtual connection.
- Route usbfs ioctl through the calling pseudo-fd.
- The newest connection becomes the active USBFS session.
- A stale lower fd is rejected and can never read the active session completion queue.
- Activating a newer connection cancels the old compatibility queue, resets pool bookkeeping and transfers WASAPI ownership to the new session.
- Old close remains unable to stop a newer connection's output route.
- CD BOT and HostUsbCdBridge remain separate.

Diagnostics:
- `Phase20-B2.3.4.7: openVirtual ... fd=...`
- `Phase20-B2.3.4.7: USBFS session activated oldFd=... newFd=...`
- `Phase20-B2.3.4.7: stale USBFS session rejected ...`
