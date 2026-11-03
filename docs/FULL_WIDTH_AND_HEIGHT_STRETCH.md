# Full width and height stretch

The remaining horizontal black bars came from the GStreamer D3D11 presenter,
which still used `force-aspect-ratio=true`. This revision sets it to `false`.
The Win32 fallback also disables letterboxing unconditionally. Both presenters
therefore use the complete client width and height for every launcher preset.
