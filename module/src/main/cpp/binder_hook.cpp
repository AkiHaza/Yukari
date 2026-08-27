#include "binder_hook.h"
#include "logger.h"
#include "service_cache.h"
#include "service_match.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/android/binder.h>
#include <pthread.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

#ifndef BC_TRANSACTION_SG
#define BC_TRANSACTION_SG _IOW('c', 17, struct binder_transaction_data_sg)
#endif

#ifndef TF_ONE_WAY
#define TF_ONE_WAY 0x01
#endif

#ifndef BC_FREE_BUFFER
#define BC_FREE_BUFFER _IOW('c', 3, binder_uintptr_t)
#endif

// Max reply buffer size for swap.  Keep the historical 256 KiB ceiling so
// large OEM service/debug enumerations are still filtered; allocation remains
// lazy and only occurs when a matching reply is received.
#define MAX_REPLY_BUF (256 * 1024)

namespace {
using IoctlFn = int (*)(int, unsigned long, void *);
IoctlFn g_original_ioctl = nullptr;
bool g_hook_installed = false;

using TransactNativeFn = jboolean (*)(JNIEnv *, jobject, jint, jobject, jobject, jint);
TransactNativeFn g_original_transact_native = nullptr;
bool g_jni_hook_installed = false;

constexpr const char *kIServiceManagerDescriptor = "android.os.IServiceManager";

// BinderProxy's native method is stable since API 26.  Transaction numbers
// are part of IServiceManager.aidl and have remained stable as well.
// IServiceManager grew methods over time.  Keep both the pre-Android-12 and
// current transaction numbers; the interface token check prevents matching
// unrelated Binder interfaces that happen to use the same code.
constexpr jint kSvcListServicesLegacy = 4;
constexpr jint kSvcListServices = 6;
constexpr jint kSvcGetServiceDebugInfoLegacy = 10;
constexpr jint kSvcGetServiceDebugInfo = 16;

struct ParcelMethods {
    jclass cls = nullptr;
    jmethodID data_position = nullptr;
    jmethodID set_data_position = nullptr;
    jmethodID read_string = nullptr;
    jmethodID write_string = nullptr;
    jmethodID read_int = nullptr;
    jmethodID write_int = nullptr;
    jmethodID create_string_array = nullptr;
    jmethodID write_string_array = nullptr;
};

ParcelMethods g_parcel_methods;
pthread_mutex_t g_parcel_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> g_parcel_methods_ready{false};

// Thread local: whether the current thread has a pending synchronous
// transaction to handle 0 (servicemanager).
thread_local bool g_pending_sm_reply = false;

// --- Lazy Buffer Management (Optimized Memory Usage) ---
// Instead of allocating 256KB per thread upfront, we allocate on demand
// and clean up automatically when the thread exits using pthread_key.
thread_local unsigned char *g_swap_buf = nullptr; // Allocated on first use

// pthread_key destructor: automatically frees the buffer when thread exits
pthread_key_t g_buf_key;
pthread_once_t g_buf_key_once = PTHREAD_ONCE_INIT;

void buf_destructor(void *buf) {
    if (buf) {
        free(buf);
    }
}

void buf_key_init() {
    pthread_key_create(&g_buf_key, buf_destructor);
}

// Returns a thread-local buffer, allocating it if necessary.
// Registered with pthread_key so it's freed on thread exit.
unsigned char *get_swap_buf() {
    if (!g_swap_buf) {
        pthread_once(&g_buf_key_once, buf_key_init);
        g_swap_buf = static_cast<unsigned char *>(malloc(MAX_REPLY_BUF));
        if (g_swap_buf) {
            pthread_setspecific(g_buf_key, g_swap_buf);
        }
    }
    return g_swap_buf;
}

// Track the active swap buffer address to intercept BC_FREE_BUFFER
thread_local void *g_active_swap_ptr = nullptr;
// The kernel-owned reply mapping must still be released.  Keep its pointer
// while the userspace replacement is visible to Java, then restore it in the
// BC_FREE_BUFFER command sent back to the driver.
thread_local binder_uintptr_t g_original_reply_ptr = 0;

struct binder_transaction_data_sg_local {
    binder_transaction_data transaction_data;
    binder_size_t buffers_size;
};

struct ElfMappingId {
    dev_t dev = 0;
    ino_t inode = 0;
};

size_t align4(size_t value) { return (value + 3u) & ~static_cast<size_t>(3u); }
size_t align8(size_t value) { return (value + 7u) & ~static_cast<size_t>(7u); }

void clear_jni_exception(JNIEnv *env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

bool init_parcel_methods(JNIEnv *env) {
    if (!env) return false;
    if (g_parcel_methods_ready.load(std::memory_order_acquire)) return true;

    pthread_mutex_lock(&g_parcel_mutex);
    if (g_parcel_methods_ready.load(std::memory_order_relaxed)) {
        pthread_mutex_unlock(&g_parcel_mutex);
        return true;
    }

    jclass local = env->FindClass("android/os/Parcel");
    if (!local) {
        clear_jni_exception(env);
        pthread_mutex_unlock(&g_parcel_mutex);
        return false;
    }

    ParcelMethods methods;
    methods.cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!methods.cls) {
        pthread_mutex_unlock(&g_parcel_mutex);
        return false;
    }

    methods.data_position = env->GetMethodID(methods.cls, "dataPosition", "()I");
    methods.set_data_position = env->GetMethodID(methods.cls, "setDataPosition", "(I)V");
    methods.read_string = env->GetMethodID(methods.cls, "readString", "()Ljava/lang/String;");
    methods.write_string = env->GetMethodID(methods.cls, "writeString", "(Ljava/lang/String;)V");
    methods.read_int = env->GetMethodID(methods.cls, "readInt", "()I");
    methods.write_int = env->GetMethodID(methods.cls, "writeInt", "(I)V");
    methods.create_string_array = env->GetMethodID(methods.cls, "createStringArray", "()[Ljava/lang/String;");
    methods.write_string_array = env->GetMethodID(methods.cls, "writeStringArray", "([Ljava/lang/String;)V");
    if (env->ExceptionCheck() || !methods.data_position ||
        !methods.set_data_position || !methods.read_string || !methods.write_string ||
        !methods.read_int || !methods.write_int || !methods.create_string_array || !methods.write_string_array) {
        clear_jni_exception(env);
        env->DeleteGlobalRef(methods.cls);
        pthread_mutex_unlock(&g_parcel_mutex);
        return false;
    }

    g_parcel_methods = methods;
    g_parcel_methods_ready.store(true, std::memory_order_release);
    pthread_mutex_unlock(&g_parcel_mutex);
    return true;
}

std::string jstring_ascii(JNIEnv *env, jstring value) {
    if (!env || !value) return {};
    const jsize length = env->GetStringLength(value);
    if (length <= 0 || length > 512) return {};
    const jchar *chars = env->GetStringChars(value, nullptr);
    if (!chars) return {};
    std::string out;
    out.reserve(static_cast<size_t>(length));
    for (jsize i = 0; i < length; ++i) {
        const jchar c = chars[i];
        out.push_back(c <= 0x7f ? static_cast<char>(c) : '?');
    }
    env->ReleaseStringChars(value, chars);
    return out;
}

jstring replacement_for(JNIEnv *env, jstring value) {
    if (!env || !value) return nullptr;
    const jsize length = env->GetStringLength(value);
    if (length <= 0 || length > 512) return nullptr;
    std::u16string replacement(static_cast<size_t>(length), u'_');
    return env->NewString(reinterpret_cast<const jchar *>(replacement.data()), length);
}

bool parcel_has_service_manager(JNIEnv *env, jobject parcel) {
    if (!env || !parcel) return false;
    const jint original_position = env->CallIntMethod(parcel, g_parcel_methods.data_position);
    if (env->ExceptionCheck() || original_position < 0) {
        clear_jni_exception(env);
        return false;
    }
    // writeInterfaceToken prepends a kernel request header.  Its size changed
    // across releases: one int (O/P), two ints (Q), three ints (R), and four
    // ints on newer builds.  RPC parcels have no header and use offset zero.
    constexpr jint kCandidateOffsets[] = {0, 4, 8, 12, 16};
    for (jint offset : kCandidateOffsets) {
        env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, offset);
        jstring descriptor = static_cast<jstring>(env->CallObjectMethod(parcel, g_parcel_methods.read_string));
        if (env->ExceptionCheck()) {
            clear_jni_exception(env);
            continue;
        }
        const std::string value = jstring_ascii(env, descriptor);
        if (descriptor) env->DeleteLocalRef(descriptor);
        if (value == kIServiceManagerDescriptor) {
            env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, original_position);
            clear_jni_exception(env);
            return true;
        }
    }
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, original_position);
    clear_jni_exception(env);
    return false;
}

