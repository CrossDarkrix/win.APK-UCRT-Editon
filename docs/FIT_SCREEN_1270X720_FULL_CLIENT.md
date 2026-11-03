# Fit screen 1270 x 720

- The launcher Fit screen preset now requests a 1270 x 720 render surface.
- Fit screen sets `MACAPK_LETTERBOX=0`.
- Every submitted frame is scaled to the complete Win32 client area by the
  existing `StretchDIBits` path, including live window resizing.
- Mouse/touch coordinates and child windows use the same full-client rectangle.
- Other device presets retain aspect-preserving letterboxing.
