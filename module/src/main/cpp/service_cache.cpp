#include "service_cache.h"
#include "service_match.h"

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
        return;
    }

    jobject cache = env->GetStaticObjectField(sm_class, cache_field);
    if (!cache) return;

    jclass map_class = env->FindClass("java/util/Map");
    jmethodID key_set = env->GetMethodID(map_class, "keySet", "()Ljava/util/Set;");
    jmethodID remove = env->GetMethodID(map_class, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;");
    jobject keys_obj = env->CallObjectMethod(cache, key_set);
    if (!keys_obj) return;

    jclass set_class = env->FindClass("java/util/Set");
    jmethodID to_array = env->GetMethodID(set_class, "toArray", "()[Ljava/lang/Object;");
    auto keys = static_cast<jobjectArray>(env->CallObjectMethod(keys_obj, to_array));
    if (!keys) return;

    const jsize count = env->GetArrayLength(keys);
    for (jsize i = 0; i < count; ++i) {
        auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
        if (!key) continue;
        const char *raw = env->GetStringUTFChars(key, nullptr);
        if (raw) {
            if (hide_service(raw)) {
                env->CallObjectMethod(cache, remove, key);
            }
            env->ReleaseStringUTFChars(key, raw);
        }
        env->DeleteLocalRef(key);
    }
}
