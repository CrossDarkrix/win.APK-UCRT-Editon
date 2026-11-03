// HostRadios -- the host's Wi-Fi and Bluetooth radios, for WifiManager and
// BluetoothAdapter (Win.apk: windows/host/native_radios.cpp, on the Native Wifi
// and Bluetooth APIs).
//
// A host without the natives (the Mac, which binds none of these) throws
// UnsatisfiedLinkError; every accessor then answers "not known", and the
// callers keep the answers they gave before this class. Not remembered: a call
// made before the host has bound its natives must not disable them for the run.
package com.macapk.loader;

public final class HostRadios {
    private HostRadios() {}

    /** WifiManager.WIFI_STATE_* of the first Wi-Fi interface, or -1 when the
     *  host has no Wi-Fi interface or cannot say. */
    public static int wifiState() {
        try {
            return nativeWifiState();
        } catch (UnsatisfiedLinkError e) {
            return -1;
        }
    }

    /** The current Wi-Fi association, {ssid, bssid, rssi dBm, frequency MHz,
     *  link Mbps}, or null when not associated or not known. SSID and BSSID are empty
     *  when the host withholds it (Windows asks location consent for it, as
     *  Android does). */
    public static String[] wifiConnection() {
        try {
            return nativeWifiConnection();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** The networks of the last scan, five strings each: ssid, bssid,
     *  capabilities ("[WPA2-PSK-CCMP][ESS]"), level dBm, frequency MHz. Null
     *  when not known. */
    public static String[] wifiScanResults() {
        try {
            return nativeWifiScanResults();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** Asks the host for a fresh scan; its results replace the list a few
     *  seconds later. False when no scan could be started. */
    public static boolean wifiStartScan() {
        try {
            return nativeWifiStartScan();
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    /** The host's Bluetooth radio, {name, address "AA:BB:..", "1" when on else
     *  "0"}, or null when there is none or it is not known. */
    public static String[] bluetoothRadio() {
        try {
            return nativeBluetoothRadio();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** The paired devices, three strings each: name, address, class of device
     *  (decimal). Null when not known. */
    public static String[] bluetoothBonded() {
        try {
            return nativeBluetoothBonded();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    private static native int nativeWifiState();
    private static native String[] nativeWifiConnection();
    private static native String[] nativeWifiScanResults();
    private static native boolean nativeWifiStartScan();
    private static native String[] nativeBluetoothRadio();
    private static native String[] nativeBluetoothBonded();
}
