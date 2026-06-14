#include "binder_hook.h"
#include "logger.h"
#include "service_match.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <linux/android/binder.h>
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

namespace {
using IoctlFn = int (*)(int, unsigned long, void *);
IoctlFn g_original_ioctl = nullptr;
bool g_hook_installed = false;
bool g_enhanced_mode = false;
thread_local int g_pending_service_manager_replies = 0;
thread_local std::deque<std::vector<uint8_t>> g_enhanced_reply_copies;

struct binder_transaction_data_sg_local {
    binder_transaction_data transaction_data;
    binder_size_t buffers_size;
};

struct ElfMappingId {
    dev_t dev = 0;
    ino_t inode = 0;
};

struct MemoryRangeProt {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    int prot = 0;
};

size_t align4(size_t value) {
    return (value + 3u) & ~static_cast<size_t>(3u);
}

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

int parse_maps_prot(const char *perms) {
    int prot = 0;
    if (perms[0] == 'r') prot |= PROT_READ;
    if (perms[1] == 'w') prot |= PROT_WRITE;
    if (perms[2] == 'x') prot |= PROT_EXEC;
    return prot;
}

bool collect_range_protections(void *addr, size_t size, std::vector<MemoryRangeProt> &ranges) {
    ranges.clear();
    if (!addr || size == 0) return false;

    const uintptr_t target_begin = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t target_end = target_begin + size;
    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return false;

    char line[512]{};
    while (std::fgets(line, sizeof(line), fp)) {
        unsigned long long begin = 0;
        unsigned long long end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, perms) != 3) continue;
        if (end <= target_begin || begin >= target_end) continue;

        MemoryRangeProt range{};
        range.begin = static_cast<uintptr_t>(begin) > target_begin ? static_cast<uintptr_t>(begin) : target_begin;
        range.end = static_cast<uintptr_t>(end) < target_end ? static_cast<uintptr_t>(end) : target_end;
        range.prot = parse_maps_prot(perms);
        ranges.push_back(range);
    }
    std::fclose(fp);

    if (ranges.empty()) return false;
    uintptr_t covered = target_begin;
    for (const auto &range : ranges) {
        if (range.begin > covered) return false;
        if (range.end > covered) covered = range.end;
    }
    return covered >= target_end;
}

bool range_is_writable(const std::vector<MemoryRangeProt> &ranges) {
    for (const auto &range : ranges) {
        if ((range.prot & PROT_WRITE) == 0) return false;
    }
    return !ranges.empty();
}

bool is_probable_parcel_string16(const uint8_t *parcel, size_t size, size_t off, int32_t len) {
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
    if ((txn.flags & TF_ONE_WAY) == 0) ++g_pending_service_manager_replies;

    auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
    scrub_service_strings(parcel, static_cast<size_t>(txn.data_size), "request");
}

bool filter_reply_by_buffer_swap(binder_transaction_data *txn) {
    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return false;

    const size_t data_size = static_cast<size_t>(txn->data_size);
    const size_t offsets_size = static_cast<size_t>(txn->offsets_size);
    const size_t offsets_off = align8(data_size);
    const size_t copy_size = offsets_off + offsets_size;

    g_enhanced_reply_copies.emplace_back(copy_size);
    auto &copy = g_enhanced_reply_copies.back();
    std::memcpy(copy.data(), reinterpret_cast<const void *>(txn->data.ptr.buffer), data_size);
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        std::memcpy(copy.data() + offsets_off, reinterpret_cast<const void *>(txn->data.ptr.offsets), offsets_size);
    }

    const int hits = scrub_service_strings(copy.data(), data_size, "reply-enhanced");
    if (hits == 0) {
        g_enhanced_reply_copies.pop_back();
        return false;
    }

    txn->data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(copy.data());
    if (offsets_size > 0 && txn->data.ptr.offsets != 0) {
        txn->data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(copy.data() + offsets_off);
    }
    yukari_log_info("enhanced mode swapped reply buffer after filtering %d item(s)", hits);
    return true;
}