void filter_list_reply(JNIEnv *env, jobject parcel) {
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
    const jint exception = env->CallIntMethod(parcel, g_parcel_methods.read_int);
    if (env->ExceptionCheck() || exception != 0) {
        clear_jni_exception(env);
        env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
        clear_jni_exception(env);
        return;
    }
    auto values = static_cast<jobjectArray>(env->CallObjectMethod(parcel, g_parcel_methods.create_string_array));
    if (env->ExceptionCheck() || !values) {
        clear_jni_exception(env);
        env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
        clear_jni_exception(env);
        return;
    }
    const jsize count = env->GetArrayLength(values);
    for (jsize i = 0; i < count; ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        const std::string service = jstring_ascii(env, value);
        if (value && !service.empty() && hide_service(service)) {
            jstring replacement = replacement_for(env, value);
            if (replacement) {
                env->SetObjectArrayElement(values, i, replacement);
                env->DeleteLocalRef(replacement);
            }
        }
        if (value) env->DeleteLocalRef(value);
    }
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
    env->CallVoidMethod(parcel, g_parcel_methods.write_int, exception);
    env->CallVoidMethod(parcel, g_parcel_methods.write_string_array, values);
    // BinderProxy returns reply Parcels positioned at zero.  Leave the
    // rewritten Parcel in the same state so the generated IServiceManager
    // proxy can call readException()/createStringArray() normally.
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
    clear_jni_exception(env);
    env->DeleteLocalRef(values);
}

