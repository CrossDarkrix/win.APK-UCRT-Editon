// android.graphics.RenderEffect -- API-31 GPU post-process effect attached to a
// RenderNode (graphicsLayer { renderEffect = ... } in Compose: blur, offset,
// color-filter, shader). Was an empty stub; v1.0.434 stores the BLUR variant so
// Canvas.drawRenderNode can approximate it with a java.awt ConvolveOp on the
// recorded layer bitmap (Material3 modal scrim / glassmorphism blur). The other
// factory shapes resolve (so construction never NoClassDefFoundError/NoSuchMethod)
// but render unblurred. createColorFilterEffect now RETAINS its ColorFilter
// (macapkColorFilter()) so the state is no longer thrown away at construction;
// Canvas.drawRenderNode still has to run that filter over the recorded layer
// for it to become visible. Mirrors
// frameworks/base/graphics/java/android/graphics/RenderEffect.java factories.
package android.graphics;

public class RenderEffect {
    private boolean blur;
    private float blurRadiusX, blurRadiusY;
    private ColorFilter colorFilter;

    public RenderEffect() {}

    public static RenderEffect createBlurEffect(float radiusX, float radiusY, Shader.TileMode edgeTreatment) {
        RenderEffect e = new RenderEffect();
        e.blur = true; e.blurRadiusX = radiusX; e.blurRadiusY = radiusY;
        return e;
    }
    public static RenderEffect createBlurEffect(float radiusX, float radiusY,
                                                RenderEffect inputEffect, Shader.TileMode edgeTreatment) {
        return createBlurEffect(radiusX, radiusY, edgeTreatment);
    }
    public static RenderEffect createOffsetEffect(float offsetX, float offsetY) { return new RenderEffect(); }
    public static RenderEffect createOffsetEffect(float offsetX, float offsetY, RenderEffect input) { return new RenderEffect(); }
    // AOSP RenderEffect.createColorFilterEffect(ColorFilter[, RenderEffect])
    // recolors the layer beneath it (the grayscale / tint / disabled-state
    // scrim pattern). Retain the filter so the effect carries the state the
    // draw path needs; it is the same filter object Paint.setColorFilter takes,
    // so applying it is the color-filter compositing Canvas already does. The
    // two-argument form composes over `input`, which we keep as the chained
    // source; a chain whose input is a blur must still blur, so inherit the
    // input's state the way createChainEffect does.
    public static RenderEffect createColorFilterEffect(ColorFilter colorFilter) {
        RenderEffect e = new RenderEffect();
        e.colorFilter = colorFilter;
        return e;
    }
    public static RenderEffect createColorFilterEffect(ColorFilter colorFilter, RenderEffect input) {
        RenderEffect e = new RenderEffect();
        if (input != null) {
            e.blur = input.blur;
            e.blurRadiusX = input.blurRadiusX;
            e.blurRadiusY = input.blurRadiusY;
        }
        e.colorFilter = colorFilter;
        return e;
    }
    public static RenderEffect createShaderEffect(Shader shader) { return new RenderEffect(); }
    public static RenderEffect createChainEffect(RenderEffect outer, RenderEffect inner) {
        return outer != null ? outer : inner;
    }
    public static RenderEffect createBitmapEffect(Bitmap bitmap) { return new RenderEffect(); }
    public static RenderEffect createBitmapEffect(Bitmap bitmap, Rect src, Rect dst) { return new RenderEffect(); }
    public static RenderEffect createBlendModeEffect(RenderEffect dst, RenderEffect src, BlendMode blendMode) {
        return src != null ? src : dst;
    }
    public static RenderEffect createRuntimeShaderEffect(RuntimeShader shader, String uniformShaderName) {
        return new RenderEffect();
    }

    /** Internal: true when this is a blur effect Canvas.drawRenderNode should
     *  approximate. */
    public boolean macapkIsBlur() { return blur; }
    public float macapkBlurRadiusX() { return blurRadiusX; }
    public float macapkBlurRadiusY() { return blurRadiusY; }
    /** Internal: the ColorFilter a createColorFilterEffect carries, or null.
     *  Canvas.drawRenderNode must run it over the recorded layer bitmap the
     *  same way it already applies the blur above, otherwise the effect is
     *  silently invisible. */
    public ColorFilter macapkColorFilter() { return colorFilter; }
}
