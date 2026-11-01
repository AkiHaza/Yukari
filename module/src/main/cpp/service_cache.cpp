#include "service_cache.h"
#include "service_match.h"

namespace {
class LocalFrame {
public:
    explicit LocalFrame(JNIEnv *env) : env_(env), ready_(env->PushLocalFrame(16) == 0) {}
    ~LocalFrame() { if (ready_) env_->PopLocalFrame(nullptr); }
    bool ready() const { return ready_; }
private:
    JNIEnv *env_;
    bool ready_;
};
}

bool clear_cache(JNIEnv *env) {
    // Preserve caller exceptions. Later exceptions belong to optional cleanup.
    if (!env || env->ExceptionCheck()) return false;
    LocalFrame frame(env);
    bool complete = false;
    do {
        if (!frame.ready()) break;
        jclass manager = env->FindClass("android/os/ServiceManager");
        if (!manager || env->ExceptionCheck()) break;
        jfieldID field = env->GetStaticFieldID(manager, "sCache", "Ljava/util/Map;");
        if (!field || env->ExceptionCheck()) break;
        jobject cache = env->GetStaticObjectField(manager, field);
        if (env->ExceptionCheck()) break;
        if (!cache) { complete = true; break; }
        jclass map = env->FindClass("java/util/Map");
        if (!map || env->ExceptionCheck()) break;
        jmethodID key_set = env->GetMethodID(map, "keySet", "()Ljava/util/Set;");
        if (!key_set || env->ExceptionCheck()) break;
        jmethodID remove = env->GetMethodID(map, "remove",
                                           "(Ljava/lang/Object;)Ljava/lang/Object;");
        if (!remove || env->ExceptionCheck()) break;
        jobject keys = env->CallObjectMethod(cache, key_set);
        if (!keys || env->ExceptionCheck()) break;
        jclass set = env->FindClass("java/util/Set");
        if (!set || env->ExceptionCheck()) break;
        jmethodID to_array = env->GetMethodID(set, "toArray", "()[Ljava/lang/Object;");
        if (!to_array || env->ExceptionCheck()) break;
        auto array = static_cast<jobjectArray>(env->CallObjectMethod(keys, to_array));
        if (!array || env->ExceptionCheck()) break;
        jclass string_class = env->FindClass("java/lang/String");
        if (!string_class || env->ExceptionCheck()) break;
        const jsize count = env->GetArrayLength(array);
        if (env->ExceptionCheck()) break;
        complete = true;
        for (jsize i = 0; i < count; ++i) {
            jobject key = env->GetObjectArrayElement(array, i);
            if (env->ExceptionCheck()) { complete = false; break; }
            if (!key) continue;
            const bool is_string = env->IsInstanceOf(key, string_class);
            if (env->ExceptionCheck()) { complete = false; break; }
            if (!is_string) { env->DeleteLocalRef(key); continue; }
            auto name = static_cast<jstring>(key);
            const char *chars = env->GetStringUTFChars(name, nullptr);
            if (!chars || env->ExceptionCheck()) {
                if (chars) env->ReleaseStringUTFChars(name, chars);
                complete = false;
                break;
            }
            const bool hidden = hide_service(chars);
            env->ReleaseStringUTFChars(name, chars);
            if (hidden) {
                jobject removed = env->CallObjectMethod(cache, remove, key);
                if (removed) env->DeleteLocalRef(removed);
            }
            env->DeleteLocalRef(key);
            if (env->ExceptionCheck()) { complete = false; break; }
        }
    } while (false);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return complete;
}
