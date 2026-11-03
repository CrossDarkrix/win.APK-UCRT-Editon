#!/usr/bin/env bash
set -euo pipefail
# Win.apk currently builds with /mingw64 clang binaries, so use the matching
# MINGW64 GStreamer packages to keep ABI and pkg-config paths consistent.
pacman -S --needed \
  mingw-w64-x86_64-gstreamer \
  mingw-w64-x86_64-gst-plugins-base \
  mingw-w64-x86_64-gst-plugins-good \
  mingw-w64-x86_64-gst-plugins-bad

gst-inspect-1.0.exe appsrc >/dev/null
gst-inspect-1.0.exe d3d11upload >/dev/null
gst-inspect-1.0.exe d3d11videosink >/dev/null
echo "[OK] GStreamer D3D11 presentation dependencies are installed."