void filter_debug_info_reply(JNIEnv *env, jobject parcel) {
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
    const jint exception = env->CallIntMethod(parcel, g_parcel_methods.read_int);
    if (env->ExceptionCheck() || exception != 0) {
        clear_jni_exception(env);
        env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
        clear_jni_exception(env);
        return;
    }
    const jint count = env->CallIntMethod(parcel, g_parcel_methods.read_int);
    if (env->ExceptionCheck() || count < 0 || count > 4096) {
        clear_jni_exception(env);
        env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
        clear_jni_exception(env);
        return;
    }
    for (jint i = 0; i < count; ++i) {
        const jint present = env->CallIntMethod(parcel, g_parcel_methods.read_int);
        if (present == 0) continue;
        if (present != 1 || env->ExceptionCheck()) {
            clear_jni_exception(env);
            break;
        }
        const jint start = env->CallIntMethod(parcel, g_parcel_methods.data_position);
        auto name = static_cast<jstring>(env->CallObjectMethod(parcel, g_parcel_methods.read_string));
        if (env->ExceptionCheck() || !name) {
            clear_jni_exception(env);
            break;
        }
        const jint end = env->CallIntMethod(parcel, g_parcel_methods.data_position);
        const std::string service = jstring_ascii(env, name);
        if (!service.empty() && hide_service(service)) {
            jstring replacement = replacement_for(env, name);
            if (replacement) {
                env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, start);
                env->CallVoidMethod(parcel, g_parcel_methods.write_string, replacement);
                env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, end);
                env->DeleteLocalRef(replacement);
            }
        }
        env->DeleteLocalRef(name);
        // ServiceDebugInfo contains one debugPid int after the name.
        (void)env->CallIntMethod(parcel, g_parcel_methods.read_int);
        if (env->ExceptionCheck()) {
            clear_jni_exception(env);
            break;
        }
    }
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
    env->CallVoidMethod(parcel, g_parcel_methods.write_int, exception);
    // The parser only changes String16 payloads in place; no need to rewrite
    // the count or the surrounding parcel data.
    env->CallVoidMethod(parcel, g_parcel_methods.set_data_position, 0);
    clear_jni_exception(env);
}

jboolean hook_transact_native(JNIEnv *env, jobject thiz, jint code, jobject data_obj, jobject reply_obj,
                              jint flags);

