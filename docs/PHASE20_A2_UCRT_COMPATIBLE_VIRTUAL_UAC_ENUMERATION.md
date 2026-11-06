# Phase 20-A2 UCRT-Compatible Virtual UAC Enumeration

- Keeps the virtual USB device visible through UsbManager.deviceList.
- Verifies UsbEndpoint type and direction use the AOSP masks required by UCRT isDac().
- Supplies a complete minimal UAC1 playback descriptor chain: AC header, USB-streaming input terminal, speaker output terminal, AS alt 0/1, AS general, Type I PCM format, isochronous OUT endpoint, and class-specific endpoint.
- Advertises stereo, packed 24-bit PCM at 96 kHz with a 576-byte adaptive OUT endpoint.
- Implements the UAC1 GET_CUR sampling-frequency response for endpoint 1.
- PCM transport remains reserved for Phase 20-B.
