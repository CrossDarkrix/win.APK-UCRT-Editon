# Win.apk Direct2D D2D-4

Implemented native backend: Direct2D 1.1, DirectWrite, D3D11 BGRA device, DXGI surface target, staging readback, WIC fallback, brush cache, generation-keyed bitmap cache, resize/device resource recreation, and a direct ByteBuffer command decoder for primitive drawing.

The existing Java2D Canvas remains the compatibility path. NativeD2D is the backend API used to migrate RenderNode/Canvas command recording incrementally.
