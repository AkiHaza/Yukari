#include "resource_policy.h"

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>

// The shared host JNI header deliberately contains declarations only. Complete
// them here so the actual production hook code can run without a JVM.
#include <jni.h>
using jchar = unsigned short;
using jshort = short;
using jsize = jint;
using jclass = jobject;
using jmethodID = void *;
constexpr jboolean JNI_TRUE = 1;

struct _jobject {
    std::basic_string<jchar> characters;
    jobject packages = nullptr;
    jobject entries[256]{};
};

struct JNIEnv {
    bool exception = false;
    bool map_failure = false;
    bool string_failure = false;
    int original_calls = 0;
    int map_calls = 0;
    jobject manager_a = nullptr;
    jobject manager_b = nullptr;

    bool ExceptionCheck() { return exception; }
    void ExceptionClear() { exception = false; }
    jsize GetStringLength(jstring value) { return static_cast<jsize>(value->characters.size()); }
    const jchar *GetStringChars(jstring value, jboolean *) {
        if (string_failure) { exception = true; return nullptr; }
        return value->characters.c_str();
    }
    void ReleaseStringChars(jstring, const jchar *) {}
    jint PushLocalFrame(jint) { return 0; }
    jobject PopLocalFrame(jobject value) { return value; }
    jclass FindClass(const char *) { return nullptr; }
    jmethodID GetStaticMethodID(jclass, const char *, const char *) { return nullptr; }
    jmethodID GetMethodID(jclass, const char *, const char *) { return nullptr; }
    jobject CallStaticObjectMethod(jclass, jmethodID method, ...) {
        ++map_calls;
        if (map_failure) { exception = true; return nullptr; }
        va_list args;
        va_start(args, method);
        const auto ptr = va_arg(args, jlong);
        va_end(args);
        const auto manager = ptr == 1 ? manager_a : manager_b;
        return manager ? manager->packages : nullptr;
    }
    jobject CallObjectMethod(jobject object, jmethodID method, ...) {
        if (method == reinterpret_cast<jmethodID>(std::uintptr_t{1})) {
            ++map_calls;
            if (map_failure) { exception = true; return nullptr; }
            return object->packages;
        }
        va_list args;
        va_start(args, method);
        const int key = va_arg(args, int);
        va_end(args);
        return key >= 0 && key < 256 ? object->entries[key] : nullptr;
    }
};

void log_info(const char *, ...) {}
void log_error(const char *, ...) {}
#include "../../module/src/main/cpp/resource_hook.cpp"