std::string to_ascii(const char16_t *chars, int32_t len) {
    std::string ascii;
    if (!chars || len <= 0 || len > 512) return ascii;
    ascii.reserve(static_cast<size_t>(len));
    for (int32_t i = 0; i < len; ++i) {
        ascii.push_back(chars[i] <= 0x7f ? static_cast<char>(chars[i]) : '?');
    }
    return ascii;
}

bool is_parcel_str16(const uint8_t *parcel, size_t size, size_t off, int32_t len) {
    if (!parcel || (off & 0x3u) != 0 || len <= 0 || len > 512) return false;
    const size_t str_off = off + sizeof(int32_t);
    const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
    const size_t terminator_off = str_off + bytes;
    const size_t next_off = align4(terminator_off + sizeof(char16_t));
    if (next_off > size) return false;

    char16_t terminator = 1;
    std::memcpy(&terminator, parcel + terminator_off, sizeof(terminator));
    if (terminator != 0) return false;

    for (int32_t i = 0; i < len; ++i) {
        char16_t c = 0;
        std::memcpy(&c, parcel + str_off + static_cast<size_t>(i) * sizeof(char16_t), sizeof(c));
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

void overwrite_utf16(char16_t *chars, int32_t len) {
    if (!chars || len <= 0) return;
    for (int32_t i = 0; i < len; ++i) chars[i] = u'_';
}

size_t process_string16(uint8_t *parcel, size_t size, size_t off, bool &hit) {
    if (off + sizeof(int32_t) > size) return 0;
    int32_t len = 0;
    std::memcpy(&len, parcel + off, sizeof(len));
    if (!is_parcel_str16(parcel, size, off, len)) return 0;

    const size_t str_off = off + sizeof(int32_t);
    const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
    const size_t terminator_off = str_off + bytes;
    const size_t next_off = align4(terminator_off + sizeof(char16_t));

    auto *chars = reinterpret_cast<char16_t *>(parcel + str_off);
    const std::string value = to_ascii(chars, len);
    if (hide_service(value)) {
        overwrite_utf16(chars, len);
        hit = true;
        log_info("scrubbed service string in SM reply: %s", value.c_str());
    }
    return next_off - off;
}

// Scan a ServiceManager reply buffer for String16 values containing
// ROM keywords. Covers listServices, getServiceDebugInfo, etc.
void scan_sm_reply_for_strings(uint8_t *parcel, size_t size) {
    if (!parcel || size < sizeof(int32_t)) return;
    int total_hits = 0;

    for (size_t off = 0; off + sizeof(int32_t) <= size; off += sizeof(uint32_t)) {
        bool hit = false;
        process_string16(parcel, size, off, hit);
        if (hit) ++total_hits;
    }

    if (total_hits > 0) {
        log_info("scan_sm_reply: filtered %d service string(s)", total_hits);
    }
}

void process_transaction(const binder_transaction_data &txn) {
    // Ignore unrelated transactions.  libbinder can batch one-way writes
    // with a synchronous call; they must not cancel the pending SM reply.
    if (txn.target.handle != 0 || (txn.flags & TF_ONE_WAY) != 0) return;
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) {
        g_pending_sm_reply = false;
        return;
    }

    // Do not rewrite getService/checkService requests.  Returning a null
    // binder for a framework lookup can make callers crash during startup.
    // Enumeration/debug replies are scrubbed below, which is the stable and
    // low-risk observation boundary.
    g_pending_sm_reply = txn.code == static_cast<uint32_t>(kSvcListServices) ||
                         txn.code == static_cast<uint32_t>(kSvcListServicesLegacy) ||
                         txn.code == static_cast<uint32_t>(kSvcGetServiceDebugInfo) ||
                         txn.code == static_cast<uint32_t>(kSvcGetServiceDebugInfoLegacy);
}

// Copy reply to swap buffer, filter it, and replace the pointer.
bool swap_reply_buffer(binder_transaction_data *txn) {
    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return false;

    const binder_uintptr_t original_buffer = txn->data.ptr.buffer;
    const size_t data_size = static_cast<size_t>(txn->data_size);
    const size_t offsets_size = static_cast<size_t>(txn->offsets_size);
    if ((data_size & 0x3u) != 0 || (offsets_size & (sizeof(binder_size_t) - 1u)) != 0) {
        log_info("swap: malformed alignment (data=%zu offsets=%zu), skip", data_size, offsets_size);
        return false;
    }
    if (offsets_size > 0 && txn->data.ptr.offsets == 0) {
        log_info("swap: offsets_size without offsets pointer, skip");
        return false;
    }
    if (data_size > MAX_REPLY_BUF) {
        log_info("swap: reply data too large (%zu), skip", data_size);
        return false;
    }
    const size_t offsets_off = align8(data_size);
    if (offsets_off > MAX_REPLY_BUF || offsets_size > MAX_REPLY_BUF - offsets_off) {
        log_info("swap: reply too large (data=%zu offsets=%zu), skip", data_size, offsets_size);
        return false;
    }
    const size_t copy_size = offsets_off + offsets_size;

    if (copy_size > MAX_REPLY_BUF) {
        log_info("swap: reply too large (%zu), skip", copy_size);
        return false;
    }
    if (g_active_swap_ptr != nullptr) {
        log_info("swap: buffer still in use, skip");
        return false;
    }

    unsigned char *buf = get_swap_buf();
    if (!buf) {
        log_error("swap: failed to allocate swap buffer");
        return false;
    }

    std::memcpy(buf, reinterpret_cast<const void *>(original_buffer), data_size);
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        std::memcpy(buf + offsets_off, reinterpret_cast<const void *>(txn->data.ptr.offsets), offsets_size);
    }

    scan_sm_reply_for_strings(buf, data_size);

    txn->data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(buf);
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        txn->data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(buf + offsets_off);
    }

    g_active_swap_ptr = buf;
    g_original_reply_ptr = original_buffer;
    log_info("swap: replaced reply buffer with lazy swap (%zu bytes)", copy_size);
    return true;
}

