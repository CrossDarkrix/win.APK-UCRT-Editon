// native_audio.cpp -- com.macapk.loader.NativeAudio on Windows: AudioTrack
// streams, SoundPool and MediaPlayer over the mixer in audio_engine.cpp.
// Handles and ids are table keys, never pointers, so a stale one from Java is
// a no-op rather than a crash.

#include "host_natives.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include "audio_engine.h"

namespace winapk::host {

namespace {

namespace au = winapk::audio;

std::mutex g_lock;
jlong g_next_handle = 1;
jint g_next_id = 1;
std::map<jlong, std::shared_ptr<au::Stream>> g_streams;
std::map<jint, std::shared_ptr<const au::Clip>> g_sounds;
std::map<jint, std::shared_ptr<au::Voice>> g_voices;
std::vector<jint> g_auto_paused;

struct Player {
    std::shared_ptr<const au::Clip> clip;
    std::shared_ptr<au::Voice> voice;
    float left = 1, right = 1;
    bool looping = false;
    bool started = false; // start() was requested and stop() has not been called
    bool paused = false;
    double position = 0;  // seconds; where the next start begins
};
std::map<jlong, Player> g_players;

std::shared_ptr<au::Stream> stream(jlong h) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_streams.find(h);
    return it == g_streams.end() ? nullptr : it->second;
}
std::shared_ptr<au::Voice> voice(jint id) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_voices.find(id);
    return it == g_voices.end() ? nullptr : it->second;
}

std::string utf8_from_wide(const wchar_t* value) {
    if (!value || !*value) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};

    // n includes the trailing NUL. The previous implementation allocated n - 1
    // bytes but asked WideCharToMultiByte() to write n bytes. That one-byte
    // overwrite corrupted endpoint records before they reached AudioManager,
    // so device-picker UIs could end up with only their synthetic "Automatic"
    // row. Allocate the complete Windows result, then remove the terminator.
    std::string out(static_cast<size_t>(n), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                             out.data(), n, nullptr, nullptr);
    if (written != n) return {};
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

jobjectArray JNICALL Audio_getOutputDevices(JNIEnv* env, jclass) {
    jclass string_class = env->FindClass("java/lang/String");
    if (!string_class) return nullptr;
    std::vector<std::string> records;
    HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(ci);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* default_device = nullptr;
    LPWSTR default_id_w = nullptr;
    std::string default_id;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator)))) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device)) &&
            SUCCEEDED(default_device->GetId(&default_id_w))) {
            default_id = utf8_from_wide(default_id_w);
        }
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            if (SUCCEEDED(collection->GetCount(&count))) {
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* device = nullptr;
                    IPropertyStore* props = nullptr;
                    IAudioClient* client = nullptr;
                    LPWSTR id_w = nullptr;
                    PROPVARIANT name_value;
                    PropVariantInit(&name_value);
                    WAVEFORMATEX* format = nullptr;
                    if (SUCCEEDED(collection->Item(i, &device)) &&
                        SUCCEEDED(device->GetId(&id_w))) {
                        std::string id = utf8_from_wide(id_w);
                        std::string name = id;
                        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
                            SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name_value)) &&
                            name_value.vt == VT_LPWSTR && name_value.pwszVal) {
                            name = utf8_from_wide(name_value.pwszVal);
                        }
                        int rate = 0, channels = 0;
                        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                     reinterpret_cast<void**>(&client))) &&
                            SUCCEEDED(client->GetMixFormat(&format)) && format) {
                            rate = static_cast<int>(format->nSamplesPerSec);
                            channels = static_cast<int>(format->nChannels);
                        }
                        records.push_back(id + "\x1f" + name + "\x1f" +
                                          (id == default_id ? "1" : "0") + "\x1f" +
                                          std::to_string(rate) + "\x1f" + std::to_string(channels));
                    }
                    if (format) CoTaskMemFree(format);
                    if (client) client->Release();
                    PropVariantClear(&name_value);
                    if (props) props->Release();
                    if (id_w) CoTaskMemFree(id_w);
                    if (device) device->Release();
                }
            }
        }
    }
    if (default_id_w) CoTaskMemFree(default_id_w);
    if (default_device) default_device->Release();
    if (collection) collection->Release();
    if (enumerator) enumerator->Release();
    if (uninit) CoUninitialize();
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(records.size()), string_class, nullptr);
    if (!result) return nullptr;
    for (jsize i = 0; i < static_cast<jsize>(records.size()); ++i) {
        jstring item = env->NewStringUTF(records[static_cast<size_t>(i)].c_str());
        if (!item) return result;
        env->SetObjectArrayElement(result, i, item);
        env->DeleteLocalRef(item);
    }
    return result;
}

