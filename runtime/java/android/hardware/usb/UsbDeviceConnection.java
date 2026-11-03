package android.hardware.usb;

import com.macapk.loader.HostUsbCdBridge;
import java.io.FileDescriptor;
import java.util.Arrays;

/** USB Bulk-Only Transport facade backed by the Windows SCSI CD bridge. */
public class UsbDeviceConnection {
    private final UsbDevice mDevice;
    private long mHandle;
    private byte[] mPendingData;
    private byte[] mPendingCsw;
    private int mDataOffset;

    public UsbDeviceConnection(UsbDevice device) { this(device, 0); }
    public UsbDeviceConnection(UsbDevice device, long handle) { mDevice = device; mHandle = handle; }

    public boolean claimInterface(UsbInterface intf, boolean force) { return mHandle != 0 && intf != null; }
    public boolean releaseInterface(UsbInterface intf) { return mHandle != 0; }
    public boolean setInterface(UsbInterface intf) { return mHandle != 0; }
    public boolean setConfiguration(UsbConfiguration configuration) { return mHandle != 0; }
    public int controlTransfer(int requestType, int request, int value, int index,
            byte[] buffer, int length, int timeout) { return 0; }
    public int controlTransfer(int requestType, int request, int value, int index,
            byte[] buffer, int offset, int length, int timeout) { return 0; }

    public synchronized int bulkTransfer(UsbEndpoint endpoint, byte[] buffer, int length, int timeout) {
        return bulkTransfer(endpoint, buffer, 0, length, timeout);
    }

    public synchronized int bulkTransfer(UsbEndpoint endpoint, byte[] buffer, int offset,
            int length, int timeout) {
        if (mHandle == 0 || endpoint == null || buffer == null || offset < 0 || length < 0
                || offset + length > buffer.length) return -1;
        if (endpoint.getDirection() == UsbConstants.USB_DIR_OUT) {
            if (length != 31 || le32(buffer, offset) != 0x43425355) return -1;
            int tag = le32(buffer, offset + 4);
            int transferLength = le32(buffer, offset + 8);
            int cdbLength = buffer[offset + 14] & 0x1f;
            if (cdbLength < 1 || cdbLength > 16) return -1;
            byte[] cdb = Arrays.copyOfRange(buffer, offset + 15, offset + 15 + cdbLength);
            byte[] result = HostUsbCdBridge.execute(mHandle, cdb, transferLength, timeout);
            boolean ok = result != null;
            mPendingData = ok ? result : new byte[0];
            mDataOffset = 0;
            int residue = Math.max(0, transferLength - mPendingData.length);
            mPendingCsw = csw(tag, residue, ok ? 0 : 1);
            return length;
        }
        if (mPendingData != null && mDataOffset < mPendingData.length) {
            int n = Math.min(length, mPendingData.length - mDataOffset);
            System.arraycopy(mPendingData, mDataOffset, buffer, offset, n);
            mDataOffset += n;
            return n;
        }
        if (mPendingCsw != null) {
            int n = Math.min(length, mPendingCsw.length);
            System.arraycopy(mPendingCsw, 0, buffer, offset, n);
            mPendingData = null; mPendingCsw = null; mDataOffset = 0;
            return n;
        }
        return -1;
    }

    private static int le32(byte[] b, int o) {
        return (b[o] & 255) | ((b[o+1] & 255) << 8) | ((b[o+2] & 255) << 16) | ((b[o+3] & 255) << 24);
    }
    private static void put32(byte[] b, int o, int v) {
        b[o]=(byte)v; b[o+1]=(byte)(v>>>8); b[o+2]=(byte)(v>>>16); b[o+3]=(byte)(v>>>24);
    }
    private static byte[] csw(int tag, int residue, int status) {
        byte[] b=new byte[13]; put32(b,0,0x53425355); put32(b,4,tag); put32(b,8,residue); b[12]=(byte)status; return b;
    }

    public UsbRequest requestWait() { return null; }
    public UsbRequest requestWait(long timeout) throws java.util.concurrent.TimeoutException {
        if (timeout < 0) throw new IllegalArgumentException("timeout must not be negative");
        throw new java.util.concurrent.TimeoutException("no asynchronous USB request completed");
    }
    public byte[] getRawDescriptors() { return null; }
    public String getSerial() { return mDevice != null ? mDevice.getSerialNumber() : null; }
    public int getFileDescriptor() { return -1; }
    public FileDescriptor getFileDescriptorObject() { return null; }
    public synchronized void close() { if (mHandle != 0) { HostUsbCdBridge.close(mHandle); mHandle=0; } }
}
