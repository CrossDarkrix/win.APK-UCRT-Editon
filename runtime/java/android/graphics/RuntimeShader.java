package android.graphics;

// android.graphics.RuntimeShader -- API 33 programmable AGSL shader. Real Android
// compiles the AGSL string into a Skia SkRuntimeEffect and samples it per-pixel on
// the GPU (or in Skia's CPU backend). Our Canvas is software/AWT-backed and we have
// no AGSL runtime, so this is a no-op store: construction succeeds, every uniform /
// input setter records nothing (or just keeps the value for the contract) and never
// throws. A fill using this shader renders nothing; that "we cannot run AGSL" fact
// is logged at the fill site, not here, to avoid per-call log spam.
//
// The point of this class is binary compatibility: apps that build a RuntimeShader
// (Compose ripple effects, AGSL-based brushes) must be able to construct it and push
// uniforms without a NoSuchMethodError aborting the composition. We mirror the AOSP
// public method surface exactly (names, return void, param types) per
// frameworks/base/graphics/java/android/graphics/RuntimeShader.java.
public class RuntimeShader extends Shader {

    // Kept for the contract / debugging. AOSP holds the compiled effect; we only
    // need the source string so callers that read it back (rare) get something sane.
    private final String agsl;

    public RuntimeShader(String agsl) {
        super();
        this.agsl = agsl;
    }

    // No-op uniform setters. Real Android validates the name against the compiled
    // effect's uniform table; we have no table, so we accept everything silently.

    public void setFloatUniform(String uniformName, float value) {
        // no-op: no AGSL runtime
    }

    public void setFloatUniform(String uniformName, float value1, float value2) {
        // no-op: no AGSL runtime
    }

    public void setFloatUniform(String uniformName, float value1, float value2, float value3) {
        // no-op: no AGSL runtime
    }

    public void setFloatUniform(String uniformName, float value1, float value2, float value3, float value4) {
        // no-op: no AGSL runtime
    }

    public void setFloatUniform(String uniformName, float[] values) {
        // no-op: no AGSL runtime
    }

    public void setIntUniform(String uniformName, int value) {
        // no-op: no AGSL runtime
    }

    public void setIntUniform(String uniformName, int value1, int value2) {
        // no-op: no AGSL runtime
    }

    public void setIntUniform(String uniformName, int value1, int value2, int value3) {
        // no-op: no AGSL runtime
    }

    public void setIntUniform(String uniformName, int value1, int value2, int value3, int value4) {
        // no-op: no AGSL runtime
    }

    public void setIntUniform(String uniformName, int[] values) {
        // no-op: no AGSL runtime
    }

    public void setColorUniform(String uniformName, int color) {
        // no-op: no AGSL runtime
    }

    public void setColorUniform(String uniformName, long color) {
        // no-op: no AGSL runtime
    }

    public void setColorUniform(String uniformName, Color color) {
        // no-op: no AGSL runtime
    }

    public void setInputShader(String shaderName, Shader shader) {
        // no-op: no AGSL runtime; we cannot chain a child shader into a program we
        // do not execute.
    }

    public void setInputBuffer(String shaderName, BitmapShader shader) {
        // no-op: no AGSL runtime
    }

    /** AOSP RuntimeShader.setInputColorFilter(String, ColorFilter) (API 36). */
    public void setInputColorFilter(String colorFilterName, ColorFilter colorFilter) {
        // no-op: no AGSL runtime
    }

    /** AOSP RuntimeShader.setInputXfermode(String, RuntimeXfermode) (API 36). */
    public void setInputXfermode(String xfermodeName, RuntimeXfermode xfermode) {
        // no-op: no AGSL runtime
    }

    /** AOSP RuntimeShader.setWorkingColorSpace(ColorSpace) (API 36) -- the color
     *  space the shader's arithmetic happens in; null means "the destination's".
     *  Stored so it reads back; we do not execute the shader, so it cannot
     *  affect a pixel. */
    public void setWorkingColorSpace(ColorSpace colorSpace) {
        mWorkingColorSpace = colorSpace;
    }
    private ColorSpace mWorkingColorSpace;
}