void JNICALL Audio_setSessionVolume(JNIEnv*, jclass, jfloat value) {
    au::set_session_volume(value);
}
jfloat JNICALL Audio_getSessionVolume(JNIEnv*, jclass) {
    return au::session_volume();
}
void JNICALL Audio_setSessionMute(JNIEnv*, jclass, jboolean muted) {
    au::set_session_mute(muted == JNI_TRUE);
}
jboolean JNICALL Audio_getSessionMute(JNIEnv*, jclass) {
    return au::session_mute() ? JNI_TRUE : JNI_FALSE;
}

jboolean JNICALL Audio_setOutputDevice(JNIEnv* env, jclass, jstring endpoint) {
    std::string id;
    if (endpoint) {
        const char* chars = env->GetStringUTFChars(endpoint, nullptr);
        if (!chars) return JNI_FALSE;
        id = chars;
        env->ReleaseStringUTFChars(endpoint, chars);
        if (id.empty()) return JNI_FALSE;
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninit = SUCCEEDED(ci);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        int wn = MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, nullptr, 0);
        std::wstring wid(wn > 0 ? static_cast<size_t>(wn) : 0, L'\0');
        if (wn > 1) {
            MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, wid.data(), wn);
            wid.resize(static_cast<size_t>(wn - 1));
        }
        bool valid = !wid.empty() &&
            SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->GetDevice(wid.c_str(), &device));
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (uninit) CoUninitialize();
        if (!valid) return JNI_FALSE;
    }
    return au::set_output_endpoint(id) ? JNI_TRUE : JNI_FALSE;
}

// ---- AudioTrack ---------------------------------------------------------------------
std::map<jlong, int> g_encodings;

jlong JNICALL Audio_create(JNIEnv*, jclass, jint rate, jint channels, jint encoding, jint buffer_frames) {
    auto s = au::stream_create(rate, channels, encoding, buffer_frames);
    if (!s) return 0;
    std::lock_guard<std::mutex> hold(g_lock);
    const jlong h = g_next_handle++;
    g_streams[h] = s;
    g_encodings[h] = encoding;
    return h;
}

