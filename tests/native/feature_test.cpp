#include "feature_policy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef YUKARI_FEATURE_JNI_TEST
#include <cstdarg>
#include <cstring>
#include <memory>
#include <vector>
#include "../../module/src/main/cpp/feature_hook.cpp"

void log_info(const char *, ...) {}
void log_error(const char *, ...) {}
#endif

namespace {
void check(bool ok, int line) {
    if (!ok) {
        std::fprintf(stderr, "feature test failed at line %d\n", line);
        std::exit(1);
    }
}
#define CHECK(value) check((value), __LINE__)
}

#ifdef YUKARI_FEATURE_JNI_TEST
namespace fake_jni {
struct Parcel : _jobject {
    std::vector<unsigned char> bytes;
    jint position = 0;
    bool recycled = false;
};
struct String : _jstring {
    explicit String(std::u16string text) : value(std::move(text)) {}
    std::u16string value;
};
std::vector<std::unique_ptr<Parcel>> parcels;
std::vector<std::unique_ptr<String>> strings;
_jthrowable exception_value;
bool pending = false;
bool fail_obtain = false;
bool throw_transaction = false;
jboolean transaction_result = JNI_TRUE;
std::vector<unsigned char> sent;
jobject sent_object = nullptr;

enum Method : std::uintptr_t { Size = 1, Position, SetPosition, ReadInt, ReadString,
                              WriteString, Obtain, Append, Recycle, NativeSize };
jmethodID id(Method value) { return reinterpret_cast<jmethodID>(value); }
Parcel *parcel(jobject object) { return static_cast<Parcel *>(object); }
Parcel *by_pointer(jlong pointer) { return reinterpret_cast<Parcel *>(pointer); }
jlong pointer(Parcel *value) { return reinterpret_cast<jlong>(value); }

Parcel *make_parcel() {
    parcels.push_back(std::make_unique<Parcel>());
    return parcels.back().get();
}
jstring make_string(std::u16string text) {
    strings.push_back(std::make_unique<String>(std::move(text)));
    return strings.back().get();
}
void put_int(Parcel &value, jint number) {
    const auto position = static_cast<std::size_t>(value.position);
    if (value.bytes.size() < position + 4) value.bytes.resize(position + 4);
    std::memcpy(value.bytes.data() + position, &number, 4);
    value.position += 4;
}
jint get_int(Parcel &value) {
    if (value.position < 0 || static_cast<std::size_t>(value.position) + 4 > value.bytes.size()) {
        pending = true;
        return 0;
    }
    jint result;
    std::memcpy(&result, value.bytes.data() + value.position, 4);
    value.position += 4;
    return result;
}
void put_string(Parcel &value, std::u16string_view text) {
    put_int(value, static_cast<jint>(text.size()));
    const auto end = (static_cast<std::size_t>(value.position) + (text.size() + 1) * 2 + 3) & ~std::size_t{3};
    if (value.bytes.size() < end) value.bytes.resize(end);
    std::memcpy(value.bytes.data() + value.position, text.data(), text.size() * 2);
    const char16_t terminator = 0;
    std::memcpy(value.bytes.data() + value.position + text.size() * 2, &terminator, 2);
    value.position = static_cast<jint>(end);
}
jstring get_string(Parcel &value) {
    const jint length = get_int(value);
    if (pending || length < 0 || length > 256 ||
        static_cast<std::size_t>(value.position) + (static_cast<std::size_t>(length) + 1) * 2 > value.bytes.size()) {
        pending = true;
        return nullptr;
    }
    std::u16string result(static_cast<std::size_t>(length), u'\0');
    std::memcpy(result.data(), value.bytes.data() + value.position, static_cast<std::size_t>(length) * 2);
    value.position = static_cast<jint>((static_cast<std::size_t>(value.position) + (static_cast<std::size_t>(length) + 1) * 2 + 3) & ~std::size_t{3});
    return make_string(std::move(result));
}

jboolean exception_check(JNIEnv *) { return pending ? JNI_TRUE : JNI_FALSE; }
void exception_clear(JNIEnv *) { pending = false; }
jthrowable exception_occurred(JNIEnv *) { return pending ? &exception_value : nullptr; }
jint throw_exception(JNIEnv *, jthrowable) { pending = true; return JNI_OK; }
void delete_local(JNIEnv *, jobject) {}
jsize string_length(JNIEnv *, jstring value) { return static_cast<jsize>(static_cast<String *>(value)->value.size()); }
void string_region(JNIEnv *, jstring value, jsize offset, jsize size, jchar *out) {
    const auto &text = static_cast<String *>(value)->value;
    std::memcpy(out, text.data() + offset, static_cast<std::size_t>(size) * 2);
}
jstring new_string(JNIEnv *, const jchar *value, jsize size) {
    return make_string(std::u16string(reinterpret_cast<const char16_t *>(value), static_cast<std::size_t>(size)));
}
jlong long_field(JNIEnv *, jobject object, jfieldID) { return pointer(parcel(object)); }
jint call_int(JNIEnv *, jobject object, jmethodID method, va_list) {
    auto &value = *parcel(object);
    if (method == id(Size)) return static_cast<jint>(value.bytes.size());
    if (method == id(Position)) return value.position;
    CHECK(method == id(ReadInt));
    return get_int(value);
}
jobject call_object(JNIEnv *, jobject object, jmethodID method, va_list) {
    CHECK(method == id(ReadString));
    return get_string(*parcel(object));
}
void original_position(JNIEnv *, jclass, jlong object, jint position) {
    auto &value = *by_pointer(object);
    if (position < 0 || static_cast<std::size_t>(position) > value.bytes.size()) pending = true;
    else value.position = position;
}
void original_lifecycle(JNIEnv *, jclass, jlong) {}
void call_void(JNIEnv *env, jobject object, jmethodID method, va_list args) {
    auto &value = *parcel(object);
    if (method == id(SetPosition)) {
        hook_set_position(env, nullptr, pointer(&value), va_arg(args, jint));
    } else if (method == id(WriteString)) {
        auto text = va_arg(args, jstring);
        put_string(value, static_cast<String *>(text)->value);
    } else if (method == id(Append)) {
        auto *source = parcel(va_arg(args, jobject));
        const jint start = va_arg(args, jint);
        const jint size = va_arg(args, jint);
        value.bytes.assign(source->bytes.begin() + start, source->bytes.begin() + start + size);
        value.position = size;
    } else {
        CHECK(method == id(Recycle));
        value.recycled = true;
        hook_free_buffer(env, nullptr, pointer(&value));
    }
}
jobject call_static_object(JNIEnv *, jclass, jmethodID method, va_list) {
    CHECK(method == id(Obtain));
    if (fail_obtain) { pending = true; return nullptr; }
    return make_parcel();
}
jint call_static_int(JNIEnv *, jclass, jmethodID method, va_list args) {
    CHECK(method == id(NativeSize));
    return static_cast<jint>(by_pointer(va_arg(args, jlong))->bytes.size());
}
jboolean original_transact(JNIEnv *, jobject, jint, jobject data, jobject reply, jint) {
    sent = parcel(data)->bytes;
    sent_object = data;
    if (reply) {
        auto &output = *parcel(reply);
        output.bytes.clear();
        output.position = 0;
        put_int(output, 0);
        put_int(output, 1);
        put_string(output, u"org.lineageos.hardware");
        output.position = 0;
    }
    if (throw_transaction) pending = true;
    return transaction_result;
}

void initialize(JNIEnv &env, JNINativeInterface_ &table) {
    table.ExceptionCheck = exception_check;
    table.ExceptionClear = exception_clear;
    table.ExceptionOccurred = exception_occurred;
    table.Throw = throw_exception;
    table.DeleteLocalRef = delete_local;
    table.GetStringLength = string_length;
    table.GetStringRegion = string_region;
    table.NewString = new_string;
    table.GetLongField = long_field;
    table.CallIntMethodV = call_int;
    table.CallObjectMethodV = call_object;
    table.CallVoidMethodV = call_void;
    table.CallStaticObjectMethodV = call_static_object;
    table.CallStaticIntMethodV = call_static_int;
    env.functions = &table;
    g_parcel = {reinterpret_cast<jclass>(1), reinterpret_cast<jfieldID>(1),
                id(Size), id(Position), id(SetPosition), id(ReadInt), id(ReadString),
                id(WriteString), id(Obtain), id(Append), id(Recycle), id(NativeSize)};
    g_set_position = original_position;
    g_free_buffer = original_lifecycle;
    g_destroy = original_lifecycle;
    g_transact = original_transact;
    g_has_feature_code = 100;
    g_list_features_code = 101;
    g_status = {true, true};
    g_replies = {};
}

Parcel *request(std::u16string_view token, std::u16string_view feature) {
    Parcel *input = make_parcel();
    put_int(*input, 0);
    put_int(*input, 0);
    put_int(*input, 0);
    put_string(*input, token);
    put_string(*input, feature);
    put_int(*input, 1);
    input->position = 12;
    return input;
}

jstring original_read(JNIEnv *, jclass, jlong object) {
    return get_string(*by_pointer(object));
}

void run() {
    JNIEnv env{};
    JNINativeInterface_ table{};
    initialize(env, table);
    auto *input = request(u"android.content.pm.IPackageManager", u"org.lineageos.hardware");
    auto *reply = make_parcel();
    const auto original = input->bytes;
    CHECK(hook_transact(&env, nullptr, 100, input, reply, 0) == JNI_TRUE);
    CHECK(!pending && input->bytes == original && input->position == 12);
    CHECK(sent_object != input && parcel(sent_object)->recycled);
    CHECK(sent.size() == original.size() && sent != original);

    fail_obtain = true;
    CHECK(hook_transact(&env, nullptr, 100, input, reply, 0) == JNI_TRUE);
    CHECK(!pending && sent_object == input && input->bytes == original && input->position == 12);
    fail_obtain = false;
    throw_transaction = true;
    CHECK(hook_transact(&env, nullptr, 100, input, reply, 0) == JNI_TRUE);
    CHECK(pending && input->bytes == original && parcel(sent_object)->recycled);
    throw_transaction = false;
    pending = false;

    auto *unknown = request(u"other.Interface", u"org.lineageos.hardware");
    CHECK(hook_transact(&env, nullptr, 100, unknown, reply, 0) == JNI_TRUE);
    CHECK(sent_object == unknown && unknown->position == 12);
    auto *clean = request(u"android.content.pm.IPackageManager", u"android.hardware.camera");
    CHECK(hook_transact(&env, nullptr, 100, clean, reply, 0) == JNI_TRUE);
    CHECK(sent_object == clean);
    CHECK(hook_transact(&env, nullptr, 100, input, reply, 1) == JNI_TRUE);
    CHECK(sent_object == input);

    CHECK(hook_transact(&env, nullptr, 101, input, reply, 0) == JNI_TRUE);
    const auto reply_bytes = reply->bytes;
    CHECK(g_replies.contains(static_cast<std::uintptr_t>(pointer(reply)), reply->bytes.size()));
    g_read_string8 = original_read;
    reply->position = 8;
    auto filtered = hook_read_string8(&env, nullptr, pointer(reply));
    CHECK(static_cast<String *>(filtered)->value == std::u16string(std::u16string_view(u"org.lineageos.hardware").size(), u'_'));
    CHECK(reply->bytes == reply_bytes);
    auto *unrelated = make_parcel();
    put_string(*unrelated, u"org.lineageos.hardware");
    unrelated->position = 0;
    auto visible = hook_read_string8(&env, nullptr, pointer(unrelated));
    CHECK(static_cast<String *>(visible)->value == u"org.lineageos.hardware");

    hook_set_position(&env, nullptr, pointer(reply), 8);
    CHECK(!g_replies.contains(static_cast<std::uintptr_t>(pointer(reply)), reply->bytes.size()));
    CHECK(static_cast<String *>(hook_read_string8(&env, nullptr, pointer(reply)))->value == u"org.lineageos.hardware");
    CHECK(hook_transact(&env, nullptr, 101, input, reply, 0) == JNI_TRUE);
    hook_free_buffer(&env, nullptr, pointer(reply));
    CHECK(!g_replies.contains(static_cast<std::uintptr_t>(pointer(reply)), reply->bytes.size()));
    CHECK(hook_transact(&env, nullptr, 101, input, reply, 0) == JNI_TRUE);
    hook_destroy(&env, nullptr, pointer(reply));
    CHECK(!g_replies.contains(static_cast<std::uintptr_t>(pointer(reply)), reply->bytes.size()));
    CHECK(hook_transact(&env, nullptr, 101, input, reply, 0) == JNI_TRUE);
    CHECK(hook_transact(&env, nullptr, 999, input, reply, 0) == JNI_TRUE);
    CHECK(!g_replies.contains(static_cast<std::uintptr_t>(pointer(reply)), reply->bytes.size()));
    transaction_result = JNI_FALSE;
    CHECK(hook_transact(&env, nullptr, 101, input, reply, 0) == JNI_FALSE);
    CHECK(!g_replies.contains(static_cast<std::uintptr_t>(pointer(reply)), reply->bytes.size()));
    transaction_result = JNI_TRUE;
    CHECK(!pending);
    std::puts("Feature JNI boundary tests passed");
}
} // namespace fake_jni
#endif

