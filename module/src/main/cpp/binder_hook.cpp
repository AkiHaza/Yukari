#include "binder_hook.h"
#include "logger.h"
#include "service_match.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/android/binder.h>
#include <pthread.h>
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

// Max reply buffer size for swap. ServiceManager list/debug replies can be
// larger than 32KB on devices with many registered services.
#define MAX_REPLY_BUF (256 * 1024)

namespace {
using IoctlFn = int (*)(int, unsigned long, void *);
IoctlFn g_original_ioctl = nullptr;
bool g_hook_installed = false;

// Thread local: whether the current thread has a pending synchronous
// transaction to handle 0 (servicemanager).
thread_local bool g_pending_sm_reply = false;

// Allocate only for replies that actually need filtering, growing to the
// smallest power-of-two capacity that fits. Reuse until the thread exits.
thread_local unsigned char *g_swap_buf = nullptr;
thread_local size_t g_swap_capacity = 0;

// pthread_key destructor: automatically frees the buffer when thread exits
pthread_key_t g_buf_key;
pthread_once_t g_buf_key_once = PTHREAD_ONCE_INIT;
bool g_buf_key_valid = false;

void buf_destructor(void *buf) {
    if (buf) {
        free(buf);
    }
}

void buf_key_init() {
    g_buf_key_valid = pthread_key_create(&g_buf_key, buf_destructor) == 0;
}

// Returns a thread-local buffer, allocating it if necessary.
// Registered with pthread_key so it's freed on thread exit.
unsigned char *get_swap_buf(size_t required) {
    if (required <= g_swap_capacity) return g_swap_buf;
    pthread_once(&g_buf_key_once, buf_key_init);
    if (!g_buf_key_valid) return nullptr;

    size_t capacity = g_swap_capacity ? g_swap_capacity : 256;
    while (capacity < required) capacity *= 2;
    auto *buf = static_cast<unsigned char *>(malloc(capacity));
    if (!buf) return nullptr;
    if (pthread_setspecific(g_buf_key, buf) != 0) {
        free(buf);
        return nullptr;
    }
    free(g_swap_buf);
    g_swap_buf = buf;
    g_swap_capacity = capacity;
    return g_swap_buf;
}

// Track the active swap buffer address to intercept BC_FREE_BUFFER
thread_local void *g_active_swap_ptr = nullptr;
thread_local binder_uintptr_t g_original_reply_buffer = 0;

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

bool matches_string16(const uint8_t *parcel, size_t size, size_t off, int32_t &len) {
    if (off + sizeof(int32_t) > size) return false;
    std::memcpy(&len, parcel + off, sizeof(len));
    if (!is_parcel_str16(parcel, size, off, len)) return false;
    const auto *chars = reinterpret_cast<const char16_t *>(parcel + off + sizeof(int32_t));
    return hide_service(std::u16string_view(chars, static_cast<size_t>(len)));
}

// Read-only preflight: clean replies keep their original Binder buffer and
// require neither a userspace copy nor a thread-local allocation.
size_t find_hidden_string16(const uint8_t *parcel, size_t size) {
    for (size_t off = 0; off + sizeof(int32_t) <= size; off += sizeof(uint32_t)) {
        int32_t len = 0;
        if (matches_string16(parcel, size, off, len)) return off;
    }
    return size;
}

// Scan a Parcel for String16 values containing ROM keywords. The same parser
// is used for requests and replies: request layouts have gained extra header
// fields across Android releases, while the string representation is stable.
void scan_sm_parcel_for_strings(uint8_t *parcel, size_t size, size_t start = 0) {
    if (!parcel || size < sizeof(int32_t)) return;
    int total_hits = 0;

    for (size_t off = start; off + sizeof(int32_t) <= size; off += sizeof(uint32_t)) {
        int32_t len = 0;
        if (!matches_string16(parcel, size, off, len)) continue;
        auto *chars = reinterpret_cast<char16_t *>(parcel + off + sizeof(int32_t));
        char value[513];
        for (int32_t i = 0; i < len; ++i) value[i] = static_cast<char>(chars[i]);
        value[len] = '\0';
        overwrite_utf16(chars, len);
        ++total_hits;
        log_info("scrubbed service string in SM parcel: %s", value);
    }

    if (total_hits > 0) {
        log_info("scan_sm_parcel: filtered %d service string(s)", total_hits);
    }
}

void process_transaction(const binder_transaction_data &txn) {
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    if (txn.target.handle != 0) return;
    if (txn.flags & TF_ONE_WAY) return;

    // Rewrite matching names before the transaction reaches ServiceManager.
    // Do not depend on a particular IServiceManager transaction number or
    // Parcel header size: both vary between platform releases, and newer
    // lookup APIs (for example waitForService/isDeclared) would otherwise
    // leave profile visible to callers.
    auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
    scan_sm_parcel_for_strings(parcel, static_cast<size_t>(txn.data_size));

    g_pending_sm_reply = true;
}

// Copy reply to swap buffer, filter it, and replace the pointer.
bool swap_reply_buffer(binder_transaction_data *txn) {
    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return false;

    const size_t data_size = static_cast<size_t>(txn->data_size);
    const size_t offsets_size = static_cast<size_t>(txn->offsets_size);
    if (data_size > MAX_REPLY_BUF || offsets_size > MAX_REPLY_BUF - align8(data_size)) {
        log_info("swap: reply too large (%zu data, %zu offsets), skip", data_size, offsets_size);
        return false;
    }
    if (g_active_swap_ptr != nullptr) {
        log_info("swap: buffer still in use, skip");
        return false;
    }

    const auto *original = reinterpret_cast<const uint8_t *>(txn->data.ptr.buffer);
    const size_t first_hit = find_hidden_string16(original, data_size);
    if (first_hit == data_size) return false;

    const size_t offsets_off = align8(data_size);
    const size_t copy_size = offsets_off + offsets_size;
    unsigned char *buf = get_swap_buf(copy_size);
    if (!buf) {
        log_error("swap: failed to allocate swap buffer");
        return false;
    }

    std::memcpy(buf, original, data_size);
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        std::memcpy(buf + offsets_off, reinterpret_cast<const void *>(txn->data.ptr.offsets), offsets_size);
    }