template <typename Array, typename Element>
jint write_array(JNIEnv* env, jlong h, Array data, jint offset, jint size, jboolean blocking, int encoding) {
    auto s = stream(h);
    if (!s || !data || offset < 0 || size <= 0 || offset + size > env->GetArrayLength(data)) return 0;
    // Copied out first: a blocking write must not hold the array pinned.
    std::vector<Element> copy(static_cast<size_t>(size));
    if constexpr (sizeof(Element) == 1) env->GetByteArrayRegion(data, offset, size, copy.data());
    else if constexpr (sizeof(Element) == 2) env->GetShortArrayRegion(data, offset, size, copy.data());
    else env->GetFloatArrayRegion(data, offset, size, copy.data());
    return static_cast<jint>(au::stream_write(s.get(), copy.data(), copy.size(), encoding, blocking == JNI_TRUE));
}
jint JNICALL Audio_writeBytes(JNIEnv* env, jclass, jlong h, jbyteArray d, jint off, jint size, jboolean blocking) {
    int encoding;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        encoding = g_encodings.count(h) ? g_encodings[h] : 3;
    }
    // A byte array carries 8-bit samples, or 16-bit / float ones as raw bytes.
    if (encoding == 3) return write_array<jbyteArray, jbyte>(env, h, d, off, size, blocking, 3);
    const int width = encoding == 4 ? 4 : 2;
    auto s = stream(h);
    if (!s || !d || off < 0 || size <= 0 || off + size > env->GetArrayLength(d)) return 0;
    std::vector<jbyte> copy(static_cast<size_t>(size));
    env->GetByteArrayRegion(d, off, size, copy.data());
    const size_t samples = au::stream_write(s.get(), copy.data(), copy.size() / width, encoding, blocking == JNI_TRUE);
    return static_cast<jint>(samples * width);
}
jint JNICALL Audio_writeShorts(JNIEnv* env, jclass, jlong h, jshortArray d, jint off, jint size, jboolean blocking) {
    return write_array<jshortArray, jshort>(env, h, d, off, size, blocking, 2);
}
jint JNICALL Audio_writeFloats(JNIEnv* env, jclass, jlong h, jfloatArray d, jint off, jint size, jboolean blocking) {
    return write_array<jfloatArray, jfloat>(env, h, d, off, size, blocking, 4);
}
void JNICALL Audio_play(JNIEnv*, jclass, jlong h) {
    if (auto s = stream(h)) au::stream_play(s.get());
}
void JNICALL Audio_pause(JNIEnv*, jclass, jlong h) {
    if (auto s = stream(h)) au::stream_pause(s.get());
}
void JNICALL Audio_stop(JNIEnv*, jclass, jlong h) {
    if (auto s = stream(h)) au::stream_stop(s.get());
}
void JNICALL Audio_flush(JNIEnv*, jclass, jlong h) {
    if (auto s = stream(h)) au::stream_flush(s.get());
}
void JNICALL Audio_release(JNIEnv*, jclass, jlong h) {
    std::shared_ptr<au::Stream> s;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_streams.find(h);
        if (it == g_streams.end()) return;
        s = it->second;
        g_streams.erase(it);
        g_encodings.erase(h);
    }
    au::stream_release(s);
}
void JNICALL Audio_setVolume(JNIEnv*, jclass, jlong h, jfloat l, jfloat r) {
    if (auto s = stream(h)) au::stream_set_volume(s.get(), l, r);
}
jint JNICALL Audio_getPlayState(JNIEnv*, jclass, jlong h) {
    auto s = stream(h);
    return s ? au::stream_state(s.get()) : au::kStopped;
}
jlong JNICALL Audio_getPosition(JNIEnv*, jclass, jlong h) {
    auto s = stream(h);
    return s ? au::stream_position(s.get()) : 0;
}
jboolean JNICALL Audio_getTimestamp(JNIEnv* env, jclass, jlong h, jlongArray out) {
    auto s = stream(h);
    int64_t frames, nanos;
    if (!s || !out || env->GetArrayLength(out) < 2 || !au::stream_timestamp(s.get(), &frames, &nanos)) return JNI_FALSE;
    const jlong v[2] = {frames, nanos};
    env->SetLongArrayRegion(out, 0, 2, v);
    return JNI_TRUE;
}
// AudioTrack's status codes: SUCCESS 0, ERROR_BAD_VALUE -2, ERROR_INVALID_OPERATION -3.
jint JNICALL Audio_setPlaybackRate(JNIEnv*, jclass, jlong h, jint hz) {
    auto s = stream(h);
    if (!s) return -3;
    return au::stream_set_rate(s.get(), hz) ? 0 : -2;
}
jint JNICALL Audio_getPlaybackRate(JNIEnv*, jclass, jlong h) {
    auto s = stream(h);
    return s ? au::stream_rate(s.get()) : 0;
}
jint JNICALL Audio_setPlaybackParams(JNIEnv*, jclass, jlong h, jfloat speed, jfloat pitch) {
    auto s = stream(h);
    if (!s) return -3;
    if (!(speed > 0 && speed <= 8 && pitch > 0 && pitch <= 8)) return -2;
    au::stream_set_params(s.get(), speed, pitch);
    return 0;
}
void JNICALL Audio_getPlaybackParams(JNIEnv* env, jclass, jlong h, jfloatArray out) {
    auto s = stream(h);
    if (!s || !out || env->GetArrayLength(out) < 2) return;
    float v[2];
    au::stream_params(s.get(), &v[0], &v[1]);
    env->SetFloatArrayRegion(out, 0, 2, v);
}

