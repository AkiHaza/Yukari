#include "service_cache.h"
#include "service_match.h"

#include <string>

void clear_cache(JNIEnv *env) {
    if (!env) return;

    jclass sm_class = env->FindClass("android/os/ServiceManager");
    if (!sm_class) {
        env->ExceptionClear();
        return;
    }

    jfieldID cache_field = env->GetStaticFieldID(sm_class, "sCache", "Ljava/util/Map;");
    if (!cache_field) {
        env->ExceptionClear();
        env->DeleteLocalRef(sm_class);
        return;
    }

    jobject cache = env->GetStaticObjectField(sm_class, cache_field);
    if (env->ExceptionCheck() || !cache) {
        env->ExceptionClear();
        env->DeleteLocalRef(sm_class);
        return;
    }

    jclass map_class = env->FindClass("java/util/Map");
    if (!map_class) {
        env->ExceptionClear();
        env->DeleteLocalRef(cache);
        env->DeleteLocalRef(sm_class);
        return;
    }
    jmethodID key_set = env->GetMethodID(map_class, "keySet", "()Ljava/util/Set;");
    jmethodID remove = env->GetMethodID(map_class, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;");
    if (env->ExceptionCheck() || !key_set || !remove) {
        env->ExceptionClear();
        env->DeleteLocalRef(map_class);
        env->DeleteLocalRef(cache);
        env->DeleteLocalRef(sm_class);
        return;
    }
    jobject keys_obj = env->CallObjectMethod(cache, key_set);
    if (env->ExceptionCheck() || !keys_obj) {
        env->ExceptionClear();
        env->DeleteLocalRef(map_class);
        env->DeleteLocalRef(cache);
        env->DeleteLocalRef(sm_class);
        return;
    }

    jclass set_class = env->FindClass("java/util/Set");
    if (!set_class) {
        env->ExceptionClear();
        env->DeleteLocalRef(keys_obj);
        env->DeleteLocalRef(map_class);
        env->DeleteLocalRef(cache);
        env->DeleteLocalRef(sm_class);
        return;
    }
    jmethodID to_array = env->GetMethodID(set_class, "toArray", "()[Ljava/lang/Object;");
    if (env->ExceptionCheck() || !to_array) {
        env->ExceptionClear();
        env->DeleteLocalRef(set_class);
        env->DeleteLocalRef(keys_obj);
        env->DeleteLocalRef(map_class);
        env->DeleteLocalRef(cache);
        env->DeleteLocalRef(sm_class);
        return;
    }
    auto keys = static_cast<jobjectArray>(env->CallObjectMethod(keys_obj, to_array));
    if (env->ExceptionCheck() || !keys) {
        env->ExceptionClear();
        env->DeleteLocalRef(set_class);
        env->DeleteLocalRef(keys_obj);
        env->DeleteLocalRef(map_class);
        env->DeleteLocalRef(cache);
        env->DeleteLocalRef(sm_class);
        return;
    }

    const jsize count = env->GetArrayLength(keys);
    for (jsize i = 0; i < count; ++i) {
        auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
        if (!key) continue;
        const char *raw = env->GetStringUTFChars(key, nullptr);
        if (raw) {
            try {
                if (hide_service(std::string(raw))) {
                    env->CallObjectMethod(cache, remove, key);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            } catch (...) {
                // A transient native allocation failure must not escape JNI
                // and turn optional cache cleanup into an app crash.
            }
            env->ReleaseStringUTFChars(key, raw);
        }
        env->DeleteLocalRef(key);
    }

    // A failed GetStringUTFChars (for example, transient OOM) leaves a
    // pending JNI exception.  Do not leak it into application startup.
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->DeleteLocalRef(keys);
    env->DeleteLocalRef(set_class);
    env->DeleteLocalRef(keys_obj);
    env->DeleteLocalRef(map_class);
    env->DeleteLocalRef(cache);
    env->DeleteLocalRef(sm_class);
}