namespace {
[[noreturn]] void failed(const char *expression, int line) {
    std::fprintf(stderr, "resource test line %d: %s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) ((expression) ? void() : failed(#expression, __LINE__))
}

void policy_tests() {
    using namespace std::literals;
    using resource_policy::hidden_id;
    using resource_policy::hidden_name;
    using resource_policy::hidden_package;
    CHECK(hidden_package("lineageos.platform"sv));
    CHECK(hidden_package(u"lineageos.platform"sv));
    CHECK(!hidden_package("Lineageos.platform"sv));
    CHECK(!hidden_package("lineageos.platform.extra"sv));
    CHECK(!hidden_package("other.lineageos.platform"sv));
    CHECK(!hidden_package(""sv));
    CHECK(!hidden_package("lineageos.platform\0extra"sv));
    CHECK(hidden_name("string/name"sv, "lineageos.platform"sv));
    CHECK(hidden_name("lineageos.platform:string/name"sv, "android"sv));
    CHECK(hidden_name(u"lineageos.platform:color/example"sv, u""sv));
    CHECK(!hidden_name("android:string/name"sv, "lineageos.platform"sv));
    CHECK(!hidden_name("lineageos.platform.extra:string/name"sv, ""sv));
    CHECK(!hidden_name("lineageos.platform"sv, "android"sv));
    CHECK(!hidden_name(":string/name"sv, "lineageos.platform"sv));
    CHECK(!hidden_name(""sv, "lineageos.platform"sv));
    CHECK(!hidden_id(0x3f010001, 0));
    CHECK(!hidden_id(0x3f010001, -1));
    CHECK(!hidden_id(0x3f010001, 256));
    CHECK(!hidden_id(0x3f010001, 0x40));
    CHECK(hidden_id(0x3f010001, 0x3f));
    CHECK(hidden_id(0x40010001, 0x40));
    CHECK(!hidden_id(0x01010001, 0x3f));
    CHECK(!hidden_id(0x7f010001, 0x3f));
    CHECK(!hidden_id(0, 0));
}

namespace {
jint original_id = 0;
jobject original_name = nullptr;

jint modern_identifier(JNIEnv *env, jclass, jlong, jstring, jstring, jstring) {
    ++env->original_calls;
    return original_id;
}
jint modern_value(JNIEnv *env, jclass, jlong, jint, jshort, jobject, jboolean) {
    ++env->original_calls;
    return 7;
}
jint old_value(JNIEnv *env, jobject, jint, jshort, jobject, jboolean) {
    ++env->original_calls;
    return 0; // StringBlock zero is a successful legacy lookup.
}
jstring modern_name(JNIEnv *env, jclass, jlong, jint) {
    ++env->original_calls;
    return original_name;
}

_jobject string_object(std::u16string_view value) {
    _jobject object;
    for (char16_t ch : value) object.characters.push_back(static_cast<jchar>(ch));
    return object;
}

void hook_tests() {
    auto lineage = string_object(u"lineageos.platform");
    auto unrelated = string_object(u"example.resources");
    auto explicit_lineage = string_object(u"lineageos.platform:string/name");
    auto explicit_android = string_object(u"android:string/name");
    auto bare_name = string_object(u"string/name");
    _jobject map;
    map.entries[0x3f] = &unrelated;
    map.entries[0x40] = &lineage;
    _jobject manager;
    manager.packages = &map;
    _jobject other_map;
    other_map.entries[0x40] = &unrelated;
    _jobject other_manager;
    other_manager.packages = &other_map;
    JNIEnv env;
    env.manager_a = &manager;
    env.manager_b = &other_manager;
    native_packages = reinterpret_cast<jmethodID>(std::uintptr_t{1});
    legacy_packages = native_packages;
    sparse_get = reinterpret_cast<jmethodID>(std::uintptr_t{2});
    native_identifier = modern_identifier;
    native_value = modern_value;
    legacy_value = old_value;
    native_names[0] = modern_name;
    original_name = &unrelated;

    // A historical ID alone must never hide an unrelated package.
    CHECK(!mapped_id_is_hidden(&env, &manager, 1, 0x3f010001, true));
    CHECK(mapped_id_is_hidden(&env, &manager, 1, 0x40010001, true));
    CHECK(!mapped_id_is_hidden(&env, &other_manager, 2, 0x40010001, true));
    CHECK(!mapped_id_is_hidden(&env, &manager, 1, 0x7f010001, true));
    CHECK(value_modern(&env, &manager, 1, 0x3f010001, 0, nullptr, 0) == 7);
    const int calls = env.original_calls;
    CHECK(value_modern(&env, &manager, 1, 0x40010001, 0, nullptr, 0) == 0);
    CHECK(env.original_calls == calls);
    CHECK(value_legacy(&env, &manager, 0x40010001, 0, nullptr, 0) == -1);
    CHECK(value_legacy(&env, &manager, 0x3f010001, 0, nullptr, 0) == 0);
    CHECK(name_modern<0>(&env, &manager, 1, 0x40010001) == nullptr);
    CHECK(name_modern<0>(&env, &manager, 1, 0x3f010001) == &unrelated);

    // Names with an explicit different package override the default.
    original_id = 0x3f010001;
    CHECK(identifier_modern(&env, &manager, 1, &explicit_lineage, nullptr, &unrelated) == 0);
    CHECK(identifier_modern(&env, &manager, 1, &bare_name, nullptr, &lineage) == 0);
    CHECK(identifier_modern(&env, &manager, 1, &explicit_android, nullptr, &lineage) == original_id);
    original_id = 0x40010001;
    CHECK(identifier_modern(&env, &manager, 1, &bare_name, nullptr, &unrelated) == 0);

    // A loader/package change on the same manager must take effect immediately.
    map.entries[0x40] = &unrelated;
    CHECK(value_modern(&env, &manager, 1, 0x40010001, 0, nullptr, 0) == 7);
    map.entries[0x40] = &lineage;
    env.map_failure = true;
    CHECK(value_modern(&env, &manager, 1, 0x40010001, 0, nullptr, 0) == 7);
    CHECK(!env.ExceptionCheck());
    env.map_failure = false;
    env.string_failure = true;
    CHECK(value_modern(&env, &manager, 1, 0x40010001, 0, nullptr, 0) == 7);
    CHECK(!env.ExceptionCheck());
    env.string_failure = false;
    native_packages = nullptr;
    CHECK(value_modern(&env, &manager, 1, 0x40010001, 0, nullptr, 0) == 7);
    native_packages = reinterpret_cast<jmethodID>(std::uintptr_t{1});
    env.exception = true;
    const int map_calls = env.map_calls;
    CHECK(value_modern(&env, &manager, 1, 0x40010001, 0, nullptr, 0) == 7);
    CHECK(env.ExceptionCheck());
    CHECK(env.map_calls == map_calls);
}
}

int main() {
    policy_tests();
    hook_tests();
    std::puts("resource policy and JNI hook tests passed");
}