// ---- SoundPool --------------------------------------------------------------------------
std::shared_ptr<const au::Clip> decode_array(JNIEnv* env, jbyteArray data, jint len) {
    if (!data || len <= 0 || len > env->GetArrayLength(data)) return nullptr;
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(bytes.data()));
    return au::decode(bytes.data(), bytes.size());
}
jint JNICALL Sp_load(JNIEnv* env, jclass, jbyteArray data, jint len) {
    auto clip = decode_array(env, data, len);
    if (!clip) return 0;
    std::lock_guard<std::mutex> hold(g_lock);
    const jint id = g_next_id++;
    g_sounds[id] = clip;
    return id;
}
jint JNICALL Sp_play(JNIEnv*, jclass, jint sound, jfloat l, jfloat r, jint loop, jfloat rate) {
    std::shared_ptr<const au::Clip> clip;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_sounds.find(sound);
        if (it == g_sounds.end()) return 0;
        clip = it->second;
    }
    auto v = au::voice_start(clip, l, r, loop, rate > 0 ? rate : 1.0f, false);
    std::lock_guard<std::mutex> hold(g_lock);
    // Finished voices are dropped from the table as new ones start.
    for (auto it = g_voices.begin(); it != g_voices.end();) {
        it = au::voice_finished(it->second.get()) ? g_voices.erase(it) : std::next(it);
    }
    const jint id = g_next_id++;
    g_voices[id] = v;
    return id;
}
void JNICALL Sp_stop(JNIEnv*, jclass, jint id) {
    if (auto v = voice(id)) au::voice_stop(v.get());
}
void JNICALL Sp_pause(JNIEnv*, jclass, jint id) {
    if (auto v = voice(id)) au::voice_set_paused(v.get(), true);
}
void JNICALL Sp_resume(JNIEnv*, jclass, jint id) {
    if (auto v = voice(id)) au::voice_set_paused(v.get(), false);
}
void JNICALL Sp_setVolume(JNIEnv*, jclass, jint id, jfloat l, jfloat r) {
    if (auto v = voice(id)) au::voice_set_volume(v.get(), l, r);
}
void JNICALL Sp_setLoop(JNIEnv*, jclass, jint id, jint loop) {
    if (auto v = voice(id)) au::voice_set_loop(v.get(), loop);
}
void JNICALL Sp_setRate(JNIEnv*, jclass, jint id, jfloat rate) {
    if (auto v = voice(id)) au::voice_set_rate(v.get(), rate);
}
jboolean JNICALL Sp_unload(JNIEnv*, jclass, jint sound) {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_sounds.erase(sound) ? JNI_TRUE : JNI_FALSE;  // playing voices keep their clip
}
void JNICALL Sp_autoPause(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> hold(g_lock);
    for (auto& [id, v] : g_voices) {
        if (au::voice_playing(v.get())) {
            au::voice_set_paused(v.get(), true);
            g_auto_paused.push_back(id);
        }
    }
}
void JNICALL Sp_autoResume(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> hold(g_lock);
    for (jint id : g_auto_paused) {
        auto it = g_voices.find(id);
        if (it != g_voices.end()) au::voice_set_paused(it->second.get(), false);
    }
    g_auto_paused.clear();
}

