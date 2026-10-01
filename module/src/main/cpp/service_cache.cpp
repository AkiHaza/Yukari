#include "service_cache.h"
#include "service_match.h"

#include <string>

void clear_cache(JNIEnv *env) {
    if (!env || env->ExceptionCheck()) return;

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
    jmethodID remove = nullptr;
    if (!env->ExceptionCheck() && key_set) {
        remove = env->GetMethodID(map_class, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;");
    }
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
    jclass string_class = nullptr;
    if (!env->ExceptionCheck()) string_class = env->FindClass("java/lang/String");
    for (jsize index = 0; string_class && !env->ExceptionCheck() && index < count; ++index) {
        jobject key = env->GetObjectArrayElement(keys, index);
        if (env->ExceptionCheck()) break;
        if (!key) continue;
        if (!env->IsInstanceOf(key, string_class)) {
            env->DeleteLocalRef(key);
            continue;
        }
        auto string_key = static_cast<jstring>(key);
        const char *raw = env->GetStringUTFChars(string_key, nullptr);
        if (!raw || env->ExceptionCheck()) {
            if (raw) env->ReleaseStringUTFChars(string_key, raw);
            env->DeleteLocalRef(key);
            break;
        }
        if (raw) {
            try {
                if (hide_service(std::string(raw))) {
                    jobject removed = env->CallObjectMethod(cache, remove, key);
                    if (removed) env->DeleteLocalRef(removed);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            } catch (...) {
                // A transient native allocation failure must not escape JNI
                // and turn optional cache cleanup into an app crash.
            }
            env->ReleaseStringUTFChars(string_key, raw);
        }
        env->DeleteLocalRef(key);
    }

    // A failed GetStringUTFChars (for example, transient OOM) leaves a
    // pending JNI exception.  Do not leak it into application startup.
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (string_class) env->DeleteLocalRef(string_class);
    env->DeleteLocalRef(keys);
    env->DeleteLocalRef(set_class);
    env->DeleteLocalRef(keys_obj);
    env->DeleteLocalRef(map_class);
    env->DeleteLocalRef(cache);
    env->DeleteLocalRef(sm_class);
}
