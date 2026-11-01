#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <jni.h>
using jsize = jint;
using jclass = jobject;
using jfieldID = void *;
using jmethodID = void *;

namespace {
[[noreturn]] void failed(const char *expression, int line) {
    std::fprintf(stderr, "cache test line %d: %s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) ((expression) ? void() : failed(#expression, __LINE__))
}

struct _jobject {
    const char *text = nullptr;
    bool string = false;
};

struct JNIEnv {
    static jmethodID method(int number) {
        return reinterpret_cast<jmethodID>(static_cast<std::uintptr_t>(number));
    }

    bool exception = false;
    bool empty_cache = false;
    bool return_with_exception = false;
    bool null_without_exception = false;
    int fail_at = -1;
    int current_call = 0;
    int frame_pushes = 0;
    int frame_pops = 0;
    int clears = 0;
    int string_gets = 0;
    int string_releases = 0;
    int chars_outstanding = 0;
    int removed_refs_deleted = 0;
    int direct_key_deletes = 0;
    bool frame_active = false;
    std::vector<std::string> calls;
    std::vector<jobject> removed_keys;
    std::unordered_map<jobject, int> references;

    _jobject manager_class;
    _jobject map_class;
    _jobject set_class;
    _jobject string_class;
    _jobject cache;
    _jobject keys;
    _jobject array;
    _jobject removed_value;
    _jobject hidden_a{"lineage.hardware", true};
    _jobject normal{"activity", true};
    _jobject non_string{"not a Java String", false};
    _jobject hidden_b{"LineAge.foo", true};
    _jobject profile{"profile", true};
    std::vector<jobject> elements{&hidden_a, &normal, &non_string, nullptr, &hidden_b, &profile};

    bool hit(const char *name) {
        // The only JNI operations allowed while pending are the explicit
        // cleanup/exception functions below, which do not call hit().
        CHECK(!exception);
        calls.emplace_back(name);
        const bool inject = current_call++ == fail_at;
        if (inject && !null_without_exception) exception = true;
        return inject;
    }
    template <typename T> T local(T object) {
        if (object) { CHECK(frame_active); ++references[object]; }
        return object;
    }
    int outstanding_references() const {
        int count = 0;
        for (const auto &entry : references) count += entry.second;
        return count;
    }
    bool ExceptionCheck() { return exception; }
    void ExceptionClear() { CHECK(exception); exception = false; ++clears; }
    jint PushLocalFrame(jint capacity) {
        CHECK(capacity >= 8);
        if (hit("PushLocalFrame")) return -1;
        CHECK(!frame_active);
        frame_active = true;
        ++frame_pushes;
        return 0;
    }
    jobject PopLocalFrame(jobject kept) {
        CHECK(frame_active);
        CHECK(kept == nullptr);
        CHECK(chars_outstanding == 0);
        frame_active = false;
        ++frame_pops;
        references.clear();
        return kept;
    }
    jclass FindClass(const char *name) {
        if (hit("FindClass")) return nullptr;
        if (std::strcmp(name, "android/os/ServiceManager") == 0) return local(&manager_class);
        if (std::strcmp(name, "java/util/Map") == 0) return local(&map_class);
        if (std::strcmp(name, "java/util/Set") == 0) return local(&set_class);
        CHECK(std::strcmp(name, "java/lang/String") == 0);
        return local(&string_class);
    }
    jfieldID GetStaticFieldID(jclass clazz, const char *name, const char *signature) {
        CHECK(clazz == &manager_class);
        CHECK(std::strcmp(name, "sCache") == 0);
        CHECK(std::strcmp(signature, "Ljava/util/Map;") == 0);
        if (hit("GetStaticFieldID")) return nullptr;
        return reinterpret_cast<jfieldID>(std::uintptr_t{1});
    }
    jobject GetStaticObjectField(jclass clazz, jfieldID field) {
        CHECK(clazz == &manager_class);
        CHECK(field != nullptr);
        if (hit("GetStaticObjectField")) return nullptr;
        return empty_cache ? nullptr : local(&cache);
    }
    jmethodID GetMethodID(jclass clazz, const char *name, const char *signature) {
        if (hit("GetMethodID")) return nullptr;
        if (std::strcmp(name, "keySet") == 0) {
            CHECK(clazz == &map_class);
            CHECK(std::strcmp(signature, "()Ljava/util/Set;") == 0);
            return method(1);
        }
        if (std::strcmp(name, "remove") == 0) {
            CHECK(clazz == &map_class);
            CHECK(std::strcmp(signature, "(Ljava/lang/Object;)Ljava/lang/Object;") == 0);
            return method(2);
        }
        CHECK(clazz == &set_class);
        CHECK(std::strcmp(name, "toArray") == 0);
        CHECK(std::strcmp(signature, "()[Ljava/lang/Object;") == 0);
        return method(3);
    }
    jobject CallObjectMethod(jobject receiver, jmethodID operation, ...) {
        const bool injected = hit("CallObjectMethod");
        if (injected && !return_with_exception) return nullptr;
        if (operation == method(1)) {
            CHECK(receiver == &cache);
            return local(&keys);
        }
        if (operation == method(3)) {
            CHECK(receiver == &keys);
            return local(&array);
        }
        CHECK(operation == method(2));
        CHECK(receiver == &cache);
        va_list args;
        va_start(args, operation);
        const auto key = va_arg(args, jobject);
        va_end(args);
        CHECK(key == &hidden_a || key == &hidden_b || key == &profile);
        removed_keys.push_back(key);
        return local(&removed_value);
    }
    jsize GetArrayLength(jobjectArray value) {
        CHECK(value == &array);
        if (hit("GetArrayLength")) return 0;
        return static_cast<jsize>(elements.size());
    }
    jobject GetObjectArrayElement(jobjectArray value, jsize index) {
        CHECK(value == &array);
        CHECK(index >= 0 && static_cast<std::size_t>(index) < elements.size());
        const bool injected = hit("GetObjectArrayElement");
        if (injected && !return_with_exception) return nullptr;
        return local(elements[static_cast<std::size_t>(index)]);
    }
    bool IsInstanceOf(jobject value, jclass clazz) {
        CHECK(clazz == &string_class);
        CHECK(value != nullptr);
        if (hit("IsInstanceOf")) return false;
        return value->string;
    }
    const char *GetStringUTFChars(jstring value, jboolean *) {
        CHECK(value != nullptr && value->string);
        ++string_gets;
        const bool injected = hit("GetStringUTFChars");
        if (injected && !return_with_exception) return nullptr;
        ++chars_outstanding;
        return value->text;
    }
    void ReleaseStringUTFChars(jstring value, const char *characters) {
        CHECK(value != nullptr && value->string);
        CHECK(characters == value->text);
        CHECK(chars_outstanding > 0);
        --chars_outstanding;
        ++string_releases;
    }
    void DeleteLocalRef(jobject value) {
        CHECK(value != nullptr);
        CHECK(references[value] > 0);
        --references[value];
        if (value == &removed_value) ++removed_refs_deleted;
        for (const auto key : elements) { if (value == key) ++direct_key_deletes; }
    }
};

#include "../../module/src/main/cpp/service_cache.cpp"

namespace {
void cleaned(const JNIEnv &env) {
    CHECK(!env.exception);
    CHECK(!env.frame_active);
    CHECK(env.frame_pushes == env.frame_pops);
    CHECK(env.chars_outstanding == 0);
    CHECK(env.outstanding_references() == 0);
    CHECK(env.removed_refs_deleted == static_cast<int>(env.removed_keys.size()));
}

void success_cases() {
    CHECK(!clear_cache(nullptr));
    JNIEnv env;
    CHECK(clear_cache(&env));
    cleaned(env);
    CHECK(env.frame_pushes == 1);
    CHECK(env.clears == 0);
    CHECK(env.removed_keys.size() == 3);
    CHECK(env.string_gets == 4); // Non-String and null keys are skipped.
    CHECK(env.string_gets == env.string_releases);
    CHECK(env.direct_key_deletes == 5);
    JNIEnv empty;
    empty.empty_cache = true;
    CHECK(clear_cache(&empty));
    cleaned(empty);
    CHECK(empty.string_gets == 0);
    JNIEnv pending;
    pending.exception = true;
    CHECK(!clear_cache(&pending));
    CHECK(pending.exception);
    CHECK(pending.calls.empty());
    CHECK(pending.clears == 0);
    CHECK(pending.frame_pushes == 0);
    CHECK(pending.frame_pops == 0);
}

void failure_cases() {
    JNIEnv baseline;
    CHECK(clear_cache(&baseline));
    for (int index = 0; index < static_cast<int>(baseline.calls.size()); ++index) {
        JNIEnv env;
        env.fail_at = index;
        CHECK(!clear_cache(&env));
        cleaned(env);
        CHECK(env.current_call == index + 1);
        CHECK(env.clears == 1);
        // JNI can return a nonnull reference/character pointer with an
        // exception. These must still be released before unwinding the frame.
        const auto &operation = baseline.calls[index];
        if (operation == "CallObjectMethod" || operation == "GetObjectArrayElement" ||
            operation == "GetStringUTFChars") {
            JNIEnv with_value;
            with_value.fail_at = index;
            with_value.return_with_exception = true;
            CHECK(!clear_cache(&with_value));
            cleaned(with_value);
            CHECK(with_value.current_call == index + 1);
            CHECK(with_value.clears == 1);
        }
        // Optional member lookup and extraction failures must also terminate
        // safely if the VM does not supply a throwable.
        if (operation == "FindClass" || operation == "GetStaticFieldID" ||
            operation == "GetMethodID" || operation == "GetStringUTFChars") {
            JNIEnv null_only;
            null_only.fail_at = index;
            null_only.null_without_exception = true;
            CHECK(!clear_cache(&null_only));
            cleaned(null_only);
            CHECK(null_only.current_call == index + 1);
            CHECK(null_only.clears == 0);
        }
    }
    std::printf("cache JNI tests passed (%zu fault sites)\n", baseline.calls.size());
}
}

int main() {
    success_cases();
    failure_cases();
}
