#include "binder_hook.h"
#include "logger.h"
#include "service_match.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <linux/android/binder.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#ifndef BC_TRANSACTION_SG
#define BC_TRANSACTION_SG _IOW('c', 17, struct binder_transaction_data_sg)
#endif

namespace {
using IoctlFn = int (*)(int, unsigned long, void *);
IoctlFn g_original_ioctl = nullptr;
bool g_hook_installed = false;
thread_local int g_pending_service_manager_replies = 0;
thread_local std::deque<std::vector<uint8_t>> g_reply_copies;

struct binder_transaction_data_sg_local {
    binder_transaction_data transaction_data;
    binder_size_t buffers_size;
};

size_t align8(size_t value) {
    return (value + 7u) & ~static_cast<size_t>(7u);
}

std::string utf16_to_ascii(const char16_t *chars, int32_t len) {
    std::string ascii;
    if (!chars || len <= 0 || len > 512) return ascii;
    ascii.reserve(static_cast<size_t>(len));
    for (int32_t i = 0; i < len; ++i) {
        const char16_t c = chars[i];
        ascii.push_back(c <= 0x7f ? static_cast<char>(c) : '?');
    }
    return ascii;
}

bool is_probable_parcel_string16(const uint8_t *parcel, size_t size, size_t off, int32_t len) {
    if (!parcel || (off & 0x3u) != 0 || len <= 0 || len > 512) return false;
    const size_t str_off = off + sizeof(int32_t);
    const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
    const size_t terminator_off = str_off + bytes;
    const size_t next_off = ((terminator_off + sizeof(char16_t) + 3u) & ~static_cast<size_t>(3u));
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

int scrub_service_strings(uint8_t *parcel, size_t size, const char *source) {
    if (!parcel || size < sizeof(int32_t)) return 0;
    int hits = 0;
    for (size_t off = 0; off + sizeof(int32_t) < size; off += sizeof(uint32_t)) {
        int32_t len = 0;
        std::memcpy(&len, parcel + off, sizeof(len));
        if (!is_probable_parcel_string16(parcel, size, off, len)) continue;

        auto *chars = reinterpret_cast<char16_t *>(parcel + off + sizeof(int32_t));
        const std::string value = utf16_to_ascii(chars, len);
        if (should_hide_service(value)) {
            overwrite_utf16(chars, len);
            ++hits;
            yukari_log_info("scrubbed %s service string: %s", source, value.c_str());
        }
    }
    return hits;
}

void process_service_manager_transaction(const binder_transaction_data &txn) {
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    if (txn.target.handle != 0) return;
    ++g_pending_service_manager_replies;

    auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
    scrub_service_strings(parcel, static_cast<size_t>(txn.data_size), "request");
}

void process_reply_transaction(binder_transaction_data *txn) {
    if (!txn || g_pending_service_manager_replies <= 0) return;
    --g_pending_service_manager_replies;
    if (txn->data_size == 0 || txn->data.ptr.buffer == 0) return;

    const size_t data_size = static_cast<size_t>(txn->data_size);
    const size_t offsets_size = static_cast<size_t>(txn->offsets_size);
    const size_t offsets_off = align8(data_size);
    const size_t copy_size = offsets_off + offsets_size;

    g_reply_copies.emplace_back(copy_size);
    auto &copy = g_reply_copies.back();
    std::memcpy(copy.data(), reinterpret_cast<const void *>(txn->data.ptr.buffer), data_size);
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        std::memcpy(copy.data() + offsets_off, reinterpret_cast<const void *>(txn->data.ptr.offsets), offsets_size);
    }

    const int hits = scrub_service_strings(copy.data(), data_size, "reply");
    if (hits == 0) {
        g_reply_copies.pop_back();
        return;
    }

    txn->data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(copy.data());
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        txn->data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(copy.data() + offsets_off);
    }
    yukari_log_info("filtered %d service-manager reply item(s)", hits);
}

void process_binder_write_buffer(binder_write_read *bwr) {
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
            if (cmd == BC_TRANSACTION) process_service_manager_transaction(*txn);
            ptr += sizeof(binder_transaction_data);
        } else if (cmd == BC_TRANSACTION_SG) {
            if (ptr + sizeof(binder_transaction_data_sg_local) > end) return;
            auto *txn = reinterpret_cast<binder_transaction_data_sg_local *>(ptr);
            process_service_manager_transaction(txn->transaction_data);
            ptr += sizeof(binder_transaction_data_sg_local);
        } else {
            return;
        }
    }
}

void process_binder_read_buffer(binder_write_read *bwr) {
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
                process_reply_transaction(txn);
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_TRANSACTION: {
                if (ptr + sizeof(binder_transaction_data) > end) return;
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_NOOP:
            case BR_TRANSACTION_COMPLETE:
            case BR_DEAD_REPLY:
            case BR_FAILED_REPLY:
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

int hooked_ioctl(int fd, unsigned long request, void *arg) {
    if (request != BINDER_WRITE_READ || !arg) {
        return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    }

    g_reply_copies.clear();
    auto *bwr = reinterpret_cast<binder_write_read *>(arg);
    process_binder_write_buffer(bwr);
    const int ret = g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    if (ret == 0) process_binder_read_buffer(bwr);
    return ret;
}
} // namespace

void install_binder_hooks(zygisk::Api *api) {
    if (g_hook_installed) return;
    if (!api) {
        yukari_log_error("zygisk api is null; cannot install binder hook");
        return;
    }

    api->pltHookRegister(".*libbinder.*\\.so$", "ioctl", reinterpret_cast<void *>(hooked_ioctl),
                         reinterpret_cast<void **>(&g_original_ioctl));
    if (!api->pltHookCommit() || !g_original_ioctl) {
        yukari_log_error("zygisk plt ioctl hook failed");
        return;
    }

    g_hook_installed = true;
    yukari_log_info("zygisk plt ioctl hook installed at %p", reinterpret_cast<void *>(g_original_ioctl));
}
