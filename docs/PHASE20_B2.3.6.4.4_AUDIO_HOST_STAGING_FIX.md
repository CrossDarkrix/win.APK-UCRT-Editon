# Phase 20-B2.3.6.4.4 Audio Host Staging Fix

- Requires and copies winapk-audio-host.exe in stage_runtime.sh.
- Copies the host into the UCRT GStreamer distribution in both build scripts.
- Places the host in the runtime target directory for direct build-tree use.
- Packaging and installer already consume the staged Win.apk tree recursively, so no separate installer file list is required.
