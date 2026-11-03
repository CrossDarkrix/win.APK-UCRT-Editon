package com.macapk.loader;
import java.nio.ByteBuffer;
/** Direct2D 1.1 + DirectWrite + D3D11/DXGI renderer. */
public final class NativeD2D {
    private NativeD2D() {}
    public static final int BACKEND_WIC=1, BACKEND_DXGI=2;
    public static final int CMD_SAVE=1, CMD_RESTORE=2, CMD_TRANSFORM=3;
    public static final int CMD_CLIP_RECT=4, CMD_FILL_RECT=5, CMD_FILL_ROUND_RECT=6;
    public static final int CMD_LINE=7, CMD_ELLIPSE=8;
    public static native long create(int width,int height);
    public static native void destroy(long h);
    public static native boolean resize(long h,int width,int height);
    public static native int backend(long h);
    public static native boolean begin(long h,int clearArgb);
    public static native boolean end(long h);
    public static native void save(long h);
    public static native void restore(long h);
    public static native void setTransform(long h,float m11,float m12,float m21,float m22,float dx,float dy);
    public static native void clipRect(long h,float l,float t,float r,float b);
    public static native void clipRoundRect(long h,float l,float t,float r,float b,float rx,float ry);
    public static native void fillRect(long h,float l,float t,float r,float b,int argb);
    public static native void fillRoundRect(long h,float l,float t,float r,float b,float rx,float ry,int argb);
    public static native void drawLine(long h,float x1,float y1,float x2,float y2,float width,int argb);
    public static native void fillEllipse(long h,float l,float t,float r,float b,int argb);
    public static native boolean drawText(long h,String text,String family,float size,float l,float t,float r,float b,int argb,int weight,boolean italic);
    public static native boolean drawBitmap(long h,long bitmap,float l,float t,float r,float b,float alpha);
    public static native long uploadBitmap(long h,int key,int generation,int[] argb,int width,int height);
    public static native void releaseBitmap(long h,long bitmap);
    public static native boolean submit(long h,ByteBuffer commands,int bytes);
    /** D2D-5 RenderNode command-list cache. Available on BACKEND_DXGI. */
    public static native long acquireCommandList(long h,long nodeId,long generation);
    public static native boolean beginCommandList(long h,long commandList);
    public static native boolean endCommandList(long h,long commandList);
    public static native boolean drawCommandList(long h,long commandList,
            float m11,float m12,float m21,float m22,float dx,float dy,float alpha);
    public static native boolean isCommandListReady(long h,long commandList);
    public static native void releaseCommandList(long h,long commandList);
    public static native void invalidateCommandLists(long h,long nodeId);
    public static native int commandListCount(long h);
    public static native boolean readPixels(long h,int[] outArgb);
    public static native String backendInfo(long h);
}
