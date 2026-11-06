# Phase 20-B2 Virtual Isochronous URB PCM Bridge

- Implements Windows bionic g_ioctl routing for virtual FD 0x5A20.
- Handles USBDEVFS_SUBMITURB, REAPURB, REAPURBNDELAY and DISCARDURB.
- Validates ISO OUT endpoint 0x01 and Android x86_64 usbdevfs_urb fields.
- Converts packed signed 24-bit stereo 96 kHz PCM to interleaved float.
- Sends PCM to the existing Windows WASAPI audio engine.
- Updates URB and per-packet status and actual_length, and queues completed URBs.
- Preserves Phase 20-B1.1 speed and management operations.
