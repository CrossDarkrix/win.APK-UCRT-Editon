# Phase 20-B2.3.3.3 URB Rate Sync Fix

This build replaces the failed additional JNI bridge approach. It is based on the six-rate diagnostic with no new Java native method.

The actual UAC stream rate is derived from the first isochronous URB packet schedule: total 24-bit stereo PCM bytes, packet count, and the existing high-speed 8000 packets/second model. The estimate is matched only to 44.1/48/88.2/96/176.4/192 kHz within a strict tolerance. The native USB Direct stream is created only after this synchronization. A later real rate change stops the old stream and recreates it at the inferred rate.

This avoids UnsatisfiedLinkError and prevents 192 kHz PCM from being rendered at a stale 96 kHz clock. The descriptor, Java interfaces, USBFS transport, +18 dB gain, and transparent limiter remain unchanged from the six-rate diagnostic. No B2.4 behavior is included.
