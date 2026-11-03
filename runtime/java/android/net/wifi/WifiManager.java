// android.net.wifi.WifiManager -- Wi-Fi service. The state, the connection and
// the scan results come from the host's radio when the host reports one
// (com.macapk.loader.HostRadios); controls that would change the host's Wi-Fi
// (enable, connect, add networks) answer "not possible".
package android.net.wifi;

import android.net.DhcpInfo;
import android.os.WorkSource;
import java.util.Collections;
import java.util.List;

public class WifiManager {
    public static final int WIFI_STATE_DISABLING = 0;
    public static final int WIFI_STATE_DISABLED  = 1;
    public static final int WIFI_STATE_ENABLING  = 2;
    public static final int WIFI_STATE_ENABLED   = 3;
    public static final int WIFI_STATE_UNKNOWN   = 4;

    // Action constants -- F-Droid's WifiStateChangeService and similar
    // listeners register IntentFilters with these.
    public static final String WIFI_STATE_CHANGED_ACTION = "android.net.wifi.WIFI_STATE_CHANGED";
    public static final String NETWORK_STATE_CHANGED_ACTION = "android.net.wifi.STATE_CHANGE";
    public static final String SCAN_RESULTS_AVAILABLE_ACTION = "android.net.wifi.SCAN_RESULTS";
    public static final String NETWORK_IDS_CHANGED_ACTION = "android.net.wifi.NETWORK_IDS_CHANGED";
    public static final String SUPPLICANT_STATE_CHANGED_ACTION = "android.net.wifi.supplicant.STATE_CHANGE";
    public static final String SUPPLICANT_CONNECTION_CHANGE_ACTION = "android.net.wifi.supplicant.CONNECTION_CHANGE";
    public static final String CONFIGURED_NETWORKS_CHANGED_ACTION = "android.net.wifi.CONFIGURED_NETWORKS_CHANGE";
    public static final String RSSI_CHANGED_ACTION = "android.net.wifi.RSSI_CHANGED";

    public static final String EXTRA_WIFI_STATE = "wifi_state";
    public static final String EXTRA_PREVIOUS_WIFI_STATE = "previous_wifi_state";
    public static final String EXTRA_NEW_STATE = "newState";
    public static final String EXTRA_NETWORK_INFO = "networkInfo";
    public static final String EXTRA_BSSID = "bssid";
    public static final String EXTRA_WIFI_INFO = "wifiInfo";
    public static final String EXTRA_NEW_RSSI = "newRssi";
    public static final String EXTRA_SUPPLICANT_CONNECTED = "connected";

    public static final int WIFI_MODE_FULL = 1;
    public static final int WIFI_MODE_SCAN_ONLY = 2;
    public static final int WIFI_MODE_FULL_HIGH_PERF = 3;
    public static final int WIFI_MODE_FULL_LOW_LATENCY = 4;

    private final WifiInfo info = new WifiInfo();
    private final DhcpInfo dhcp = new DhcpInfo();

    public WifiManager() {}

    // ---- the host's radio (com.macapk.loader.HostRadios) -------------------
    //
    // A host that reports its Wi-Fi (Win.apk) answers with the real state,
    // connection and scan results; one that does not (HostRadios.wifiState()
    // < 0) keeps the stub answers: "on", a nameless connection, no networks.

    public boolean isWifiEnabled() {
        int s = com.macapk.loader.HostRadios.wifiState();
        return s < 0 || s == WIFI_STATE_ENABLED;
    }

    public int getWifiState() {
        int s = com.macapk.loader.HostRadios.wifiState();
        return s < 0 ? WIFI_STATE_ENABLED : s;
    }

    public boolean setWifiEnabled(boolean on) { return false; }

    public WifiInfo getConnectionInfo() {
        String[] c = com.macapk.loader.HostRadios.wifiConnection();
        if (c == null) {
            if (com.macapk.loader.HostRadios.wifiState() < 0) return info;
            return new WifiInfo(null, "02:00:00:00:00:00", WifiInfo.INVALID_RSSI, -1,
                    WifiInfo.LINK_SPEED_UNKNOWN, 0, SupplicantState.DISCONNECTED);
        }
        String ssid = c[0].isEmpty() ? null : c[0];
        String bssid = c[1].isEmpty() ? "02:00:00:00:00:00" : c[1];
        return new WifiInfo(ssid, bssid, parse(c[2], WifiInfo.INVALID_RSSI), parse(c[3], -1),
                parse(c[4], WifiInfo.LINK_SPEED_UNKNOWN), hostIpv4(), SupplicantState.COMPLETED);
    }

    private static int parse(String s, int fallback) {
        try { return s.isEmpty() ? fallback : Integer.parseInt(s); }
        catch (NumberFormatException e) { return fallback; }
    }

    /** The host's IPv4 address on its default route, as AOSP's little-endian
     *  int. Connecting a UDP socket sends nothing; it only picks the route. */
    private static int hostIpv4() {
        try (java.net.DatagramSocket s = new java.net.DatagramSocket()) {
            s.connect(java.net.InetAddress.getByName("192.0.2.1"), 9);
            byte[] a = s.getLocalAddress().getAddress();
            if (a.length != 4) return 0;
            return (a[0] & 0xff) | (a[1] & 0xff) << 8 | (a[2] & 0xff) << 16 | (a[3] & 0xff) << 24;
        } catch (Throwable t) {
            return 0;
        }
    }