int main() {
    using namespace yukari::features;
    for (auto name : names) {
        CHECK(hidden(name));
        const std::u16string wide(name.begin(), name.end());
        CHECK(hidden(std::u16string_view(wide)));
        CHECK(!hidden(std::string_view(std::string(name) + ".extra")));
        CHECK(!hidden(std::string_view(std::string("x") + std::string(name))));
    }
    CHECK(!hidden(std::string_view{}));
    CHECK(!hidden(std::string_view("org.Lineageos.hardware")));
    CHECK(!hidden(std::string_view("android.hardware.camera")));
    CHECK(!hidden(std::string_view("org.lineageos.unknown")));
    CHECK(!hidden(std::u16string_view(u"org.lineageos.hardware\u0080")));

    ReplyRegistry<2> replies;
    CHECK(!replies.mark(0, 12));
    CHECK(!replies.mark(1, 0));
    CHECK(replies.mark(1, 40));
    CHECK(replies.contains(1, 40));
    CHECK(!replies.contains(2, 40)); // Unrelated Parcel never inherits scope.
    CHECK(replies.mark(2, 80));
    CHECK(!replies.mark(3, 90)); // Full table fails visible.
    CHECK(replies.contains(1, 40));
    CHECK(replies.contains(2, 80));
    CHECK(!replies.contains(1, 41)); // A size change invalidates old provenance.
    CHECK(!replies.contains(1, 40));
    CHECK(replies.mark(3, 90));
    replies.forget(2); // recycle/destroy/reposition/Binder reuse
    CHECK(!replies.contains(2, 80));
    CHECK(replies.mark(2, 70));
    CHECK(!replies.contains(2, 80));
    CHECK(!replies.contains(2, 70));
    replies.forget(3);
    CHECK(replies.mark(3, 100));
    CHECK(replies.mark(3, 120));
    CHECK(replies.contains(3, 120));
    std::puts("Feature policy tests passed");
#ifdef YUKARI_FEATURE_JNI_TEST
    fake_jni::run();
#endif
}
