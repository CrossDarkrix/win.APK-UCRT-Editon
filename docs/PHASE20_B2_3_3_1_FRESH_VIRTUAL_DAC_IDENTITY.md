# Phase 20-B2.3.3.1 Fresh Virtual DAC Identity Diagnostic

This build is based only on the last known-good B2.3.3.1 transparent soft limiter build.

Changed virtual Android USB identity:
- Device file/name: /dev/bus/usb/virtual/ucrt-dac-fresh-002
- Product name: Win.apk Fresh Virtual USB DAC
- Device version: 2.01
- Serial number: UCRT-DAC-FRESH-002

Intentionally unchanged:
- VID:PID 1209:A20A
- USB descriptors, interfaces, alternate settings and endpoint
- USBFS/URB behavior
- WASAPI USB Direct path
- +18 dB default and transparent soft limiter

Purpose: distinguish stale app/runtime identity state from a regression in USB transport code.
