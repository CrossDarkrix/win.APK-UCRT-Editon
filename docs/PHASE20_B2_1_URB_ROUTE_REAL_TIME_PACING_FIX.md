# Phase 20-B2.1 URB Route and Real-Time Pacing Fix

- Routes x86_64 direct syscall number 16 (ioctl) into the same g_ioctl implementation used by imported ioctl.
- Logs every virtual USBFS request at the final Windows route.
- Schedules URB completion from PCM byte count at 576,000 bytes/second (96 kHz, stereo, packed 24-bit).
- Blocking REAPURB waits until the scheduled completion; REAPURBNDELAY returns EAGAIN until ready.
- Keeps SUBMITURB asynchronous and supports multiple queued URBs.
- Removes the repeated optional HostUsbDirectBridge.enumerate JNI call when that native method is unavailable.
