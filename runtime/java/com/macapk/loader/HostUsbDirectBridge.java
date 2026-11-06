package com.macapk.loader;

/** Windows SetupAPI/WinUSB backend for Android USB host compatibility. */
public final class HostUsbDirectBridge {
    private HostUsbDirectBridge() {}
    public static native String[] enumerate();
    public static native long open(String path);
    public static native void close(long handle);
    public static native byte[] descriptors(long handle);
    public static native boolean claim(long handle, int interfaceNumber, boolean force);
    public static native boolean release(long handle, int interfaceNumber);
    public static native boolean setInterface(long handle, int interfaceNumber, int alternateSetting);
    public static native int control(long handle,int requestType,int request,int value,int index,byte[] buffer,int offset,int length,int timeout);
    public static native int transfer(long handle,int endpoint,byte[] buffer,int offset,int length,int timeout);
    public static native boolean cancel(long handle,int endpoint);
    public static native boolean hasPermission(String packageName,String stableId);
    public static native void setPermission(String packageName,String stableId,boolean granted);
}
