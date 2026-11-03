package com.macapk.loader;

/** Native Windows SCSI bridge used by android.hardware.usb BOT emulation. */
public final class HostUsbCdBridge {
    private HostUsbCdBridge() {}
    public static native String[] enumerate();
    public static native long open(String devicePath);
    public static native void close(long handle);
    public static native byte[] execute(long handle, byte[] cdb, int inputLength, int timeoutMs);
}