    scan_sm_parcel_for_strings(buf, data_size, first_hit);

    g_original_reply_buffer = txn->data.ptr.buffer;
    txn->data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(buf);
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        txn->data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(buf + offsets_off);
    }

    g_active_swap_ptr = buf;
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
                // The kernel owns the original reply buffer, not the swap.
                // Return its actual address so the driver can release it.
                *buf_ptr = g_original_reply_buffer;
                g_original_reply_buffer = 0;
                log_info("swap: intercepted BC_FREE_BUFFER for swap buffer");
            }
            ptr += sizeof(binder_uintptr_t);
        } else {
            return;
        }
    }
}

void process_read(binder_write_read *bwr) {
    if (!bwr || !bwr->read_buffer || !bwr->read_consumed) return;
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

int hook_ioctl(int fd, unsigned long request, void *arg) {
    if (request != BINDER_WRITE_READ || !arg) {
        return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    }

    auto *bwr = reinterpret_cast<binder_write_read *>(arg);
    process_write(bwr);
    const int ret = g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    if (ret == 0 && g_pending_sm_reply) process_read(bwr);
    return ret;
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
        if (!std::strstr(path, "/libbinder.so")) continue;

        const dev_t dev = makedev(major_id, minor_id);
        const auto ino = static_cast<ino_t>(inode);
        if (!seen(mappings, dev, ino)) mappings.push_back({dev, ino});
    }
    std::fclose(fp);
    return mappings;
}
} // namespace

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

    for (const auto &mapping : mappings) {
        api->pltHookRegister(mapping.dev, mapping.inode, "ioctl", reinterpret_cast<void *>(hook_ioctl),
                             reinterpret_cast<void **>(&g_original_ioctl));
    }
    if (!api->pltHookCommit() || !g_original_ioctl) {
        log_error("zygisk plt ioctl hook failed");
        return;
    }

    g_hook_installed = true;
    log_info("zygisk plt ioctl hook installed for %zu libbinder mapping(s)", mappings.size());
}
