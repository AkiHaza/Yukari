#include "resource_hook.h"
#include "logger.h"
#include "resource_policy.h"

#include <cstdint>
#include <string_view>

namespace {
constexpr const char *kAssetManager = "android/content/res/AssetManager";

using NativeIdentifier = jint (*)(JNIEnv *, jclass, jlong, jstring, jstring, jstring);
using NativeValue = jint (*)(JNIEnv *, jclass, jlong, jint, jshort, jobject, jboolean);
using NativeName = jstring (*)(JNIEnv *, jclass, jlong, jint);
using LegacyIdentifier = jint (*)(JNIEnv *, jobject, jstring, jstring, jstring);
using LegacyValue = jint (*)(JNIEnv *, jobject, jint, jshort, jobject, jboolean);
using LegacyName = jstring (*)(JNIEnv *, jobject, jint);

NativeIdentifier native_identifier = nullptr;
NativeValue native_value = nullptr;
NativeName native_names[4]{};
LegacyIdentifier legacy_identifier = nullptr;
LegacyValue legacy_value = nullptr;
LegacyName legacy_names[4]{};
jmethodID native_packages = nullptr;
jmethodID legacy_packages = nullptr;
jmethodID sparse_get = nullptr;
bool package_flags = false;
bool attempted = false;
int installed = 0;
ResourceHookStatus status;
thread_local bool resolving_package = false;

void clear_lookup_exception(JNIEnv *env) {
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Each read is bounded by the Java string length, without UTF conversions.
class JavaText {
public:
    JavaText(JNIEnv *env, jstring value) : env_(env), value_(value) {
        if (!value_) { valid_ = true; return; }
        length_ = env_->GetStringLength(value_);
        if (env_->ExceptionCheck()) { clear_lookup_exception(env_); return; }
        chars_ = env_->GetStringChars(value_, nullptr);
        if (!chars_ || env_->ExceptionCheck()) { clear_lookup_exception(env_); return; }
        valid_ = true;
    }
    ~JavaText() { if (chars_) env_->ReleaseStringChars(value_, chars_); }
    JavaText(const JavaText &) = delete;
    JavaText &operator=(const JavaText &) = delete;
    bool valid() const { return valid_; }
    std::basic_string_view<jchar> view() const {
        return chars_ ? std::basic_string_view<jchar>(chars_, length_)
                      : std::basic_string_view<jchar>();
    }
private:
    JNIEnv *env_;
    jstring value_;
    const jchar *chars_ = nullptr;
    jsize length_ = 0;
    bool valid_ = false;
};

bool named_package_is_hidden(JNIEnv *env, jstring name, jstring package) {
    if (!name) return false;
    JavaText resource_name(env, name);
    JavaText default_package(env, package);
    return resource_name.valid() && default_package.valid() &&
           resource_policy::hidden_name(resource_name.view(), default_package.view());
}

// Query the current manager for every decision. Caching native pointers could
// retain stale mappings when an AssetManager is destroyed or gains loaders.
bool mapped_id_is_hidden(JNIEnv *env, jobject manager, jlong ptr, jint id, bool modern) {
    const auto package_id = static_cast<std::uint32_t>(id) >> 24;
    if (package_id == 0 || !sparse_get || resolving_package ||
        (modern ? !native_packages : !legacy_packages)) return false;
    if (env->PushLocalFrame(4) < 0) { clear_lookup_exception(env); return false; }
    resolving_package = true;
    jobject map = nullptr;
    if (modern) {
        const auto clazz = static_cast<jclass>(manager);
        map = package_flags
                  ? env->CallStaticObjectMethod(clazz, native_packages, ptr, JNI_TRUE, JNI_TRUE)
                  : env->CallStaticObjectMethod(clazz, native_packages, ptr);
    } else {
        map = env->CallObjectMethod(manager, legacy_packages);
    }
    bool hidden = false;
    if (!env->ExceptionCheck() && map) {
        auto package = static_cast<jstring>(env->CallObjectMethod(
            map, sparse_get, static_cast<jint>(package_id)));
        if (!env->ExceptionCheck() && package) {
            JavaText package_name(env, package);
            if (package_name.valid() && resource_policy::hidden_package(package_name.view())) {
                hidden = resource_policy::hidden_id(static_cast<std::uint32_t>(id),
                                                    static_cast<int>(package_id));
            }
        }
    }
    clear_lookup_exception(env);
    resolving_package = false;
    env->PopLocalFrame(nullptr);
    return hidden;
}

jint identifier_modern(JNIEnv *env, jclass clazz, jlong ptr, jstring name,
                       jstring type, jstring package) {
    if (!env || env->ExceptionCheck()) return native_identifier(env, clazz, ptr, name, type, package);
    if (named_package_is_hidden(env, name, package)) return 0;
    const auto id = native_identifier(env, clazz, ptr, name, type, package);
    return !env->ExceptionCheck() && mapped_id_is_hidden(env, clazz, ptr, id, true) ? 0 : id;
}

jint identifier_legacy(JNIEnv *env, jobject manager, jstring name, jstring type, jstring package) {
    if (!env || env->ExceptionCheck()) return legacy_identifier(env, manager, name, type, package);
    if (named_package_is_hidden(env, name, package)) return 0;
    const auto id = legacy_identifier(env, manager, name, type, package);
    return !env->ExceptionCheck() && mapped_id_is_hidden(env, manager, 0, id, false) ? 0 : id;
}

jint value_modern(JNIEnv *env, jclass clazz, jlong ptr, jint id, jshort density,
                  jobject output, jboolean resolve) {
    if (env && !env->ExceptionCheck() && mapped_id_is_hidden(env, clazz, ptr, id, true)) return 0;
    return native_value(env, clazz, ptr, id, density, output, resolve);
}

jint value_legacy(JNIEnv *env, jobject manager, jint id, jshort density,
                  jobject output, jboolean resolve) {
    // Android 8 returns a StringBlock index (zero is valid), unlike modern
    // nativeGetResourceValue's positive cookie. A missing value is negative.
    if (env && !env->ExceptionCheck() && mapped_id_is_hidden(env, manager, 0, id, false)) return -1;
    return legacy_value(env, manager, id, density, output, resolve);
}

template <int Index>
jstring name_modern(JNIEnv *env, jclass clazz, jlong ptr, jint id) {
    if (env && !env->ExceptionCheck() && mapped_id_is_hidden(env, clazz, ptr, id, true)) return nullptr;
    return native_names[Index](env, clazz, ptr, id);
}

template <int Index>
jstring name_legacy(JNIEnv *env, jobject manager, jint id) {
    if (env && !env->ExceptionCheck() && mapped_id_is_hidden(env, manager, 0, id, false)) return nullptr;
    return legacy_names[Index](env, manager, id);
}

jmethodID find_method(JNIEnv *env, jclass clazz, const char *name, const char *signature,
                      bool is_static) {
    const auto method = is_static ? env->GetStaticMethodID(clazz, name, signature)
                                  : env->GetMethodID(clazz, name, signature);
    if (env->ExceptionCheck()) { clear_lookup_exception(env); return nullptr; }
    return method;
}

template <typename Function>
int bind(JNIEnv *env, zygisk::Api *api, jclass clazz, const char *name,
         const char *signature, bool is_static, Function replacement, Function &original) {
    if (!find_method(env, clazz, name, signature, is_static)) return 0;
    JNINativeMethod method{const_cast<char *>(name), const_cast<char *>(signature),
                           reinterpret_cast<void *>(replacement)};
    api->hookJniNativeMethods(env, kAssetManager, &method, 1);
    clear_lookup_exception(env);
    if (!method.fnPtr || method.fnPtr == reinterpret_cast<void *>(replacement)) return 0;
    original = reinterpret_cast<Function>(method.fnPtr);
    return 1;
}
} // namespace

bool install_resource_hooks(JNIEnv *env, zygisk::Api *api) {
    if (!env || !api || env->ExceptionCheck()) return false;
    if (attempted) return installed != 0;
    attempted = true;
    if (env->PushLocalFrame(8) < 0) { clear_lookup_exception(env); return false; }
    jclass asset = env->FindClass(kAssetManager);
    if (!asset || env->ExceptionCheck()) {
        clear_lookup_exception(env);
        env->PopLocalFrame(nullptr);
        log_error("resource hooks unavailable: AssetManager missing");
        return false;
    }
    jclass sparse = env->FindClass("android/util/SparseArray");
    if (sparse && !env->ExceptionCheck()) {
        sparse_get = find_method(env, sparse, "get", "(I)Ljava/lang/Object;", false);
    }
    clear_lookup_exception(env);
    native_packages = find_method(env, asset, "nativeGetAssignedPackageIdentifiers",
                                  "(JZZ)Landroid/util/SparseArray;", true);
    package_flags = native_packages != nullptr;
    if (!native_packages) {
        native_packages = find_method(env, asset, "nativeGetAssignedPackageIdentifiers",
                                      "(J)Landroid/util/SparseArray;", true);
    }
    legacy_packages = find_method(env, asset, "getAssignedPackageIdentifiers",
                                  "()Landroid/util/SparseArray;", false);

    int modern = 0;
    modern += bind(env, api, asset, "nativeGetResourceIdentifier",
                   "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", true,
                   identifier_modern, native_identifier);
    modern += bind(env, api, asset, "nativeGetResourceValue",
                   "(JISLandroid/util/TypedValue;Z)I", true, value_modern, native_value);
    constexpr const char *modern_names[] = {
        "nativeGetResourceName", "nativeGetResourcePackageName",
        "nativeGetResourceTypeName", "nativeGetResourceEntryName"};
    const NativeName modern_hooks[] = {name_modern<0>, name_modern<1>, name_modern<2>, name_modern<3>};
    for (int i = 0; i < 4; ++i) {
        modern += bind(env, api, asset, modern_names[i], "(JI)Ljava/lang/String;",
                       true, modern_hooks[i], native_names[i]);
    }
    int legacy = 0;
    legacy += bind(env, api, asset, "getResourceIdentifier",
                   "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", false,
                   identifier_legacy, legacy_identifier);
    legacy += bind(env, api, asset, "loadResourceValue",
                   "(ISLandroid/util/TypedValue;Z)I", false, value_legacy, legacy_value);
    constexpr const char *legacy_names_list[] = {
        "getResourceName", "getResourcePackageName", "getResourceTypeName", "getResourceEntryName"};
    const LegacyName legacy_hooks[] = {name_legacy<0>, name_legacy<1>, name_legacy<2>, name_legacy<3>};
    for (int i = 0; i < 4; ++i) {
        legacy += bind(env, api, asset, legacy_names_list[i], "(I)Ljava/lang/String;",
                       false, legacy_hooks[i], legacy_names[i]);
    }
    installed = modern + legacy;
    status = {modern, legacy, sparse_get && native_packages, sparse_get && legacy_packages};
    env->PopLocalFrame(nullptr);
    log_info("resource hooks: modern=%d/6 legacy=%d/6 mapped_ids=%d/%d", modern, legacy,
             sparse_get && native_packages ? 1 : 0, sparse_get && legacy_packages ? 1 : 0);
    return installed != 0;
}

ResourceHookStatus resource_hook_status() { return status; }
