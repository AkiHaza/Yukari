#include "feature_hook.h"
#include "feature_policy.h"
#include "logger.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <string_view>

namespace {
constexpr jint kMaxParcelSize = 256 * 1024;
constexpr std::string_view kPackageInterface = "android.content.pm.IPackageManager";
constexpr jint kOneWay = 1;

struct ParcelMethods {
    jclass cls = nullptr;
    jfieldID native_ptr = nullptr;
    jmethodID size = nullptr;
    jmethodID position = nullptr;
    jmethodID set_position = nullptr;
    jmethodID read_int = nullptr;
    jmethodID read_string = nullptr;
    jmethodID write_string = nullptr;
    jmethodID obtain = nullptr;
    jmethodID append = nullptr;
    jmethodID recycle = nullptr;
    jmethodID native_size = nullptr;
} g_parcel;

using TransactFn = jboolean (*)(JNIEnv *, jobject, jint, jobject, jobject, jint);
using ReadStringFn = jstring (*)(JNIEnv *, jclass, jlong);
using LifecycleFn = void (*)(JNIEnv *, jclass, jlong);
using PositionFn = void (*)(JNIEnv *, jclass, jlong, jint);
using CriticalPositionFn = void (*)(jlong, jint);
using UnmarshallFn = void (*)(JNIEnv *, jclass, jlong, jbyteArray, jint, jint);
using AppendFn = void (*)(JNIEnv *, jclass, jlong, jlong, jint, jint);

TransactFn g_transact = nullptr;
ReadStringFn g_read_string = nullptr;
ReadStringFn g_read_string8 = nullptr;
ReadStringFn g_read_string16 = nullptr;
LifecycleFn g_free_buffer = nullptr;
LifecycleFn g_destroy = nullptr;
PositionFn g_set_position = nullptr;
CriticalPositionFn g_set_position_critical = nullptr;
PositionFn g_set_size = nullptr;
UnmarshallFn g_unmarshall = nullptr;
AppendFn g_append = nullptr;
jint g_has_feature_code = 0;
jint g_list_features_code = 0;
FeatureHookStatus g_status;
bool g_installed = false;

yukari::features::ReplyRegistry<> g_replies;
pthread_mutex_t g_reply_mutex = PTHREAD_MUTEX_INITIALIZER;

class RegistryLock {
public:
    RegistryLock() : locked_(pthread_mutex_lock(&g_reply_mutex) == 0) {}
    ~RegistryLock() { if (locked_) pthread_mutex_unlock(&g_reply_mutex); }
    explicit operator bool() const { return locked_; }
private:
    bool locked_;
};

void forget_reply(jlong pointer) {
    RegistryLock lock;
    if (lock) g_replies.forget(static_cast<std::uintptr_t>(pointer));
}

void clear_probe_exception(JNIEnv *env) {
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool equals_ascii(JNIEnv *env, jstring value, std::string_view expected) {
    if (!value) return false;
    const jsize length = env->GetStringLength(value);
    if (env->ExceptionCheck() || length != static_cast<jsize>(expected.size()) || length > 64) {
        return false;
    }
    jchar chars[64];
    env->GetStringRegion(value, 0, length, chars);
    if (env->ExceptionCheck()) return false;
    for (jsize i = 0; i < length; ++i) {
        if (chars[i] != static_cast<jchar>(expected[static_cast<std::size_t>(i)])) return false;
    }
    return true;
}

bool hidden_string(JNIEnv *env, jstring value) {
    if (!value) return false;
    const jsize length = env->GetStringLength(value);
    if (env->ExceptionCheck() || length < 18 || length > 32) return false;
    jchar chars[32];
    env->GetStringRegion(value, 0, length, chars);
    if (env->ExceptionCheck()) return false;
    return yukari::features::hidden(std::u16string_view(
        reinterpret_cast<const char16_t *>(chars), static_cast<std::size_t>(length)));
}

jstring placeholder(JNIEnv *env, jstring original) {
    const jsize length = env->GetStringLength(original);
    if (env->ExceptionCheck() || length <= 0 || length > 32) return nullptr;
    jchar replacement[32];
    for (jsize i = 0; i < length; ++i) replacement[i] = u'_';
    return env->NewString(replacement, length);
}

// Optional probes must neither consume the caller's Parcel nor leave a JNI
// exception behind. This helper is used only before the real transaction.
class Cursor {
public:
    Cursor(JNIEnv *env, jobject parcel) : env_(env), parcel_(parcel) {
        saved_ = env_->CallIntMethod(parcel_, g_parcel.position);
        valid_ = !env_->ExceptionCheck() && saved_ >= 0;
        clear_probe_exception(env_);
    }
    ~Cursor() {
        clear_probe_exception(env_);
        if (valid_) env_->CallVoidMethod(parcel_, g_parcel.set_position, saved_);
        clear_probe_exception(env_);
    }
    bool valid() const { return valid_; }
    jint saved() const { return saved_; }
private:
    JNIEnv *env_;
    jobject parcel_;
    jint saved_ = 0;
    bool valid_ = false;
};

// Probe only the known Binder header boundaries. A matching transaction number
// is insufficient: every Binder interface has its own independent code space.
jint argument_position(JNIEnv *env, jobject data) {
    if (!data) return -1;
    Cursor cursor(env, data);
    if (!cursor.valid()) return -1;
    const jint size = env->CallIntMethod(data, g_parcel.size);
    if (env->ExceptionCheck() || size < 4 || size > kMaxParcelSize) return -1;
    for (const jint offset : {0, 4, 8, 12, 16}) {
        if (offset > size - 4) break;
        env->CallVoidMethod(data, g_parcel.set_position, offset);
        const jint length = env->ExceptionCheck() ? -1 : env->CallIntMethod(data, g_parcel.read_int);
        if (env->ExceptionCheck()) {
            clear_probe_exception(env);
            continue;
        }
        if (length != static_cast<jint>(kPackageInterface.size())) continue;
        env->CallVoidMethod(data, g_parcel.set_position, offset);
        auto token = static_cast<jstring>(env->CallObjectMethod(data, g_parcel.read_string));
        const bool match = !env->ExceptionCheck() && equals_ascii(env, token, kPackageInterface);
        const jint position = env->ExceptionCheck() ? -1 : env->CallIntMethod(data, g_parcel.position);
        if (token) env->DeleteLocalRef(token);
        clear_probe_exception(env);
        if (match && position >= 0 && position <= size) return position;
    }
    return -1;
}

// Copies only a confirmed, hidden feature query. The input bytes, size and
// cursor belong to the application and are never changed by this operation.
jobject private_feature_query(JNIEnv *env, jobject data, jint position) {
    Cursor cursor(env, data);
    if (!cursor.valid()) return nullptr;
    const jint size = env->CallIntMethod(data, g_parcel.size);
    if (env->ExceptionCheck() || position < 0 || position > size - 4) return nullptr;
    env->CallVoidMethod(data, g_parcel.set_position, position);
    const jint length = env->ExceptionCheck() ? -1 : env->CallIntMethod(data, g_parcel.read_int);
    if (env->ExceptionCheck() || length < 18 || length > 32) return nullptr;
    env->CallVoidMethod(data, g_parcel.set_position, position);
    auto name = static_cast<jstring>(env->CallObjectMethod(data, g_parcel.read_string));
    const bool hidden = !env->ExceptionCheck() && hidden_string(env, name);
    const jint end = env->ExceptionCheck() ? -1 : env->CallIntMethod(data, g_parcel.position);
    if (!hidden || env->ExceptionCheck() || end < position || end > size) {
        if (name) env->DeleteLocalRef(name);
        return nullptr;
    }
    jstring replacement = placeholder(env, name);
    env->DeleteLocalRef(name);
    if (!replacement || env->ExceptionCheck()) {
        if (replacement) env->DeleteLocalRef(replacement);
        return nullptr;
    }
    jobject copy = env->CallStaticObjectMethod(g_parcel.cls, g_parcel.obtain);
    if (copy && !env->ExceptionCheck()) {
        env->CallVoidMethod(copy, g_parcel.append, data, 0, size);
        if (!env->ExceptionCheck()) env->CallVoidMethod(copy, g_parcel.set_position, position);
        if (!env->ExceptionCheck()) env->CallVoidMethod(copy, g_parcel.write_string, replacement);
        const jint copy_end = env->ExceptionCheck() ? -1 : env->CallIntMethod(copy, g_parcel.position);
        const jint copy_size = env->ExceptionCheck() ? -1 : env->CallIntMethod(copy, g_parcel.size);
        if (copy_end != end || copy_size != size) {
            clear_probe_exception(env);
            env->CallVoidMethod(copy, g_parcel.recycle);
            env->DeleteLocalRef(copy);
            copy = nullptr;
        } else {
            env->CallVoidMethod(copy, g_parcel.set_position, cursor.saved());
        }
    }
    env->DeleteLocalRef(replacement);
    if (env->ExceptionCheck()) {
        clear_probe_exception(env);
        if (copy) {
            env->CallVoidMethod(copy, g_parcel.recycle);
            clear_probe_exception(env);
            env->DeleteLocalRef(copy);
        }
        return nullptr;
    }
    return copy;
}

void recycle_preserving_exception(JNIEnv *env, jobject copy) {
    if (!copy) return;
    jthrowable pending = env->ExceptionOccurred();
    if (pending) env->ExceptionClear();
    env->CallVoidMethod(copy, g_parcel.recycle);
    clear_probe_exception(env);
    env->DeleteLocalRef(copy);
    if (pending) {
        env->Throw(pending);
        env->DeleteLocalRef(pending);
    }
}

jlong native_pointer(JNIEnv *env, jobject parcel) {
    if (!parcel) return 0;
    const jlong pointer = env->GetLongField(parcel, g_parcel.native_ptr);
    if (env->ExceptionCheck()) {
        clear_probe_exception(env);
        return 0;
    }
    return pointer;
}

// Returns the validated reply size, restoring its cursor before the caller
// registers it. A remote exception or unknown reply header stays untouched.
jint reply_size(JNIEnv *env, jobject reply) {
    if (!reply) return 0;
    Cursor cursor(env, reply);
    if (!cursor.valid()) return 0;
    const jint size = env->CallIntMethod(reply, g_parcel.size);
    if (env->ExceptionCheck() || size < 8 || size > kMaxParcelSize) return 0;
    env->CallVoidMethod(reply, g_parcel.set_position, 0);
    const jint exception = env->ExceptionCheck() ? -1 : env->CallIntMethod(reply, g_parcel.read_int);
    return !env->ExceptionCheck() && exception == 0 ? size : 0;
}

jboolean hook_transact(JNIEnv *env, jobject self, jint code, jobject data, jobject reply, jint flags) {
    if (!g_transact) return JNI_FALSE;
    if (!env || env->ExceptionCheck()) return g_transact(env, self, code, data, reply, flags);
    if (reply) forget_reply(native_pointer(env, reply));
    const bool synchronous = (flags & kOneWay) == 0;
    const bool query = synchronous && g_status.query && code == g_has_feature_code;
    const bool enumeration = synchronous && g_status.enumeration && code == g_list_features_code;
    const jint argument = (query || enumeration) ? argument_position(env, data) : -1;
    jobject copy = query && argument >= 0 ? private_feature_query(env, data, argument) : nullptr;
    clear_probe_exception(env);
    const jboolean result = g_transact(env, self, code, copy ? copy : data, reply, flags);
    recycle_preserving_exception(env, copy);
    if (result == JNI_TRUE && !env->ExceptionCheck() && enumeration && argument >= 0) {
        const jint size = reply_size(env, reply);
        if (size > 0) {
            const jlong pointer = native_pointer(env, reply);
            RegistryLock lock;
            if (lock) g_replies.mark(static_cast<std::uintptr_t>(pointer), static_cast<std::size_t>(size));
        }
    }
    return result;
}

jstring scoped_string(JNIEnv *env, jlong pointer, ReadStringFn original, jclass cls) {
    if (!original) return nullptr;
    bool scoped = false;
    if (g_status.enumeration && !env->ExceptionCheck()) {
        const jint size = env->CallStaticIntMethod(g_parcel.cls, g_parcel.native_size, pointer);
        if (!env->ExceptionCheck() && size > 0) {
            RegistryLock lock;
            if (lock) scoped = g_replies.contains(static_cast<std::uintptr_t>(pointer), static_cast<std::size_t>(size));
        } else {
            clear_probe_exception(env);
            forget_reply(pointer);
        }
    }
    jstring result = original(env, cls, pointer);
    if (!scoped || !result || env->ExceptionCheck()) return result;
    if (!hidden_string(env, result)) {
        clear_probe_exception(env);
        return result;
    }
    jstring replacement = placeholder(env, result);
    if (!replacement || env->ExceptionCheck()) {
        clear_probe_exception(env);
        if (replacement) env->DeleteLocalRef(replacement);
        return result;
    }
    env->DeleteLocalRef(result);
    return replacement;
}

jstring hook_read_string(JNIEnv *env, jclass cls, jlong pointer) {
    return scoped_string(env, pointer, g_read_string, cls);
}
jstring hook_read_string8(JNIEnv *env, jclass cls, jlong pointer) {
    return scoped_string(env, pointer, g_read_string8, cls);
}
jstring hook_read_string16(JNIEnv *env, jclass cls, jlong pointer) {
    return scoped_string(env, pointer, g_read_string16, cls);
}
void hook_free_buffer(JNIEnv *env, jclass cls, jlong pointer) {
    forget_reply(pointer);
    if (g_free_buffer) g_free_buffer(env, cls, pointer);
}
void hook_destroy(JNIEnv *env, jclass cls, jlong pointer) {
    forget_reply(pointer);
    if (g_destroy) g_destroy(env, cls, pointer);
}
void hook_set_position(JNIEnv *env, jclass cls, jlong pointer, jint position) {
    forget_reply(pointer);
    if (g_set_position) g_set_position(env, cls, pointer, position);
}
void hook_set_position_critical(jlong pointer, jint position) {
    forget_reply(pointer);
    if (g_set_position_critical) g_set_position_critical(pointer, position);
}
void hook_set_size(JNIEnv *env, jclass cls, jlong pointer, jint size) {
    forget_reply(pointer);
    if (g_set_size) g_set_size(env, cls, pointer, size);
}
void hook_unmarshall(JNIEnv *env, jclass cls, jlong pointer, jbyteArray bytes, jint offset, jint length) {
    forget_reply(pointer);
    if (g_unmarshall) g_unmarshall(env, cls, pointer, bytes, offset, length);
}
void hook_append(JNIEnv *env, jclass cls, jlong pointer, jlong other, jint offset, jint length) {
    forget_reply(pointer);
    if (g_append) g_append(env, cls, pointer, other, offset, length);
}

jmethodID method(JNIEnv *env, const char *name, const char *signature, bool is_static = false) {
    jmethodID id = is_static ? env->GetStaticMethodID(g_parcel.cls, name, signature)
                            : env->GetMethodID(g_parcel.cls, name, signature);
    clear_probe_exception(env);
    return id;
}

jint transaction_code(JNIEnv *env, jclass stub, const char *field_name) {
    const jfieldID field = env->GetStaticFieldID(stub, field_name, "I");
    const jint result = field && !env->ExceptionCheck() ? env->GetStaticIntField(stub, field) : 0;
    clear_probe_exception(env);
    return result > 0 ? result : 0;
}

// CriticalNative has a different native ABI (no JNIEnv or jclass). Inspect the
// runtime annotation instead of guessing from an Android/OEM version number.
bool position_abi(JNIEnv *env, bool &critical) {
    jclass annotation = env->FindClass("dalvik/annotation/optimization/CriticalNative");
    if (!annotation) {
        clear_probe_exception(env);
        return false; // An unavailable ABI probe must never select a guess.
    }
    jclass class_type = env->FindClass("java/lang/Class");
    if (!class_type || env->ExceptionCheck()) return false;
    jclass method_type = env->FindClass("java/lang/reflect/Method");
    if (!method_type || env->ExceptionCheck()) return false;
    const jmethodID methods_id = env->GetMethodID(class_type, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    if (!methods_id || env->ExceptionCheck()) return false;
    const jmethodID name_id = env->GetMethodID(method_type, "getName", "()Ljava/lang/String;");
    if (!name_id || env->ExceptionCheck()) return false;
    const jmethodID annotation_id = env->GetMethodID(method_type, "isAnnotationPresent", "(Ljava/lang/Class;)Z");
    if (!annotation_id || env->ExceptionCheck()) return false;
    auto methods = static_cast<jobjectArray>(env->CallObjectMethod(g_parcel.cls, methods_id));
    if (!methods || env->ExceptionCheck()) return false;
    const jsize count = env->GetArrayLength(methods);
    for (jsize i = 0; i < count && !env->ExceptionCheck(); ++i) {
        jobject reflected = env->GetObjectArrayElement(methods, i);
        if (!reflected || env->ExceptionCheck()) return false;
        auto name = static_cast<jstring>(env->CallObjectMethod(reflected, name_id));
        const bool found = !env->ExceptionCheck() && equals_ascii(env, name, "nativeSetDataPosition");
        if (name) env->DeleteLocalRef(name);
        if (found) {
            critical = env->CallBooleanMethod(reflected, annotation_id, annotation) == JNI_TRUE;
            env->DeleteLocalRef(reflected);
            return !env->ExceptionCheck();
        }
        if (reflected) env->DeleteLocalRef(reflected);
    }
    return false;
}

template <typename Fn>
bool hook_native(JNIEnv *env, zygisk::Api *api, const char *cls, const char *name,
                 const char *signature, Fn replacement, Fn &original, bool is_static = true) {
    jclass type = env->FindClass(cls);
    if (!type || env->ExceptionCheck()) {
        clear_probe_exception(env);
        return false;
    }
    const jmethodID id = is_static ? env->GetStaticMethodID(type, name, signature)
                                  : env->GetMethodID(type, name, signature);
    env->DeleteLocalRef(type);
    if (!id || env->ExceptionCheck()) {
        clear_probe_exception(env);
        return false;
    }
    JNINativeMethod native{const_cast<char *>(name), const_cast<char *>(signature),
                           reinterpret_cast<void *>(replacement)};
    api->hookJniNativeMethods(env, cls, &native, 1);
    clear_probe_exception(env);
    if (!native.fnPtr || native.fnPtr == reinterpret_cast<void *>(replacement)) return false;
    original = reinterpret_cast<Fn>(native.fnPtr);
    return true;
}

bool init_methods(JNIEnv *env) {
    jclass local = env->FindClass("android/os/Parcel");
    if (!local || env->ExceptionCheck()) return false;
    g_parcel.cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_parcel.cls || env->ExceptionCheck()) return false;
    g_parcel.native_ptr = env->GetFieldID(g_parcel.cls, "mNativePtr", "J");
    clear_probe_exception(env);
    g_parcel.size = method(env, "dataSize", "()I");
    g_parcel.position = method(env, "dataPosition", "()I");
    g_parcel.set_position = method(env, "setDataPosition", "(I)V");
    g_parcel.read_int = method(env, "readInt", "()I");
    g_parcel.read_string = method(env, "readString", "()Ljava/lang/String;");
    g_parcel.write_string = method(env, "writeString", "(Ljava/lang/String;)V");
    g_parcel.obtain = method(env, "obtain", "()Landroid/os/Parcel;", true);
    g_parcel.append = method(env, "appendFrom", "(Landroid/os/Parcel;II)V");
    g_parcel.recycle = method(env, "recycle", "()V");
    g_parcel.native_size = method(env, "nativeDataSize", "(J)I", true);
    return g_parcel.native_ptr && g_parcel.size && g_parcel.position && g_parcel.set_position &&
           g_parcel.read_int && g_parcel.read_string && g_parcel.write_string && g_parcel.obtain &&
           g_parcel.append && g_parcel.recycle;
}
} // namespace

bool install_feature_hooks(JNIEnv *env, zygisk::Api *api) {
    if (g_installed) return g_status.query || g_status.enumeration;
    if (!env || !api || env->ExceptionCheck()) return false;
    if (env->PushLocalFrame(64) != JNI_OK) {
        clear_probe_exception(env);
        return false;
    }
    if (!init_methods(env)) {
        clear_probe_exception(env);
        if (g_parcel.cls) env->DeleteGlobalRef(g_parcel.cls);
        g_parcel = {};
        env->PopLocalFrame(nullptr);
        log_error("feature filtering unavailable: Parcel API is incomplete");
        return false;
    }
    jclass stub = env->FindClass("android/content/pm/IPackageManager$Stub");
    if (stub && !env->ExceptionCheck()) {
        g_has_feature_code = transaction_code(env, stub, "TRANSACTION_hasSystemFeature");
        g_list_features_code = transaction_code(env, stub, "TRANSACTION_getSystemAvailableFeatures");
    }
    clear_probe_exception(env);
    bool critical = false;
    bool lifecycle = g_list_features_code > 0 && g_parcel.native_size && position_abi(env, critical);
    clear_probe_exception(env);
    if (lifecycle) {
        lifecycle = hook_native(env, api, "android/os/Parcel", "nativeFreeBuffer", "(J)V",
                                hook_free_buffer, g_free_buffer);
        lifecycle = hook_native(env, api, "android/os/Parcel", "nativeDestroy", "(J)V",
                                hook_destroy, g_destroy) && lifecycle;
        const bool position = critical
            ? hook_native(env, api, "android/os/Parcel", "nativeSetDataPosition", "(JI)V",
                          hook_set_position_critical, g_set_position_critical)
            : hook_native(env, api, "android/os/Parcel", "nativeSetDataPosition", "(JI)V",
                          hook_set_position, g_set_position);
        lifecycle = lifecycle && position;
        lifecycle = hook_native(env, api, "android/os/Parcel", "nativeSetDataSize", "(JI)V",
                                hook_set_size, g_set_size) && lifecycle;
        lifecycle = hook_native(env, api, "android/os/Parcel", "nativeUnmarshall", "(J[BII)V",
                                hook_unmarshall, g_unmarshall) && lifecycle;
        lifecycle = hook_native(env, api, "android/os/Parcel", "nativeAppendFrom", "(JJII)V",
                                hook_append, g_append) && lifecycle;
    }
    bool readers = false;
    if (lifecycle) {
        const bool legacy = hook_native(env, api, "android/os/Parcel", "nativeReadString",
                                       "(J)Ljava/lang/String;", hook_read_string, g_read_string);
        const bool wide = hook_native(env, api, "android/os/Parcel", "nativeReadString16",
                                     "(J)Ljava/lang/String;", hook_read_string16, g_read_string16);
        const bool narrow = hook_native(env, api, "android/os/Parcel", "nativeReadString8",
                                       "(J)Ljava/lang/String;", hook_read_string8, g_read_string8);
        // A runtime exposing String8 may use either representation in a list.
        const bool has_narrow = method(env, "nativeReadString8", "(J)Ljava/lang/String;", true) != nullptr;
        const bool has_legacy = method(env, "nativeReadString", "(J)Ljava/lang/String;", true) != nullptr;
        const bool has_wide = method(env, "nativeReadString16", "(J)Ljava/lang/String;", true) != nullptr;
        readers = (has_legacy || has_wide) && (!has_legacy || legacy) &&
                  (!has_wide || wide) && (!has_narrow || narrow);
    }
    const bool transact = (g_has_feature_code > 0 || (lifecycle && readers)) &&
        hook_native(env, api, "android/os/BinderProxy", "transactNative",
                    "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", hook_transact, g_transact, false);
    g_status.query = transact && g_has_feature_code > 0;
    g_status.enumeration = transact && lifecycle && readers;
    g_installed = transact;
    env->PopLocalFrame(nullptr);
    clear_probe_exception(env);
    log_info("feature filtering: query=%d enumeration=%d", g_status.query, g_status.enumeration);
    return g_status.query || g_status.enumeration;
}

FeatureHookStatus feature_hook_status() { return g_status; }