// ---- MediaPlayer ------------------------------------------------------------------------
jlong JNICALL Mp_create(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> hold(g_lock);
    const jlong h = g_next_handle++;
    g_players[h] = Player();
    return h;
}
template <typename F>
void with_player(jlong h, F f) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_players.find(h);
    if (it != g_players.end()) f(it->second);
}
jboolean JNICALL Mp_setSource(JNIEnv* env, jclass, jlong h, jbyteArray data, jint len) {
    auto clip = decode_array(env, data, len);
    if (!clip) return JNI_FALSE;
    bool found = false;
    with_player(h, [&](Player& p) {
        if (p.voice) au::voice_stop(p.voice.get());
        p.voice.reset();
        p.clip = clip;
        p.position = 0;
        p.started = false;
        p.paused = false;
        found = true;
    });
    return found ? JNI_TRUE : JNI_FALSE;
}
void JNICALL Mp_start(JNIEnv*, jclass, jlong h) {
    with_player(h, [](Player& p) {
        if (!p.clip) return;
        p.started = true;
        p.paused = false;
        if (p.voice && !au::voice_finished(p.voice.get())) {
            au::voice_set_loop(p.voice.get(), p.looping ? -1 : 0);
            au::voice_set_paused(p.voice.get(), false);
            return;
        }
        p.position = 0;
        p.voice = au::voice_start(p.clip, p.left, p.right, p.looping ? -1 : 0, 1.0f, false);
    });
}
void JNICALL Mp_pause(JNIEnv*, jclass, jlong h) {
    with_player(h, [](Player& p) {
        if (!p.voice) return;
        p.position = au::voice_position(p.voice.get());
        p.paused = true;
        au::voice_set_paused(p.voice.get(), true);
    });
}
void JNICALL Mp_stop(JNIEnv*, jclass, jlong h) {
    with_player(h, [](Player& p) {
        if (p.voice) au::voice_stop(p.voice.get());
        p.voice.reset();
        p.position = 0;
        p.started = false;
        p.paused = false;
    });
}
void JNICALL Mp_seek(JNIEnv*, jclass, jlong h, jint ms) {
    with_player(h, [ms](Player& p) {
        p.position = (std::max)(jint{0}, ms) / 1000.0;
        if (p.voice && !au::voice_finished(p.voice.get())) au::voice_seek(p.voice.get(), p.position);
    });
}
void JNICALL Mp_setLooping(JNIEnv*, jclass, jlong h, jboolean loop) {
    with_player(h, [loop](Player& p) {
        p.looping = loop == JNI_TRUE;
        if (!p.voice) return;
        if (p.looping && p.started && au::voice_finished(p.voice.get())) {
            p.position = 0;
            p.voice = au::voice_start(p.clip, p.left, p.right, -1, 1.0f, p.paused);
        } else {
            au::voice_set_loop(p.voice.get(), p.looping ? -1 : 0);
        }
    });
}
void JNICALL Mp_setVolume(JNIEnv*, jclass, jlong h, jfloat l, jfloat r) {
    with_player(h, [l, r](Player& p) {
        p.left = l;
        p.right = r;
        if (p.voice) au::voice_set_volume(p.voice.get(), l, r);
    });
}
jint JNICALL Mp_duration(JNIEnv*, jclass, jlong h) {
    jint ms = -1;
    with_player(h, [&](Player& p) {
        if (p.clip && p.clip->rate) ms = static_cast<jint>(p.clip->frames() * 1000 / p.clip->rate);
    });
    return ms;
}
jint JNICALL Mp_position(JNIEnv*, jclass, jlong h) {
    jint ms = 0;
    with_player(h, [&](Player& p) {
        if (!p.clip) return;
        if (p.voice && au::voice_finished(p.voice.get())) {
            ms = static_cast<jint>(p.clip->frames() * 1000 / p.clip->rate);
        } else {
            ms = static_cast<jint>((p.voice ? au::voice_position(p.voice.get()) : p.position) * 1000);
        }
    });
    return ms;
}
jboolean JNICALL Mp_isPlaying(JNIEnv*, jclass, jlong h) {
    bool playing = false;
    with_player(h, [&](Player& p) { playing = p.voice && au::voice_playing(p.voice.get()); });
    return playing ? JNI_TRUE : JNI_FALSE;
}
void JNICALL Mp_release(JNIEnv*, jclass, jlong h) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_players.find(h);
    if (it == g_players.end()) return;
    if (it->second.voice) au::voice_stop(it->second.voice.get());
    g_players.erase(it);
}

