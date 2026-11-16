#include "module.hpp"

#include <android/log.h>
#include <android/api-level.h>
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
#include "detach_request.hpp"
#include "zygisk.hpp"

#define ARR_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define STR_LEN(a) (ARR_LEN(a) - 1)
#define VENDING_PROC "com.android.vending"

static char* DETACH_TXT = nullptr;
static size_t DETACH_TXT_LEN = 0;
static size_t HEADERS_LEN = 0;
static uint32_t getApplicationEnabledSetting_code = 0;
static jmethodID parcelSize, parcelPosition, parcelSetPosition, parcelMarshall, parcelUnmarshall;
static jboolean (*transactNative_orig)(JNIEnv*, jobject, jint, jobject, jobject, jint);

// Hook the actual Java Binder entry point. A successful PLT commit does not
// establish that ReZygisk replaced any libbinder relocation (observed on A16).
// Parcel's Java API also avoids relying on its private C++ object layout.
static jboolean transactNative_hook(JNIEnv* env, jobject self, jint code, jobject data,
                                    jobject reply, jint flags) {
    if (uint32_t(code) == getApplicationEnabledSetting_code && data && !env->ExceptionCheck()) {
        const jint size = env->CallIntMethod(data, parcelSize);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return transactNative_orig(env, self, code, data, reply, flags); }
        const jint position = env->CallIntMethod(data, parcelPosition);
        if (!env->ExceptionCheck() && size > 0 && size <= 1024) {
            auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(data, parcelMarshall));
            if (!env->ExceptionCheck() && bytes) {
                uint8_t buffer[1024];
                const jsize count = env->GetArrayLength(bytes);
                if (count == size) {
                    env->GetByteArrayRegion(bytes, 0, count, reinterpret_cast<jbyte*>(buffer));
                    if (!env->ExceptionCheck()) {
                        auto package = detachedPackageInRequest(buffer, count, HEADERS_LEN,
                            reinterpret_cast<const uint8_t*>(DETACH_TXT), DETACH_TXT_LEN);
                        if (package) {
                            const jbyte zero[2] = {0, 0};
                            env->SetByteArrayRegion(bytes, package - buffer, 2, zero);
                            env->CallVoidMethod(data, parcelUnmarshall, bytes, 0, count);
                            if (!env->ExceptionCheck()) {
                                env->CallVoidMethod(data, parcelSetPosition, position);
                                LOGD("Detached enabled-state query (%u UTF-16 units)", unsigned((count - (package - buffer)) / 2));
                            }
                        }
                    }
                }
            }
            // Unrelated interfaces may have Binder objects and cannot marshal.
            // Their requests remain intact and are passed to the original JNI.
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (bytes) env->DeleteLocalRef(bytes);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    return transactNative_orig(env, self, code, data, reply, flags);
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
    DETACH_TXT_LEN = size;
    return (size_t)size;
}

static bool runPreSpecialize(const char* process, zygisk::Api* api) {
    if (strncmp(process, VENDING_PROC, STR_LEN(VENDING_PROC)) != 0) return false;
    __android_log_print(ANDROID_LOG_INFO, "zygisk-detach", "Preparing JNI hook for %s", process);

    int fd = api->connectCompanion();
    size_t detach_len = read_companion(fd);
    close(fd);
    if (detach_len == 0) return false;

    return true;
}

static bool installBinderHook(zygisk::Api* api, JNIEnv* env) {
    int sdk = android_get_device_api_level();
    if (sdk <= 0) {
        LOGD("ERROR android_get_device_api_level: %d", sdk);
        return false;
    }
    HEADERS_LEN = getBinderHeadersLen(sdk);

    getApplicationEnabledSetting_code = getStaticIntFieldJni(env, STUB("android/content/pm/IPackageManager"),
                                                             TRSCTN("getApplicationEnabledSetting"));
    if (getApplicationEnabledSetting_code == 0) return false;

    jclass parcel = env->FindClass("android/os/Parcel");
    if (!parcel) { env->ExceptionClear(); return false; }
    parcelSize = env->GetMethodID(parcel, "dataSize", "()I");
    parcelPosition = env->GetMethodID(parcel, "dataPosition", "()I");
    parcelSetPosition = env->GetMethodID(parcel, "setDataPosition", "(I)V");
    parcelMarshall = env->GetMethodID(parcel, "marshall", "()[B");
    parcelUnmarshall = env->GetMethodID(parcel, "unmarshall", "([BII)V");
    env->DeleteLocalRef(parcel);
    if (env->ExceptionCheck() || !parcelSize || !parcelPosition || !parcelSetPosition ||
        !parcelMarshall || !parcelUnmarshall) {
        env->ExceptionClear();
        LOGD("ERROR: Parcel JNI methods unavailable");
        return false;
    }
    JNINativeMethod methods[] = {{const_cast<char*>("transactNative"),
        const_cast<char*>("(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z"),
        reinterpret_cast<void*>(transactNative_hook)}};
    api->hookJniNativeMethods(env, "android/os/BinderProxy", methods, 1);
    transactNative_orig = reinterpret_cast<decltype(transactNative_orig)>(methods[0].fnPtr);
    if (!transactNative_orig || transactNative_orig == transactNative_hook) {
        __android_log_print(ANDROID_LOG_ERROR, "zygisk-detach", "BinderProxy JNI hook unavailable");
        return false;
    }
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
        doRunPost = runPreSpecialize(process, api);
        if (doRunPost) doRunPost = installBinderHook(api, env);

        if (!doRunPost) {
            cleanup(args);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!doRunPost) return;

        __android_log_print(ANDROID_LOG_INFO, "zygisk-detach", "Loaded %s (BinderProxy JNI hook)", process);
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

    bool doRunPost;
    const char* process;
};

static void companionHandler(int remote_fd) {
    companionSendFile("/data/adb/zygisk-detach/detach.bin", remote_fd);
}

REGISTER_ZYGISK_MODULE(ZygiskDetach)
REGISTER_ZYGISK_COMPANION(companionHandler)