void process_reply(binder_transaction_data *txn) {
    if (!g_pending_sm_reply) return;
    g_pending_sm_reply = false;

    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return;

    swap_reply_buffer(txn);
}

void process_write(binder_write_read *bwr) {
    if (!bwr || !bwr->write_buffer || !bwr->write_size) return;
    auto *ptr = reinterpret_cast<uint8_t *>(bwr->write_buffer);
    auto *end = ptr + bwr->write_size;

    while (ptr + sizeof(uint32_t) <= end) {
        uint32_t cmd = 0;
        std::memcpy(&cmd, ptr, sizeof(cmd));
        ptr += sizeof(cmd);

        if (cmd == BC_TRANSACTION || cmd == BC_REPLY) {
            if (ptr + sizeof(binder_transaction_data) > end) return;
            auto *txn = reinterpret_cast<binder_transaction_data *>(ptr);
            if (cmd == BC_TRANSACTION) process_transaction(*txn);
            ptr += sizeof(binder_transaction_data);
        } else if (cmd == BC_TRANSACTION_SG) {
            if (ptr + sizeof(binder_transaction_data_sg_local) > end) return;
            auto *txn = reinterpret_cast<binder_transaction_data_sg_local *>(ptr);
            process_transaction(txn->transaction_data);
            ptr += sizeof(binder_transaction_data_sg_local);
        } else if (cmd == BC_FREE_BUFFER) {
            if (ptr + sizeof(binder_uintptr_t) > end) return;
            binder_uintptr_t *buf_ptr = reinterpret_cast<binder_uintptr_t *>(ptr);
            if (g_active_swap_ptr != nullptr &&
                *buf_ptr == reinterpret_cast<binder_uintptr_t>(g_active_swap_ptr)) {
                g_active_swap_ptr = nullptr;
                // Pass the original mmap pointer to the binder driver so it
                // can release the transaction buffer.  Supplying zero here
                // leaks one kernel buffer per filtered reply.
                *buf_ptr = g_original_reply_ptr;
                g_original_reply_ptr = 0;
                log_info("swap: intercepted BC_FREE_BUFFER for swap buffer");
            }
            ptr += sizeof(binder_uintptr_t);
        } else {
            return;
        }
    }
}

