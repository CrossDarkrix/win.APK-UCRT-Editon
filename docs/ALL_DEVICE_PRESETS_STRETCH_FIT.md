# All device presets: full-client stretch fit

Every launcher resolution preset now sets `MACAPK_LETTERBOX=0`. The selected
preset continues to define the guest render resolution, but the complete source
frame is scaled to the complete Win32 client area. Aspect ratio is not fixed,
black bars are not created, and resizing/maximizing keeps the rendering attached
to all four window edges. Mouse/touch mapping and child windows use that same
full-client rectangle.
