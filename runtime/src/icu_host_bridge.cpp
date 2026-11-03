// icu_host_bridge.cpp -- com.macapk.icu.HostIcu's natives: ICU number-skeleton
// formatting through the host's own ICU4C, /usr/lib/libicucore.A.dylib.
//
// WHY. android.icu.text.MeasureFormat (Hermes' Intl.NumberFormat {style:"unit"})
// needs CLDR unit patterns, which the JDK does not ship. macOS does: libicucore
// is ICU4C (78.1 on macOS 26) and exports its C API, so this is decision-ladder
// rung 1 -- the host supports it natively. The functions are resolved with
// dlsym at first use (no link-time dependency on a private Apple library, and a
// missing symbol degrades to an IllegalStateException instead of a dyld abort).
//
// C API used (unicode/unumberformatter.h, ICU 62+; unicode/utypes.h):
//   UNumberFormatter* unumf_openForSkeletonAndLocale(const UChar* skeleton,
//       int32_t skeletonLen, const char* locale, UErrorCode* ec);
//   UFormattedNumber* unumf_openResult(UErrorCode* ec);
//   void unumf_formatDouble(const UNumberFormatter*, double, UFormattedNumber*, UErrorCode*);
//   void unumf_formatDecimal(const UNumberFormatter*, const char* value, int32_t valueLen,
//       UFormattedNumber*, UErrorCode*);
//   int32_t unumf_resultToString(const UFormattedNumber*, UChar* buffer, int32_t cap, UErrorCode*);
//   void unumf_close(UNumberFormatter*);  void unumf_closeResult(UFormattedNumber*);
//   const char* u_errorName(UErrorCode);
// UErrorCode is an int32 enum: > 0 (U_ZERO_ERROR = 0) is a failure, < 0 a warning
// (U_USING_FALLBACK_WARNING -128, U_USING_DEFAULT_WARNING -127 are normal here).
// UChar is char16_t, which is jchar's representation.
#include <jni.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

using UErrorCode = int32_t;
using UNumberFormatter = void;
using UFormattedNumber = void;

struct IcuApi {
    UNumberFormatter* (*openForSkeletonAndLocale)(const char16_t*, int32_t, const char*, UErrorCode*) = nullptr;
    UFormattedNumber* (*openResult)(UErrorCode*) = nullptr;
    void (*formatDouble)(const UNumberFormatter*, double, UFormattedNumber*, UErrorCode*) = nullptr;
    void (*formatDecimal)(const UNumberFormatter*, const char*, int32_t, UFormattedNumber*, UErrorCode*) = nullptr;
    int32_t (*resultToString)(const UFormattedNumber*, char16_t*, int32_t, UErrorCode*) = nullptr;
    void (*close)(UNumberFormatter*) = nullptr;
    void (*closeResult)(UFormattedNumber*) = nullptr;
    const char* (*errorName)(UErrorCode) = nullptr;
    bool ok = false;
};

const IcuApi& Api() {
    static IcuApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        void* h = dlopen("/usr/lib/libicucore.A.dylib", RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            std::fprintf(stderr, "[E] icu-host: dlopen(libicucore) failed: %s\n", dlerror());
            return;
        }
#define MACAPK_ICU_SYM(field, name) api.field = reinterpret_cast<decltype(api.field)>(dlsym(h, name))
        MACAPK_ICU_SYM(openForSkeletonAndLocale, "unumf_openForSkeletonAndLocale");
        MACAPK_ICU_SYM(openResult, "unumf_openResult");
        MACAPK_ICU_SYM(formatDouble, "unumf_formatDouble");
        MACAPK_ICU_SYM(formatDecimal, "unumf_formatDecimal");
        MACAPK_ICU_SYM(resultToString, "unumf_resultToString");
        MACAPK_ICU_SYM(close, "unumf_close");
        MACAPK_ICU_SYM(closeResult, "unumf_closeResult");
        MACAPK_ICU_SYM(errorName, "u_errorName");
#undef MACAPK_ICU_SYM
        api.ok = api.openForSkeletonAndLocale && api.openResult && api.formatDouble &&
                 api.formatDecimal && api.resultToString && api.close && api.closeResult &&
                 api.errorName;
        if (!api.ok) std::fprintf(stderr, "[E] icu-host: libicucore lacks the unumf_* API\n");
    });
    return api;
}

void Throw(JNIEnv* env, const char* cls, const std::string& msg) {
    jclass c = env->FindClass(cls);
    if (c) env->ThrowNew(c, msg.c_str());
}