void process_read(binder_write_read *bwr) {
    if (!bwr || !bwr->read_buffer || !bwr->read_consumed || bwr->read_consumed > bwr->read_size) return;
    auto *ptr = reinterpret_cast<uint8_t *>(bwr->read_buffer);
    auto *end = ptr + bwr->read_consumed;

    while (ptr + sizeof(uint32_t) <= end) {
        uint32_t cmd = 0;
        std::memcpy(&cmd, ptr, sizeof(cmd));
        ptr += sizeof(cmd);

        switch (cmd) {
            case BR_REPLY: {
                if (ptr + sizeof(binder_transaction_data) > end) return;
                auto *txn = reinterpret_cast<binder_transaction_data *>(ptr);
                process_reply(txn);
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_TRANSACTION: {
                if (ptr + sizeof(binder_transaction_data) > end) return;
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_DEAD_REPLY:
            case BR_FAILED_REPLY:
                g_pending_sm_reply = false;
                break;
            case BR_NOOP:
            case BR_TRANSACTION_COMPLETE:
            case BR_FINISHED:
                break;
            case BR_DEAD_BINDER:
            case BR_CLEAR_DEATH_NOTIFICATION_DONE:
                if (ptr + sizeof(binder_uintptr_t) > end) return;
                ptr += sizeof(binder_uintptr_t);
                break;
            default:
                return;
        }
    }
}

jboolean hook_transact_native(JNIEnv *env, jobject thiz, jint code, jobject data_obj, jobject reply_obj,
                              jint flags) {
    (void)thiz;
    if (!g_original_transact_native) return JNI_FALSE;

    const bool is_list = code == kSvcListServices || code == kSvcListServicesLegacy;
    const bool is_debug = code == kSvcGetServiceDebugInfo || code == kSvcGetServiceDebugInfoLegacy;
    bool is_service_manager = false;
    if ((is_list || is_debug) && data_obj != nullptr && init_parcel_methods(env)) {
        // Only use the request to identify IServiceManager.  Rewriting a
        // direct getService/checkService name to force a null result can
        // crash framework code during application initialization.
        is_service_manager = parcel_has_service_manager(env, data_obj);
    }

    const jboolean result = g_original_transact_native(env, thiz, code, data_obj, reply_obj, flags);
    // Never swallow an exception raised by the real Binder implementation;
    // callers rely on RemoteException propagation semantics.
    if (result == JNI_FALSE || !is_service_manager || reply_obj == nullptr || env->ExceptionCheck()) {
        return result;
    }

    if (!init_parcel_methods(env)) return result;
    if (is_list) {
        filter_list_reply(env, reply_obj);
    } else if (is_debug) {
        filter_debug_info_reply(env, reply_obj);
    }
    // ServiceManager.getService() may repopulate sCache after our initial
    // specialization pass.  Re-run the small reflective sweep after each
    // ServiceManager transaction so hidden names do not become observable via
    // direct map inspection, without introducing a Java proxy class.
    if (is_list || is_debug) clear_cache(env);
    clear_jni_exception(env);
    return result;
}

int hook_ioctl(int fd, unsigned long request, void *arg) {
    if (request != BINDER_WRITE_READ || !arg) {
        return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    }

    auto *bwr = reinterpret_cast<binder_write_read *>(arg);
    process_write(bwr);
    const int ret = g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    if (ret == 0) process_read(bwr);
    return ret;
}

// A tiny RX-only trampoline makes the fallback PLT slot point at anonymous
// executable memory instead of the module .text mapping.  The trampoline
// tail-jumps to the real C++ callback, preserving all argument registers.
void *make_anonymous_thunk(void *target) {
    if (!target) return nullptr;
#if defined(__aarch64__)
    constexpr size_t kSize = 32;
    auto *code = static_cast<uint8_t *>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return nullptr;
    const uint32_t insn[] = {0x58000050u, 0xD61F0200u}; // ldr x16, #8; br x16
    std::memcpy(code, insn, sizeof(insn));
    std::memcpy(code + 8, &target, sizeof(target));
#elif defined(__x86_64__)
    constexpr size_t kSize = 32;
    auto *code = static_cast<uint8_t *>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return nullptr;
    // mov r11, imm64; jmp r11
    code[0] = 0x49;
    code[1] = 0xbb;
    std::memcpy(code + 2, &target, sizeof(target));
    code[10] = 0x41;
    code[11] = 0xff;
    code[12] = 0xe3;
#elif defined(__arm__)
    constexpr size_t kSize = 16;
    auto *code = static_cast<uint8_t *>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return nullptr;
    // ldr pc, [pc, #-4]; followed by the absolute target address.
    const uint32_t insn = 0xe51ff004u;
    std::memcpy(code, &insn, sizeof(insn));
    std::memcpy(code + 4, &target, sizeof(target));
#else
    return target;
#endif
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code + kSize));
    if (mprotect(code, kSize, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, kSize);
        return nullptr;
    }
    return code;
}