    // AOSP: getDhcpInfo returns the current DHCP-supplied connection
    // info as a DhcpInfo (all fields are zero on macOS since we don't
    // expose underlying DHCP state).
    public DhcpInfo getDhcpInfo()             { return dhcp; }

    public List<ScanResult> getScanResults() {
        String[] r = com.macapk.loader.HostRadios.wifiScanResults();
        if (r == null) return Collections.emptyList();
        List<ScanResult> out = new java.util.ArrayList<>();
        long now = android.os.SystemClock.elapsedRealtime() * 1000L;
        for (int i = 0; i + 4 < r.length; i += 5) {
            ScanResult s = new ScanResult();
            s.SSID = r[i];
            s.BSSID = r[i + 1];
            s.capabilities = r[i + 2];
            s.level = parse(r[i + 3], -100);
            s.frequency = parse(r[i + 4], 0);
            s.timestamp = now;
            out.add(s);
        }
        return out;
    }

    /** AOSP: the results arrive later, announced by SCAN_RESULTS_AVAILABLE_ACTION
     *  (EXTRA_RESULTS_UPDATED). Windows finishes a scan within about four
     *  seconds, so the broadcast follows then. */
    public boolean startScan() {
        if (!com.macapk.loader.HostRadios.wifiStartScan()) return false;
        Thread t = new Thread(() -> {
            try {
                Thread.sleep(4000L);
                android.content.Context ctx = android.app.ActivityThread.currentApplication();
                if (ctx == null) return;
                android.content.Intent i = new android.content.Intent(SCAN_RESULTS_AVAILABLE_ACTION);
                i.putExtra(EXTRA_RESULTS_UPDATED, true);
                ctx.sendBroadcast(i);
            } catch (Throwable ignored) {
            }
        }, "wifi-scan-results");
        t.setDaemon(true);
        t.start();
        return true;
    }

    public static final String EXTRA_RESULTS_UPDATED = "resultsUpdated";
    public boolean  reconnect()               { return false; }
    public boolean  reassociate()             { return false; }
    public boolean  disconnect()              { return false; }
    public boolean  pingSupplicant()          { return false; }
    public boolean  saveConfiguration()       { return false; }
    public List<?>  getConfiguredNetworks()   { return Collections.emptyList(); }
    // AOSP: addNetwork(WifiConfiguration)/updateNetwork(WifiConfiguration) --
    // verified against android-emulator/android-sdk/platforms/android-30/android.jar
    // (`javap -p android.net.wifi.WifiManager`): both take the concrete
    // WifiConfiguration type, not Object. A guest javac-compiles a call site as
    // invokevirtual .../WifiManager.addNetwork(Landroid/net/wifi/WifiConfiguration;)I;
    // our previous `(Ljava/lang/Object;)I` descriptor does not match that call site
    // at all, so it threw NoSuchMethodError before this method could ever run (same
    // descriptor-mismatch bug class as the v1.0.406 NetworkRequest/ConnectivityManager
    // fixes). AOSP returns -1 ("network could not be added") on failure, matching the
    // "no supplicant here" answer this class already gives every other Wi-Fi control.
    public int      addNetwork(WifiConfiguration config) { return -1; }
    public int      updateNetwork(WifiConfiguration config) { return -1; }
    public boolean  removeNetwork(int id)     { return false; }
    public boolean  enableNetwork(int id, boolean disableOthers) { return false; }
    public boolean  disableNetwork(int id)    { return false; }
    public boolean  is5GHzBandSupported()     { return true; }
    public boolean  isP2pSupported()          { return false; }
    public boolean  isScanAlwaysAvailable()   { return false; }

    public WifiLock createWifiLock(int lockType, String tag) { return new WifiLock(); }
    public WifiLock createWifiLock(String tag)               { return new WifiLock(); }

    public MulticastLock createMulticastLock(String tag) { return new MulticastLock(); }

    public static class WifiLock {
        public void acquire() {}
        public void release() {}
        public boolean isHeld() { return false; }
        public void setReferenceCounted(boolean on) {}
        // AOSP: setWorkSource(android.os.WorkSource) -- verified against
        // android-emulator/android-sdk/platforms/android-30/android.jar
        // (`javap -p android.net.wifi.WifiManager$WifiLock`). Same
        // descriptor-mismatch bug class as addNetwork/updateNetwork above: a
        // guest's invokevirtual site carries the WorkSource descriptor and
        // would NoSuchMethodError against the old Object-typed overload.
        public void setWorkSource(WorkSource ws) {}
    }

    public static class MulticastLock {
        public void acquire() {}
        public void release() {}
        public boolean isHeld() { return false; }
        public void setReferenceCounted(boolean on) {}
    }

    public static int calculateSignalLevel(int rssi, int numLevels) {
        if (rssi <= -100) return 0;
        if (rssi >= -55) return numLevels - 1;
        float inputRange = (-55) - (-100);
        float outputRange = numLevels - 1;
        return (int) ((rssi - (-100)) * outputRange / inputRange);
    }
}