void process_reply_transaction(binder_transaction_data *txn) {
    if (g_pending_service_manager_replies <= 0) return;
    --g_pending_service_manager_replies;
    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return;

    auto *parcel = reinterpret_cast<uint8_t *>(txn->data.ptr.buffer);
    const size_t data_size = static_cast<size_t>(txn->data_size);
    std::vector<MemoryRangeProt> ranges;
    if (!collect_range_protections(parcel, data_size, ranges)) {
        yukari_log_error("failed to read reply buffer permissions");
        return;
    }
    if (!range_is_writable(ranges)) {
        if (g_enhanced_mode && filter_reply_by_buffer_swap(txn)) return;
        yukari_log_info("reply buffer is not writable; skipping reply filtering");
        return;
    }

    const int hits = scrub_service_strings(parcel, data_size, "reply");
    if (hits > 0) yukari_log_info("filtered %d service-manager reply item(s)", hits);
}

void forget_pending_reply() {
    if (g_pending_service_manager_replies > 0) --g_pending_service_manager_replies;
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
            case BR_DEAD_REPLY:
            case BR_FAILED_REPLY:
                forget_pending_reply();
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

int hooked_ioctl(int fd, unsigned long request, void *arg) {
    if (request != BINDER_WRITE_READ || !arg) {
        return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    }

    g_enhanced_reply_copies.clear();
    auto *bwr = reinterpret_cast<binder_write_read *>(arg);
    process_binder_write_buffer(bwr);
    const int ret = g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    if (ret == 0) process_binder_read_buffer(bwr);
    return ret;
}

bool mapping_seen(const std::vector<ElfMappingId> &mappings, dev_t dev, ino_t inode) {
    for (const auto &mapping : mappings) {
        if (mapping.dev == dev && mapping.inode == inode) return true;
    }
    return false;
}

std::vector<ElfMappingId> find_libbinder_mappings() {
    std::vector<ElfMappingId> mappings;
    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return mappings;

    char line[1024]{};
    while (std::fgets(line, sizeof(line), fp)) {
        unsigned long long begin = 0;
        unsigned long long end = 0;
        unsigned long long offset = 0;
        unsigned int major_id = 0;
        unsigned int minor_id = 0;
        unsigned long long inode = 0;
        char perms[5]{};
        char path[512]{};
        const int fields = std::sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %511s", &begin, &end,
                                       perms, &offset, &major_id, &minor_id, &inode, path);
        if (fields < 8 || inode == 0) continue;
        const std::string pathname = path;
        if (pathname.find("/libbinder.so") == std::string::npos) continue;

        const dev_t dev = makedev(major_id, minor_id);
        const auto ino = static_cast<ino_t>(inode);
        if (!mapping_seen(mappings, dev, ino)) mappings.push_back({dev, ino});
    }
    std::fclose(fp);
    return mappings;
}
} // namespace

void install_binder_hooks(zygisk::Api *api, bool enhanced_mode) {
    if (g_hook_installed) return;
    if (!api) {
        yukari_log_error("zygisk api is null; cannot install binder hook");
        return;
    }

    g_enhanced_mode = enhanced_mode;
    const auto mappings = find_libbinder_mappings();
    if (mappings.empty()) {
        yukari_log_error("libbinder mapping not found; cannot install ioctl hook");
        return;
    }

    for (const auto &mapping : mappings) {
        api->pltHookRegister(mapping.dev, mapping.inode, "ioctl", reinterpret_cast<void *>(hooked_ioctl),
                             reinterpret_cast<void **>(&g_original_ioctl));
    }
    if (!api->pltHookCommit() || !g_original_ioctl) {
        yukari_log_error("zygisk plt ioctl hook failed");
        return;
    }

    g_hook_installed = true;
    yukari_log_info("zygisk plt ioctl hook installed for %zu libbinder mapping(s), enhanced=%d", mappings.size(),
                    g_enhanced_mode ? 1 : 0);
}