bool seen(const std::vector<ElfMappingId> &mappings, dev_t dev, ino_t inode) {
    for (const auto &mapping : mappings) {
        if (mapping.dev == dev && mapping.inode == inode) return true;
    }
    return false;
}

std::vector<ElfMappingId> find_mappings() {
    std::vector<ElfMappingId> mappings;
    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return mappings;

    char line[1024]{};
    while (std::fgets(line, sizeof(line), fp)) {
        unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
        unsigned int major_id = 0, minor_id = 0;
        char perms[5]{};
        char path[512]{};
        const int fields = std::sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %511s", &begin, &end,
                                       perms, &offset, &major_id, &minor_id, &inode, path);
        if (fields < 8 || inode == 0) continue;
        const std::string pathname = path;
        if (pathname.find("/libbinder.so") == std::string::npos) continue;

        const dev_t dev = makedev(major_id, minor_id);
        const auto ino = static_cast<ino_t>(inode);
        if (!seen(mappings, dev, ino)) mappings.push_back({dev, ino});
    }
    std::fclose(fp);
    return mappings;
}
} // namespace

bool install_jni_hook(JNIEnv *env, zygisk::Api *api) {
    if (g_jni_hook_installed) return true;
    if (!env || !api || !init_parcel_methods(env)) {
        log_error("JNI BinderProxy hook unavailable: Parcel methods not found");
        return false;
    }

    jclass binder_proxy = env->FindClass("android/os/BinderProxy");
    if (!binder_proxy) {
        clear_jni_exception(env);
        log_error("JNI BinderProxy hook unavailable: class not found");
        return false;
    }
    const jmethodID transact = env->GetMethodID(
        binder_proxy, "transactNative", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z");
    if (env->ExceptionCheck() || !transact) {
        clear_jni_exception(env);
        env->DeleteLocalRef(binder_proxy);
        log_error("JNI BinderProxy hook unavailable: transactNative signature changed");
        return false;
    }

    void *thunk = make_anonymous_thunk(reinterpret_cast<void *>(hook_transact_native));
    if (!thunk) thunk = reinterpret_cast<void *>(hook_transact_native);
    JNINativeMethod method{"transactNative", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", thunk};
    api->hookJniNativeMethods(env, "android/os/BinderProxy", &method, 1);
    env->DeleteLocalRef(binder_proxy);
    clear_jni_exception(env);

    // Zygisk stores the previous function pointer in fnPtr.  Refuse to
    // enable the hook if an older API implementation did not provide it;
    // install_hooks() then supplies the compatibility path.
    auto original = reinterpret_cast<TransactNativeFn>(method.fnPtr);
    if (!original || original == hook_transact_native || reinterpret_cast<void *>(original) == thunk) {
        log_error("JNI BinderProxy hook did not return original function");
        return false;
    }
    g_original_transact_native = original;
    g_jni_hook_installed = true;
    log_info("BinderProxy.transactNative hook installed (anonymous thunk)");
    return true;
}

void install_hooks(zygisk::Api *api) {
    if (g_hook_installed) return;
    if (!api) {
        log_error("zygisk api is null; cannot install binder hook");
        return;
    }

    const auto mappings = find_mappings();
    if (mappings.empty()) {
        log_error("libbinder mapping not found; cannot install ioctl hook");
        return;
    }

    void *thunk = make_anonymous_thunk(reinterpret_cast<void *>(hook_ioctl));
    if (!thunk) thunk = reinterpret_cast<void *>(hook_ioctl);
    for (const auto &mapping : mappings) {
        api->pltHookRegister(mapping.dev, mapping.inode, "ioctl", thunk,
                             reinterpret_cast<void **>(&g_original_ioctl));
    }
    if (!api->pltHookCommit() || !g_original_ioctl) {
        log_error("zygisk plt ioctl hook failed");
        return;
    }

    g_hook_installed = true;
    log_info("zygisk plt ioctl hook installed for %zu libbinder mapping(s)", mappings.size());
}
