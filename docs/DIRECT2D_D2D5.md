# Direct2D D2D-5

Adds an ID2D1CommandList cache keyed by RenderNode ID and generation. Command lists can be acquired, recorded using all existing NativeD2D primitive/submit calls, closed, tested, transformed and replayed, invalidated by node, counted and released. Resize and device recreation invalidate all command lists. D2D-4 WIC remains a compatibility fallback; command lists require the D3D11/DXGI Direct2D 1.1 backend. Alpha values below 1 currently return false so callers can use the existing compatibility path rather than render with incorrect opacity.
