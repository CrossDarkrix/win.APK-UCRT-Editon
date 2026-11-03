// android.net.wifi.WifiInfo -- snapshot of the active Wi-Fi connection.
// WifiManager fills it from the host's radio (com.macapk.loader.HostRadios)
// when the host reports one; otherwise it is the steady stub it always was.
// The MAC stays AOSP's redacted constant: apps have not been given the
// device's own MAC since Android 6.
package android.net.wifi;

public class WifiInfo implements android.net.TransportInfo {
    /** AOSP WifiInfo sentinel constants. */
    public static final int LINK_SPEED_UNKNOWN = -1;
    public static final int INVALID_RSSI = -127;
    /** AOSP returns this for getSSID() when the SSID is unavailable. */
    public static final String UNKNOWN_SSID = "<unknown ssid>";

    private final String mSsid;      // raw, unquoted; null = unknown
    private final String mBssid;
    private final int mRssi, mFrequency, mLinkSpeed, mIpAddress;
    private final SupplicantState mState;

    /** The stub: a healthy connection with no identity, for a host that does
     *  not report its radio. */
    public WifiInfo() {
        this(null, "", -60, 2412, 100, 0, SupplicantState.COMPLETED);
    }

    WifiInfo(String ssid, String bssid, int rssi, int frequency, int linkSpeed, int ipAddress,
            SupplicantState state) {
        mSsid = ssid;
        mBssid = bssid;
        mRssi = rssi;
        mFrequency = frequency;
        mLinkSpeed = linkSpeed;
        mIpAddress = ipAddress;
        mState = state;
    }

    public String getMacAddress()  { return "02:00:00:00:00:00"; }
    public String getBSSID()       { return mBssid; }
    /** AOSP wraps a UTF-8 SSID in double quotes, and answers UNKNOWN_SSID when
     *  it is withheld (no location permission) or there is no connection. */
    public String getSSID()        { return mSsid == null ? UNKNOWN_SSID : "\"" + mSsid + "\""; }
    public int    getRssi()        { return mRssi; }
    public int    getLinkSpeed()   { return mLinkSpeed; }     // Mbps
    public int    getTxLinkSpeedMbps() { return mLinkSpeed; }
    public int    getRxLinkSpeedMbps() { return mLinkSpeed; }
    /** AOSP getFrequency(): the channel frequency in MHz. */
    public int    getFrequency()   { return mFrequency; }
    public int    getNetworkId()   { return -1; }
    /** AOSP getIpAddress() (deprecated): the IPv4 address as a little-endian
     *  int, 0 if unknown. */
    public int    getIpAddress()   { return mIpAddress; }
    public boolean getHiddenSSID() { return false; }
    /** AOSP getSupplicantState(): COMPLETED while associated. */
    public SupplicantState getSupplicantState() { return mState; }
}