constexpr const char* kCls = "com.macapk.loader.NativeAudio";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeCreate", "(IIII)J", Audio_create),
    WINAPK_HOST_NATIVE(kCls, "nativeWriteBytes", "(J[BIIZ)I", Audio_writeBytes),
    WINAPK_HOST_NATIVE(kCls, "nativeWriteShorts", "(J[SIIZ)I", Audio_writeShorts),
    WINAPK_HOST_NATIVE(kCls, "nativeWriteFloats", "(J[FIIZ)I", Audio_writeFloats),
    WINAPK_HOST_NATIVE(kCls, "nativePlay", "(J)V", Audio_play),
    WINAPK_HOST_NATIVE(kCls, "nativePause", "(J)V", Audio_pause),
    WINAPK_HOST_NATIVE(kCls, "nativeStop", "(J)V", Audio_stop),
    WINAPK_HOST_NATIVE(kCls, "nativeFlush", "(J)V", Audio_flush),
    WINAPK_HOST_NATIVE(kCls, "nativeRelease", "(J)V", Audio_release),
    WINAPK_HOST_NATIVE(kCls, "nativeSetVolume", "(JFF)V", Audio_setVolume),
    WINAPK_HOST_NATIVE(kCls, "nativeGetPlayState", "(J)I", Audio_getPlayState),
    WINAPK_HOST_NATIVE(kCls, "nativeGetPlaybackHeadPosition", "(J)J", Audio_getPosition),
    WINAPK_HOST_NATIVE(kCls, "nativeGetTimestamp", "(J[J)Z", Audio_getTimestamp),
    WINAPK_HOST_NATIVE(kCls, "nativeSetPlaybackRate", "(JI)I", Audio_setPlaybackRate),
    WINAPK_HOST_NATIVE(kCls, "nativeGetPlaybackRate", "(J)I", Audio_getPlaybackRate),
    WINAPK_HOST_NATIVE(kCls, "nativeSetPlaybackParams", "(JFF)I", Audio_setPlaybackParams),
    WINAPK_HOST_NATIVE(kCls, "nativeGetPlaybackParams", "(J[F)V", Audio_getPlaybackParams),
    WINAPK_HOST_NATIVE(kCls, "nativeSpLoad", "([BI)I", Sp_load),
    WINAPK_HOST_NATIVE(kCls, "nativeSpPlay", "(IFFIF)I", Sp_play),
    WINAPK_HOST_NATIVE(kCls, "nativeSpStop", "(I)V", Sp_stop),
    WINAPK_HOST_NATIVE(kCls, "nativeSpPause", "(I)V", Sp_pause),
    WINAPK_HOST_NATIVE(kCls, "nativeSpResume", "(I)V", Sp_resume),
    WINAPK_HOST_NATIVE(kCls, "nativeSpSetVolume", "(IFF)V", Sp_setVolume),
    WINAPK_HOST_NATIVE(kCls, "nativeSpSetLoop", "(II)V", Sp_setLoop),
    WINAPK_HOST_NATIVE(kCls, "nativeSpSetRate", "(IF)V", Sp_setRate),
    WINAPK_HOST_NATIVE(kCls, "nativeSpUnload", "(I)Z", Sp_unload),
    WINAPK_HOST_NATIVE(kCls, "nativeSpAutoPause", "()V", Sp_autoPause),
    WINAPK_HOST_NATIVE(kCls, "nativeSpAutoResume", "()V", Sp_autoResume),
    WINAPK_HOST_NATIVE(kCls, "nativeMpCreate", "()J", Mp_create),
    WINAPK_HOST_NATIVE(kCls, "nativeMpSetSource", "(J[BI)Z", Mp_setSource),
    WINAPK_HOST_NATIVE(kCls, "nativeMpStart", "(J)V", Mp_start),
    WINAPK_HOST_NATIVE(kCls, "nativeMpPause", "(J)V", Mp_pause),
    WINAPK_HOST_NATIVE(kCls, "nativeMpStop", "(J)V", Mp_stop),
    WINAPK_HOST_NATIVE(kCls, "nativeMpSeek", "(JI)V", Mp_seek),
    WINAPK_HOST_NATIVE(kCls, "nativeMpSetLooping", "(JZ)V", Mp_setLooping),
    WINAPK_HOST_NATIVE(kCls, "nativeMpSetVolume", "(JFF)V", Mp_setVolume),
    WINAPK_HOST_NATIVE(kCls, "nativeMpDuration", "(J)I", Mp_duration),
    WINAPK_HOST_NATIVE(kCls, "nativeMpPosition", "(J)I", Mp_position),
    WINAPK_HOST_NATIVE(kCls, "nativeMpIsPlaying", "(J)Z", Mp_isPlaying),
    WINAPK_HOST_NATIVE(kCls, "nativeMpRelease", "(J)V", Mp_release),
    WINAPK_HOST_NATIVE(kCls, "nativeGetAudioOutputDevices", "()[Ljava/lang/String;", Audio_getOutputDevices),
    WINAPK_HOST_NATIVE(kCls, "nativeSetAudioOutputDevice", "(Ljava/lang/String;)Z", Audio_setOutputDevice),
    WINAPK_HOST_NATIVE(kCls, "nativeSetSessionVolume", "(F)V", Audio_setSessionVolume),
    WINAPK_HOST_NATIVE(kCls, "nativeGetSessionVolume", "()F", Audio_getSessionVolume),
    WINAPK_HOST_NATIVE(kCls, "nativeSetSessionMute", "(Z)V", Audio_setSessionMute),
    WINAPK_HOST_NATIVE(kCls, "nativeGetSessionMute", "()Z", Audio_getSessionMute),
};

}  // namespace

HostMethods native_audio_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
