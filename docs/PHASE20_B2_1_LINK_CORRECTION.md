# Phase 20-B2.1 Link Correction

- g_ioctl remains internal to stdio.cpp's anonymous namespace.
- Exports winapk_phase20_usbfs_ioctl with C linkage after the anonymous namespace closes.
- Direct x86_64 syscall route in misc.cpp calls the exported bridge.
- Removes the invalid cross-translation-unit declaration of g_ioctl.
- URB routing, PCM conversion, WASAPI output and real-time pacing are unchanged.
