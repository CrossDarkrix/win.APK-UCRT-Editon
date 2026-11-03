// native_radios.cpp -- com.macapk.loader.HostRadios on Windows: the PC's Wi-Fi
// (Native Wifi API, wlanapi) and Bluetooth (bluetoothapis) for WifiManager and
// BluetoothAdapter.
//
// Only what Windows reports is passed on. Since Windows 11 24H2 a desktop
// program gets SSIDs, BSSIDs and scan results only with location access on
// (Settings > Privacy > Location), which is the same rule Android applies
// (ACCESS_FINE_LOCATION); without it the calls fail and the app sees what an
// Android app without that permission sees: "<unknown ssid>" and no networks.
//
// Bluetooth is the classic (BR/EDR) API: the radio, whether it is on, and the
// paired devices. Talking to devices (RFCOMM, GATT) is not offered.

#include "host_natives.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <bluetoothapis.h>
#include <wlanapi.h>

namespace winapk::host {

namespace {

// WifiManager.WIFI_STATE_*.
constexpr jint kWifiDisabled = 1, kWifiEnabled = 3;

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

jobjectArray to_java(JNIEnv* env, const std::vector<std::string>& v) {
    jclass string_class = env->FindClass("java/lang/String");
    if (!string_class) return nullptr;
    jobjectArray a = env->NewObjectArray(static_cast<jsize>(v.size()), string_class, nullptr);
    for (size_t i = 0; a && i < v.size(); ++i) {
        jstring s = env->NewStringUTF(v[i].c_str());
        env->SetObjectArrayElement(a, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(string_class);
    return a;
}

// ---- Wi-Fi ------------------------------------------------------------------

// One client handle for the process: opening one per call costs a service
// round trip, and the handle is safe to share between threads.
HANDLE wlan() {
    static HANDLE handle = [] {
        DWORD version = 0;
        HANDLE h = nullptr;
        return WlanOpenHandle(2, nullptr, &version, &h) == ERROR_SUCCESS ? h : nullptr;
    }();
    return handle;
}

// The interface apps mean by "the Wi-Fi": the first connected one, else the
// first one. False when the PC has no Wi-Fi adapter.
bool wifi_interface(GUID* guid, WLAN_INTERFACE_STATE* state) {
    HANDLE h = wlan();
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (!h || WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS) return false;
    bool found = false;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& info = list->InterfaceInfo[i];
        if (!found || info.isState == wlan_interface_state_connected) {
            *guid = info.InterfaceGuid;
            *state = info.isState;
            found = true;
            if (info.isState == wlan_interface_state_connected) break;
        }
    }
    WlanFreeMemory(list);
    return found;
}

std::string ssid_text(const DOT11_SSID& s) {
    return std::string(reinterpret_cast<const char*>(s.ucSSID), std::min<ULONG>(s.uSSIDLength, DOT11_SSID_MAX_LENGTH));
}

std::string mac_text(const unsigned char* b) {
    char out[18];
    std::snprintf(out, sizeof(out), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return out;
}

// Android's capability string for a network's security, as wpa_supplicant
// formats it ("[WPA2-PSK-CCMP][ESS]"); apps test it with contains("WPA") etc.
std::string capabilities(const WLAN_AVAILABLE_NETWORK* n) {
    std::string caps;
    if (n && n->bSecurityEnabled) {
        const char* cipher = n->dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_TKIP ? "TKIP" : "CCMP";
        switch (static_cast<int>(n->dot11DefaultAuthAlgorithm)) {
        case DOT11_AUTH_ALGO_RSNA_PSK: caps = std::string("[WPA2-PSK-") + cipher + "]"; break;
        case DOT11_AUTH_ALGO_RSNA: caps = std::string("[WPA2-EAP-") + cipher + "]"; break;
        case DOT11_AUTH_ALGO_WPA_PSK: caps = std::string("[WPA-PSK-") + cipher + "]"; break;
        case DOT11_AUTH_ALGO_WPA: caps = std::string("[WPA-EAP-") + cipher + "]"; break;
        case 9:   // DOT11_AUTH_ALGO_WPA3_SAE, newer than some SDK headers
        case 10:  // DOT11_AUTH_ALGO_OWE
            caps = std::string("[RSN-SAE-") + cipher + "]";
            break;
        case 8:  // DOT11_AUTH_ALGO_WPA3 (enterprise)
            caps = std::string("[RSN-EAP-SUITE-B-192-") + cipher + "]";
            break;
        default: caps = "[WEP]"; break;
        }
    }
    return caps + "[ESS]";
}

jint JNICALL Radio_wifiState(JNIEnv*, jclass) {
    GUID guid;
    WLAN_INTERFACE_STATE state;
    if (!wifi_interface(&guid, &state)) return -1;
    // The radio switch (airplane mode, the Wi-Fi toggle) turns the PHYs off;
    // the interface itself stays listed.
    DWORD size = 0;
    PWLAN_RADIO_STATE radio = nullptr;
    jint result = kWifiEnabled;
    if (WlanQueryInterface(wlan(), &guid, wlan_intf_opcode_radio_state, nullptr, &size,
                           reinterpret_cast<void**>(&radio), nullptr) == ERROR_SUCCESS &&
        radio) {
        bool on = false;
        for (DWORD i = 0; i < radio->dwNumberOfPhys; ++i) {
            const WLAN_PHY_RADIO_STATE& p = radio->PhyRadioState[i];
            on = on || (p.dot11SoftwareRadioState == dot11_radio_state_on &&
                        p.dot11HardwareRadioState == dot11_radio_state_on);
        }
        if (!on) result = kWifiDisabled;
        WlanFreeMemory(radio);
    }
    return result;
}

// {ssid, bssid, rssi, frequency, link Mbps}; ssid and bssid are empty when
// Windows withholds them (no location access).
jobjectArray JNICALL Radio_wifiConnection(JNIEnv* env, jclass) {
    GUID guid;
    WLAN_INTERFACE_STATE state;
    if (!wifi_interface(&guid, &state) || state != wlan_interface_state_connected) return nullptr;
    std::vector<std::string> out = {"", "", "", "", ""};
    DWORD size = 0;
    PWLAN_CONNECTION_ATTRIBUTES conn = nullptr;
    if (WlanQueryInterface(wlan(), &guid, wlan_intf_opcode_current_connection, nullptr, &size,
                           reinterpret_cast<void**>(&conn), nullptr) == ERROR_SUCCESS &&
        conn) {
        const WLAN_ASSOCIATION_ATTRIBUTES& a = conn->wlanAssociationAttributes;
        out[0] = ssid_text(a.dot11Ssid);
        out[1] = mac_text(a.dot11Bssid);
        // Signal quality 0..100 is linear in dBm between -100 and -50 (the
        // wlanapi documentation's own mapping).
        out[2] = std::to_string(static_cast<int>(a.wlanSignalQuality) / 2 - 100);
        out[4] = std::to_string(a.ulRxRate / 1000);
        // The frequency is only in the BSS list: the entry of this BSSID.
        PWLAN_BSS_LIST bss = nullptr;
        DOT11_SSID ssid = a.dot11Ssid;  // the API takes it non-const
        if (WlanGetNetworkBssList(wlan(), &guid, &ssid, a.dot11BssType, TRUE, nullptr, &bss) ==
                ERROR_SUCCESS &&
            bss) {
            for (DWORD i = 0; i < bss->dwNumberOfItems; ++i) {
                const WLAN_BSS_ENTRY& e = bss->wlanBssEntries[i];
                if (memcmp(e.dot11Bssid, a.dot11Bssid, sizeof(DOT11_MAC_ADDRESS)) == 0) {
                    out[2] = std::to_string(e.lRssi);
                    out[3] = std::to_string(e.ulChCenterFrequency / 1000);
                    break;
                }
            }
            WlanFreeMemory(bss);
        }
        WlanFreeMemory(conn);
    }
    return to_java(env, out);
}

jobjectArray JNICALL Radio_wifiScanResults(JNIEnv* env, jclass) {
    GUID guid;
    WLAN_INTERFACE_STATE state;
    if (!wifi_interface(&guid, &state)) return nullptr;
    PWLAN_BSS_LIST bss = nullptr;
    if (WlanGetNetworkBssList(wlan(), &guid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bss) != ERROR_SUCCESS ||
        !bss) {
        return nullptr;
    }
    // Security is per network (SSID) in the available-network list.
    PWLAN_AVAILABLE_NETWORK_LIST nets = nullptr;
    WlanGetAvailableNetworkList(wlan(), &guid, 0, nullptr, &nets);
    std::vector<std::string> out;
    for (DWORD i = 0; i < bss->dwNumberOfItems; ++i) {
        const WLAN_BSS_ENTRY& e = bss->wlanBssEntries[i];
        const WLAN_AVAILABLE_NETWORK* net = nullptr;
        for (DWORD k = 0; nets && k < nets->dwNumberOfItems; ++k) {
            const DOT11_SSID& s = nets->Network[k].dot11Ssid;
            if (s.uSSIDLength == e.dot11Ssid.uSSIDLength &&
                memcmp(s.ucSSID, e.dot11Ssid.ucSSID, s.uSSIDLength) == 0) {
                net = &nets->Network[k];
                break;
            }
        }
        out.push_back(ssid_text(e.dot11Ssid));
        out.push_back(mac_text(e.dot11Bssid));
        out.push_back(capabilities(net));
        out.push_back(std::to_string(e.lRssi));
        out.push_back(std::to_string(e.ulChCenterFrequency / 1000));
    }
    if (nets) WlanFreeMemory(nets);
    WlanFreeMemory(bss);
    return to_java(env, out);
}

jboolean JNICALL Radio_wifiStartScan(JNIEnv*, jclass) {
    GUID guid;
    WLAN_INTERFACE_STATE state;
    if (!wifi_interface(&guid, &state)) return JNI_FALSE;
    return WlanScan(wlan(), &guid, nullptr, nullptr, nullptr) == ERROR_SUCCESS ? JNI_TRUE : JNI_FALSE;
}

// ---- Bluetooth ----------------------------------------------------------------

// Android writes the address most significant byte first; Windows stores it
// least significant first.
std::string bt_address(const BLUETOOTH_ADDRESS& a) {
    char out[18];
    std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", a.rgBytes[5], a.rgBytes[4], a.rgBytes[3],
                  a.rgBytes[2], a.rgBytes[1], a.rgBytes[0]);
    return out;
}

jobjectArray JNICALL Radio_bluetoothRadio(JNIEnv* env, jclass) {
    BLUETOOTH_FIND_RADIO_PARAMS params{};
    params.dwSize = sizeof(params);
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) return nullptr;
    BluetoothFindRadioClose(find);
    BLUETOOTH_RADIO_INFO info{};
    info.dwSize = sizeof(info);
    std::vector<std::string> out;
    if (BluetoothGetRadioInfo(radio, &info) == ERROR_SUCCESS) {
        // A radio switched off in Settings stays listed but stops accepting
        // connections, which is what "on" means to an app.
        const bool on = BluetoothIsConnectable(radio) || BluetoothIsDiscoverable(radio);
        out = {utf8(info.szName), bt_address(info.address), on ? "1" : "0"};
    }
    CloseHandle(radio);
    return out.empty() ? nullptr : to_java(env, out);
}

jobjectArray JNICALL Radio_bluetoothBonded(JNIEnv* env, jclass) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
    search.dwSize = sizeof(search);
    search.fReturnAuthenticated = TRUE;
    search.fReturnRemembered = TRUE;
    search.fReturnConnected = TRUE;
    search.fIssueInquiry = FALSE;  // the paired list only, no radio inquiry
    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    std::vector<std::string> out;
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, &info);
    if (!find) return to_java(env, out);
    do {
        if (info.fAuthenticated || info.fRemembered) {
            out.push_back(utf8(info.szName));
            out.push_back(bt_address(info.Address));
            out.push_back(std::to_string(info.ulClassofDevice));
        }
        info = {};
        info.dwSize = sizeof(info);
    } while (BluetoothFindNextDevice(find, &info));
    BluetoothFindDeviceClose(find);
    return to_java(env, out);
}

constexpr const char* kCls = "com.macapk.loader.HostRadios";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeWifiState", "()I", Radio_wifiState),
    WINAPK_HOST_NATIVE(kCls, "nativeWifiConnection", "()[Ljava/lang/String;", Radio_wifiConnection),
    WINAPK_HOST_NATIVE(kCls, "nativeWifiScanResults", "()[Ljava/lang/String;", Radio_wifiScanResults),
    WINAPK_HOST_NATIVE(kCls, "nativeWifiStartScan", "()Z", Radio_wifiStartScan),
    WINAPK_HOST_NATIVE(kCls, "nativeBluetoothRadio", "()[Ljava/lang/String;", Radio_bluetoothRadio),
    WINAPK_HOST_NATIVE(kCls, "nativeBluetoothBonded", "()[Ljava/lang/String;", Radio_bluetoothBonded),
};

}  // namespace

HostMethods native_radios_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