std::string Utf8(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

// One format call: skeleton + locale -> formatter, value -> string. Either a
// double or a decimal string (`dec` non-null) is formatted.
jstring Format(JNIEnv* env, jstring jskeleton, jstring jlocale, double value, const std::string* dec) {
    const IcuApi& api = Api();
    if (!api.ok) {
        Throw(env, "java/lang/IllegalStateException", "host ICU (libicucore unumf_*) unavailable");
        return nullptr;
    }
    if (!jskeleton) {
        Throw(env, "java/lang/NullPointerException", "skeleton");
        return nullptr;
    }
    const jsize n = env->GetStringLength(jskeleton);
    std::vector<char16_t> sk(static_cast<size_t>(n) + 1);
    env->GetStringRegion(jskeleton, 0, n, reinterpret_cast<jchar*>(sk.data()));
    const std::string locale = Utf8(env, jlocale);

    UErrorCode ec = 0;
    UNumberFormatter* f = api.openForSkeletonAndLocale(sk.data(), n, locale.c_str(), &ec);
    if (ec > 0 || !f) {
        if (f) api.close(f);
        Throw(env, "java/lang/IllegalArgumentException",
              std::string("unumf_openForSkeletonAndLocale: ") + api.errorName(ec));
        return nullptr;
    }
    UFormattedNumber* r = api.openResult(&ec);
    if (ec > 0 || !r) {
        api.close(f);
        if (r) api.closeResult(r);
        Throw(env, "java/lang/IllegalStateException", std::string("unumf_openResult: ") + api.errorName(ec));
        return nullptr;
    }
    if (dec) api.formatDecimal(f, dec->c_str(), static_cast<int32_t>(dec->size()), r, &ec);
    else api.formatDouble(f, value, r, &ec);
    jstring out = nullptr;
    if (ec <= 0) {
        char16_t buf[256];
        UErrorCode ec2 = 0;
        int32_t len = api.resultToString(r, buf, 256, &ec2);
        if (ec2 <= 0 && len >= 0 && len <= 256) {
            out = env->NewString(reinterpret_cast<const jchar*>(buf), len);
        } else if (len > 256) {      // U_BUFFER_OVERFLOW_ERROR: retry at the reported size
            std::vector<char16_t> big(static_cast<size_t>(len) + 1);
            ec2 = 0;
            len = api.resultToString(r, big.data(), len + 1, &ec2);
            if (ec2 <= 0) out = env->NewString(reinterpret_cast<const jchar*>(big.data()), len);
        }
        if (!out && !env->ExceptionCheck()) {
            Throw(env, "java/lang/IllegalStateException",
                  std::string("unumf_resultToString: ") + api.errorName(ec2));
        }
    } else {
        Throw(env, "java/lang/IllegalArgumentException", std::string("unumf_format: ") + api.errorName(ec));
    }
    api.closeResult(r);
    api.close(f);
    return out;
}

jstring JNICALL NativeFormatDouble(JNIEnv* env, jclass, jstring sk, jstring loc, jdouble v) {
    return Format(env, sk, loc, v, nullptr);
}

jstring JNICALL NativeFormatDecimal(JNIEnv* env, jclass, jstring sk, jstring loc, jstring dec) {
    if (!dec) {
        Throw(env, "java/lang/NullPointerException", "decimal");
        return nullptr;
    }
    const std::string d = Utf8(env, dec);
    return Format(env, sk, loc, 0.0, &d);
}

// Every native HostIcu declares, with its exact descriptor: RegisterNatives
// fails the WHOLE table on one mismatch.
const JNINativeMethod kIcuHostMethods[] = {
    {const_cast<char*>("nativeFormatDouble"),
     const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;D)Ljava/lang/String;"),
     reinterpret_cast<void*>(NativeFormatDouble)},
    {const_cast<char*>("nativeFormatDecimal"),
     const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
     reinterpret_cast<void*>(NativeFormatDecimal)},
};

}  // namespace

namespace macapk {
namespace icuhost {

// Registers com.macapk.icu.HostIcu's natives. SOFT-FAILS like the clipboard
// registration: a miss leaves HostIcu.formatSkeleton throwing
// UnsupportedOperationException (unit formatting unavailable); never aborts boot.
// Returns true when registered.
bool RegisterIcuHostMethods(JNIEnv* env) {
    jclass cls = env->FindClass("com/macapk/icu/HostIcu");
    if (!cls) {
        std::fprintf(stderr, "[E] icu-host: FindClass(com.macapk.icu.HostIcu) failed\n");
        env->ExceptionClear();
        return false;
    }
    bool ok = env->RegisterNatives(cls, kIcuHostMethods,
                                   sizeof(kIcuHostMethods) / sizeof(kIcuHostMethods[0])) == JNI_OK;
    if (!ok) {
        std::fprintf(stderr, "[E] icu-host: RegisterNatives(HostIcu) failed\n");
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    return ok;
}

}  // namespace icuhost
}  // namespace macapk

#ifdef MACAPK_ICU_HOST_STANDALONE
// Offline gate build (debug/icuintl_gate.sh): the same file compiled as a
// System.load()-able dylib, so the bare-JVM harness exercises these exact
// natives without the runtime.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) != JNI_OK) return JNI_ERR;
    return macapk::icuhost::RegisterIcuHostMethods(env) ? JNI_VERSION_1_8 : JNI_ERR;
}
#endif
