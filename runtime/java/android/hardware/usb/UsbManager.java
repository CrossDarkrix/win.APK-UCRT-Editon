package android.hardware.usb;

import android.content.Context;
import com.macapk.loader.HostUsbCdBridge;
import java.util.HashMap;

/** Windows optical drives exposed as Android USB Mass Storage BOT devices. */
public class UsbManager {
    public static final String ACTION_USB_DEVICE_ATTACHED = "android.hardware.usb.action.USB_DEVICE_ATTACHED";
    public static final String ACTION_USB_DEVICE_DETACHED = "android.hardware.usb.action.USB_DEVICE_DETACHED";
    public static final String ACTION_USB_ACCESSORY_ATTACHED = "android.hardware.usb.action.USB_ACCESSORY_ATTACHED";
    public static final String ACTION_USB_ACCESSORY_DETACHED = "android.hardware.usb.action.USB_ACCESSORY_DETACHED";
    public static final String EXTRA_DEVICE = "device";
    public static final String EXTRA_ACCESSORY = "accessory";
    public static final String EXTRA_PERMISSION_GRANTED = "permission";

    public UsbManager() {}
    public UsbManager(Context context) {}

    public HashMap<String, UsbDevice> getDeviceList() {
        HashMap<String, UsbDevice> out = new HashMap<>();
        String[] devices = HostUsbCdBridge.enumerate();
        if (devices == null) return out;
        for (int i = 0; i + 1 < devices.length; i += 2) {
            String path = devices[i];
            String name = devices[i + 1];
            UsbEndpoint bulkOut = new UsbEndpoint(0x01, UsbConstants.USB_ENDPOINT_XFER_BULK, 65536, 0);
            UsbEndpoint bulkIn = new UsbEndpoint(0x81, UsbConstants.USB_ENDPOINT_XFER_BULK, 65536, 0);
            UsbInterface iface = new UsbInterface(0, 0, "Windows CD-ROM",
                    UsbConstants.USB_CLASS_MASS_STORAGE, 0x06, 0x50);
            iface.setEndpoints(new UsbEndpoint[] { bulkIn, bulkOut });
            UsbDevice device = new UsbDevice(path, 0x0000, 0x0000,
                    UsbConstants.USB_CLASS_MASS_STORAGE, 0x06, 0x50,
                    "Windows", name, "1.0", null);
            device.setInterfaces(new UsbInterface[] { iface });
            out.put(path, device);
        }
        return out;
    }

    public UsbAccessory[] getAccessoryList() { return null; }
    public android.os.ParcelFileDescriptor openAccessory(UsbAccessory accessory) { return null; }
    public boolean hasPermission(UsbDevice device) { return device != null; }
    public boolean hasPermission(UsbAccessory accessory) { return false; }
    public void requestPermission(UsbDevice device, android.app.PendingIntent pi) {}
    public void requestPermission(UsbAccessory accessory, android.app.PendingIntent pi) {}
    public UsbDeviceConnection openDevice(UsbDevice device) {
        if (device == null) return null;
        long handle = HostUsbCdBridge.open(device.getDeviceName());
        return handle == 0 ? null : new UsbDeviceConnection(device, handle);
    }
}
