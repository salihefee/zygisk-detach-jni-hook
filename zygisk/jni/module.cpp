#include "module.hpp"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "binder.hpp"
#include "zygisk.hpp"

#define ARR_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define STR_LEN(a) (ARR_LEN(a) - 1)
#define VENDING_PROC "com.android.vending"

#define PM_DESC u"android.content.pm.IPackageManager"

static char* DETACH_TXT = nullptr;
static size_t HEADERS_LEN = 0;
static uint32_t getApplicationEnabledSetting_code = 0;
static jfieldID parcelNativePtr = nullptr;

static inline void detach(PParcel* pparcel, uint32_t code) {
    auto parcel = FakeParcel(pparcel->data);
    if (pparcel->data_size < HEADERS_LEN + 4) return;
    parcel.skip(HEADERS_LEN);  // header

    auto descLen = parcel.readInt32();
    auto desc = parcel.readString16(descLen);

    if (code != getApplicationEnabledSetting_code ||
        STR_LEN(PM_DESC) != descLen ||
        memcmp(desc, PM_DESC, descLen * sizeof(char16_t)) != 0) {
        return;
    }
    parcel.skip(2);

    auto pkgLen = parcel.readInt32();
    auto pkg = parcel.readString16(pkgLen);

    auto pkgLenB = (uint8_t)(pkgLen * 2 - 1);
    size_t i = 0;
    uint8_t dlen;
    while ((dlen = DETACH_TXT[i])) {
        const char* dptr = DETACH_TXT + i + sizeof(dlen);
        i += sizeof(dlen) + dlen;
        if (dlen != pkgLenB) continue;
        if (memcmp(dptr, pkg, dlen) == 0) {
            *pkg = 0;
            return;
        }
    }
}

jboolean (*transact_orig)(JNIEnv*, jobject, jint, jobject, jobject, jint);

jboolean transact_hook(JNIEnv* env, jobject self, jint code, jobject data, jobject reply, jint flags) {
    if (data && uint32_t(code) == getApplicationEnabledSetting_code && !env->ExceptionCheck()) {
        auto parcel = reinterpret_cast<PParcel*>(uintptr_t(env->GetLongField(data, parcelNativePtr)));
        if (parcel) detach(parcel, code);
    }
    return transact_orig(env, self, code, data, reply, flags);
}

static size_t read_companion(int fd) {
    off_t size;
    if (read(fd, &size, sizeof(size)) < 0) {
        LOGD("ERROR read: %s", strerror(errno));
        return 0;
    }
    if (size <= 0) {
        LOGD("ERROR read_companion: size=%ld", size);
        return 0;
    }
    DETACH_TXT = (char*)malloc(size + 1);

    if (!readFullFromFd(fd, DETACH_TXT, size)) return 0;

    DETACH_TXT[size] = 0;
    return (size_t)size;
}

static bool runPreSpecialize(const char* process, zygisk::Api* api) {
    if (strncmp(process, VENDING_PROC, STR_LEN(VENDING_PROC)) != 0) return false;

    int fd = api->connectCompanion();
    size_t detach_len = read_companion(fd);
    close(fd);
    if (detach_len == 0) return false;

    return true;
}

static bool installBinderHook(const char* process, zygisk::Api* api, JNIEnv* env) {
    int sdk = android_get_device_api_level();
    if (sdk <= 0) {
        LOGD("ERROR android_get_device_api_level: %d", sdk);
        return false;
    }
    HEADERS_LEN = getBinderHeadersLen(sdk);

    getApplicationEnabledSetting_code = getStaticIntFieldJni(env, STUB("android/content/pm/IPackageManager"),
                                                             TRSCTN("getApplicationEnabledSetting"));
    if (getApplicationEnabledSetting_code == 0) return false;

    jclass parcelClass = env->FindClass("android/os/Parcel");
    if (parcelClass) {
        parcelNativePtr = env->GetFieldID(parcelClass, "mNativePtr", "J");
        env->DeleteLocalRef(parcelClass);
    }
    if (!parcelNativePtr) {
        env->ExceptionClear();
        LOGD("ERROR: Could not get Parcel.mNativePtr");
        return false;
    }

    // A successful PLT commit can leave libbinder's relocation unchanged on
    // ReZygisk. Hook the registered Binder JNI entry point instead.
    JNINativeMethod methods[] = {{const_cast<char*>("transactNative"),
        const_cast<char*>("(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z"),
        reinterpret_cast<void*>(transact_hook)}};
    api->hookJniNativeMethods(env, "android/os/BinderProxy", methods, 1);
    transact_orig = reinterpret_cast<decltype(transact_orig)>(methods[0].fnPtr);
    if (!transact_orig || transact_orig == transact_hook) {
        LOGD("ERROR: Could not hook BinderProxy.transactNative");
        return false;
    }

    LOGD("Loaded %s", process);
    return true;
}

class ZygiskDetach : public zygisk::ModuleBase {
   public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        process = env->GetStringUTFChars(args->nice_name, nullptr);
        bool enabled = runPreSpecialize(process, api);
        if (enabled) enabled = installBinderHook(process, api, env);

        if (!enabled) {
            cleanup(args);
        }
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs* args) override {
        (void)args;
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void cleanup(const zygisk::AppSpecializeArgs* args) {
        free(DETACH_TXT);
        env->ReleaseStringUTFChars(args->nice_name, process);
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

   private:
    zygisk::Api* api;
    JNIEnv* env;

    const char* process;
};

static void companionHandler(int remote_fd) {
    companionSendFile("/data/adb/zygisk-detach/detach.bin", remote_fd);
}

REGISTER_ZYGISK_MODULE(ZygiskDetach)
REGISTER_ZYGISK_COMPANION(companionHandler)
