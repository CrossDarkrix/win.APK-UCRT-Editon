# Phase 20-B2.3.3.2 UAC1 Six-Rate Alternate Diagnostic

Based only on the known-good B2.3.3.1 Fresh Virtual DAC Identity build.

UAC1 AudioStreaming interface 1:
- Alt 0: zero bandwidth
- Alt 1: PCM stereo 24-bit 44.1 kHz
- Alt 2: PCM stereo 24-bit 48 kHz
- Alt 3: PCM stereo 24-bit 88.2 kHz
- Alt 4: PCM stereo 24-bit 96 kHz
- Alt 5: PCM stereo 24-bit 176.4 kHz
- Alt 6: PCM stereo 24-bit 192 kHz

Each streaming alt has endpoint 0x01, adaptive isochronous OUT, wMaxPacketSize 576 and bInterval 1. USBFS GET_SPEED remains high-speed. Native USBFS SETINTERFACE maps the selected alt to its exact rate. Stream creation, buffer sizing, and URB completion pacing use that selected rate. UAC1 endpoint SET_CUR/GET_CUR sampling-frequency controls track the selected rate.

No Phase B2.4 route guard, forced reinitialization, submit gate, or CONNECT compatibility behavior is included.
