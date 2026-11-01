#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace allocation_test {
thread_local bool enabled = false;
thread_local bool fail = false;
thread_local size_t calls = 0;
std::atomic<void *> exit_buffer{nullptr};
std::atomic<size_t> exit_buffer_frees{0};

void record() {
    if (enabled) ++calls;
}
}

extern "C" void *__real_malloc(size_t);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void __real_free(void *);

extern "C" void *__wrap_malloc(size_t size) {
    allocation_test::record();
    if (allocation_test::enabled && allocation_test::fail) return nullptr;
    return __real_malloc(size);
}

extern "C" void *__wrap_realloc(void *ptr, size_t size) {
    allocation_test::record();
    if (allocation_test::enabled && allocation_test::fail) return nullptr;
    return __real_realloc(ptr, size);
}

extern "C" void __wrap_free(void *ptr) {
    if (ptr && ptr == allocation_test::exit_buffer.load()) {
        ++allocation_test::exit_buffer_frees;
    }
    __real_free(ptr);
}

void *operator new(size_t size) {
    allocation_test::record();
    if (!(allocation_test::enabled && allocation_test::fail)) {
        if (void *ptr = __real_malloc(size ? size : 1)) return ptr;
    }
    throw std::bad_alloc();
}

void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *ptr) noexcept { __real_free(ptr); }
void operator delete[](void *ptr) noexcept { __real_free(ptr); }
void operator delete(void *ptr, size_t) noexcept { __real_free(ptr); }
void operator delete[](void *ptr, size_t) noexcept { __real_free(ptr); }

void log_info(const char *, ...) {}
void log_error(const char *, ...) {}

// Keep the real production parser, ioctl command handling and TLS storage in
// this translation unit. No Android runtime or Binder device is needed.
#include "../../module/src/main/cpp/binder_hook.cpp"

namespace {
[[noreturn]] void failure(const char *expr, int line) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    std::exit(1);
}
#define CHECK(expr) ((expr) ? static_cast<void>(0) : failure(#expr, __LINE__))

void put_string(std::vector<uint8_t> &parcel, size_t off, std::u16string_view value) {
    const auto len = static_cast<int32_t>(value.size());
    const size_t end = (off + 4 + (value.size() + 1) * 2 + 3) & ~size_t{3};
    if (parcel.size() < end) parcel.resize(end);
    std::memcpy(parcel.data() + off, &len, sizeof(len));
    std::memcpy(parcel.data() + off + 4, value.data(), value.size() * 2);
    const char16_t zero = 0;
    std::memcpy(parcel.data() + off + 4 + value.size() * 2, &zero, 2);
}

std::vector<uint8_t> parcel_for(std::u16string_view value, size_t prefix = 0) {
    std::vector<uint8_t> parcel(prefix, 0xff);
    put_string(parcel, prefix, value);
    return parcel;
}

// Frozen reference for the original ASCII conversion + lowercase + substring
// matcher. It intentionally does not call the production matching functions.
bool reference_hidden(std::string value) {
    for (char &c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
    }
    if (value == "profile") return true;
    for (const char *word : {"lineage", "crdroid", "aospa",
                             "pixelexperience", "omnirom", "protonaosp"}) {
        if (value.find(word) != std::string::npos) return true;
    }
    return false;
}

void reference_scan(std::vector<uint8_t> &parcel) {
    for (size_t off = 0; off + 4 <= parcel.size(); off += 4) {
        int32_t len;
        std::memcpy(&len, parcel.data() + off, 4);
        if (len <= 0 || len > 512) continue;
        const size_t str = off + 4;
        const size_t tail = str + static_cast<size_t>(len) * 2;
        if (((tail + 2 + 3) & ~size_t{3}) > parcel.size()) continue;
        char16_t zero;
        std::memcpy(&zero, parcel.data() + tail, 2);
        if (zero != 0) continue;
        std::string value;
        bool valid = true;
        for (int32_t i = 0; i < len; ++i) {
            char16_t c;
            std::memcpy(&c, parcel.data() + str + static_cast<size_t>(i) * 2, 2);
            if (c < 0x20 || c > 0x7e) { valid = false; break; }
            value.push_back(static_cast<char>(c));
        }
        if (!valid || !reference_hidden(value)) continue;
        const char16_t replacement = u'_';
        for (int32_t i = 0; i < len; ++i) {
            std::memcpy(parcel.data() + str + static_cast<size_t>(i) * 2, &replacement, 2);
        }
    }
}

void check_scan(std::vector<uint8_t> parcel, bool expected_change) {
    const auto original = parcel;
    auto expected = parcel;
    reference_scan(expected);
    allocation_test::calls = 0;
    allocation_test::enabled = true;
    scan_sm_parcel_for_strings(parcel.data(), parcel.size());
    allocation_test::enabled = false;
    CHECK(parcel == expected);
    CHECK((parcel != original) == expected_change);
    CHECK(allocation_test::calls == 0);
}

void test_matching_and_boundaries() {
    for (auto value : {u"LiNeAgE", u"vendor.CRDROID.service", u"AOSPA",
                       u"PixelExperience", u"OMNIROM", u"ProtonAOSP", u"PrOfIlE"}) {
        for (size_t prefix : {size_t{0}, size_t{4}, size_t{20}}) {
            check_scan(parcel_for(value, prefix), true);
        }
    }
    for (auto value : {u"", u"activity", u"xprofile", u"profilex",
                       u"vendor.profile.service", u"lineage\u0080", u"lineage\n"}) {
        check_scan(parcel_for(value), false);
    }
    std::u16string longest(512, u'x');
    longest.replace(250, 7, u"lineage");
    check_scan(parcel_for(longest), true);
    longest.push_back(u'x');
    check_scan(parcel_for(longest), false);
    auto valid = parcel_for(u"lineage");
    for (size_t size = 0; size < valid.size(); ++size) {
        check_scan(std::vector<uint8_t>(valid.begin(), valid.begin() + size), false);
    }
    valid[4 + 7 * 2] = 1;
    check_scan(valid, false);
    for (int32_t length : {int32_t{-1}, INT32_MIN, INT32_MAX}) {
        auto malformed = parcel_for(u"lineage");
        std::memcpy(malformed.data(), &length, 4);
        check_scan(malformed, false);
    }
    auto multiple = parcel_for(u"activity");
    put_string(multiple, multiple.size(), u"lineage");
    put_string(multiple, multiple.size(), u"PrOfIlE");
    put_string(multiple, multiple.size(), u"vendor.profile");
    check_scan(multiple, true);
}

uint32_t random_state = 0x15d7ba42;
uint32_t random_word() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

void test_differential_parcels() {
    for (size_t iteration = 0; iteration < 20000; ++iteration) {
        std::vector<uint8_t> actual(random_word() % 2048);
        for (auto &byte : actual) byte = static_cast<uint8_t>(random_word());
        for (size_t n = 0, count = random_word() % 8; n < count; ++n) {
            const size_t offset = (random_word() % 512) * 4;
            std::u16string value(random_word() % 520, u'x');
            for (auto &c : value) c = static_cast<char16_t>(0x20 + random_word() % 95);
            if (value.size() > 15 && (random_word() & 1)) {
                value.replace(random_word() % (value.size() - 15), 7, u"LiNeAgE");
            }
            if ((random_word() % 8) == 0) value = u"PrOfIlE";
            put_string(actual, offset, value);
            if ((random_word() % 8) == 0 && !value.empty()) actual[offset + 4] = 1;
        }
        if ((iteration % 4) == 0 && !actual.empty()) actual.resize(random_word() % actual.size());
        auto expected = actual;
        reference_scan(expected);
        scan_sm_parcel_for_strings(actual.data(), actual.size());
        CHECK(actual == expected);
    }
}

binder_transaction_data transaction(std::vector<uint8_t> &data) {
    binder_transaction_data txn{};
    txn.data_size = data.size();
    txn.data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(data.data());
    return txn;
}

void test_request_filtering() {
    auto data = parcel_for(u"Lineage");
    const auto original = data;
    auto txn = transaction(data);
    txn.target.handle = 1;
    g_pending_sm_reply = false;
    process_transaction(txn);
    CHECK(data == original);
    CHECK(!g_pending_sm_reply);
    txn.target.handle = 0;
    txn.flags = TF_ONE_WAY;
    process_transaction(txn);
    CHECK(data == original);
    CHECK(!g_pending_sm_reply);
    txn.flags = 0;
    process_transaction(txn);
    auto expected = original;
    reference_scan(expected);
    CHECK(data == expected);
    CHECK(g_pending_sm_reply);
    g_pending_sm_reply = false;
}

void release_reply(binder_uintptr_t swap, binder_uintptr_t original) {
    // Binder command payloads begin four bytes after the command word. Place
    // the stream so the uintptr_t payload has natural alignment in host tests.
    alignas(binder_uintptr_t) uint8_t storage[4 + 4 + sizeof(binder_uintptr_t)]{};
    auto *command = storage + 4;
    const uint32_t opcode = BC_FREE_BUFFER;
    std::memcpy(command, &opcode, 4);
    std::memcpy(command + 4, &swap, sizeof(swap));
    binder_write_read bwr{};
    bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(command);
    bwr.write_size = 4 + sizeof(swap);
    process_write(&bwr);
    binder_uintptr_t restored;
    std::memcpy(&restored, command + 4, sizeof(restored));
    CHECK(restored == original);
}

void test_reply_buffers() {
    auto ordinary = parcel_for(u"android.os.IServiceManager");
    auto ordinary_txn = transaction(ordinary);
    const auto ordinary_ptr = ordinary_txn.data.ptr.buffer;
    allocation_test::calls = 0;
    allocation_test::enabled = true;
    g_pending_sm_reply = true;
    process_reply(&ordinary_txn);
    allocation_test::enabled = false;
    CHECK(ordinary_txn.data.ptr.buffer == ordinary_ptr);
    CHECK(allocation_test::calls == 0);
    CHECK(g_swap_buf == nullptr);
    CHECK(g_swap_capacity == 0);
    CHECK(!g_pending_sm_reply);

    auto hidden = parcel_for(u"lineage");
    auto hidden_txn = transaction(hidden);
    const auto hidden_ptr = hidden_txn.data.ptr.buffer;
    allocation_test::enabled = true;
    allocation_test::fail = true;
    CHECK(!swap_reply_buffer(&hidden_txn));
    allocation_test::enabled = false;
    allocation_test::fail = false;
    CHECK(hidden_txn.data.ptr.buffer == hidden_ptr);
    CHECK(g_swap_buf == nullptr);
    CHECK(g_swap_capacity == 0);

    std::vector<binder_size_t> offsets{0, 8, 16};
    hidden_txn.offsets_size = offsets.size() * sizeof(offsets[0]);
    hidden_txn.data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(offsets.data());
    CHECK(swap_reply_buffer(&hidden_txn));
    const auto small_swap = hidden_txn.data.ptr.buffer;
    CHECK(small_swap != hidden_ptr);
    CHECK(g_swap_capacity == 256);
    CHECK(g_original_reply_buffer == hidden_ptr);
    CHECK((hidden_txn.data.ptr.offsets & 7) == 0);
    CHECK(std::memcmp(reinterpret_cast<const void *>(hidden_txn.data.ptr.offsets),
                      offsets.data(), hidden_txn.offsets_size) == 0);
    auto expected = hidden;
    reference_scan(expected);
    CHECK(std::memcmp(reinterpret_cast<const void *>(small_swap), expected.data(), expected.size()) == 0);
    CHECK(hidden == parcel_for(u"lineage"));

    auto second_txn = transaction(hidden);
    CHECK(!swap_reply_buffer(&second_txn));
    release_reply(small_swap, hidden_ptr);
    CHECK(g_active_swap_ptr == nullptr);
    CHECK(g_original_reply_buffer == 0);
    CHECK(swap_reply_buffer(&second_txn));
    CHECK(second_txn.data.ptr.buffer == small_swap);
    release_reply(second_txn.data.ptr.buffer, hidden_ptr);

    auto large = parcel_for(u"lineage");
    large.resize(128 * 1024);
    auto large_txn = transaction(large);
    const auto large_original = large_txn.data.ptr.buffer;
    allocation_test::enabled = true;
    allocation_test::fail = true;
    CHECK(!swap_reply_buffer(&large_txn));
    allocation_test::enabled = false;
    allocation_test::fail = false;
    CHECK(large_txn.data.ptr.buffer == large_original);
    CHECK(g_swap_capacity == 256);
    CHECK(reinterpret_cast<binder_uintptr_t>(g_swap_buf) == small_swap);
    second_txn = transaction(hidden);
    CHECK(swap_reply_buffer(&second_txn));
    CHECK(second_txn.data.ptr.buffer == small_swap);
    release_reply(second_txn.data.ptr.buffer, hidden_ptr);
    allocation_test::calls = 0;
    allocation_test::enabled = true;
    CHECK(swap_reply_buffer(&large_txn));
    allocation_test::enabled = false;
    CHECK(allocation_test::calls > 0);
    CHECK(g_swap_capacity == 128 * 1024);
    release_reply(large_txn.data.ptr.buffer, large_original);

    second_txn = transaction(hidden);
    allocation_test::calls = 0;
    allocation_test::enabled = true;
    CHECK(swap_reply_buffer(&second_txn));
    allocation_test::enabled = false;
    CHECK(allocation_test::calls == 0);
    CHECK(second_txn.data.ptr.buffer == large_txn.data.ptr.buffer);
    release_reply(second_txn.data.ptr.buffer, hidden_ptr);

    auto oversized = parcel_for(u"lineage");
    oversized.resize(256 * 1024 + 4);
    auto oversized_txn = transaction(oversized);
    const auto oversized_ptr = oversized_txn.data.ptr.buffer;
    CHECK(!swap_reply_buffer(&oversized_txn));
    CHECK(oversized_txn.data.ptr.buffer == oversized_ptr);

    auto overflow_txn = transaction(hidden);
    overflow_txn.offsets_size = static_cast<binder_size_t>(-1);
    CHECK(!swap_reply_buffer(&overflow_txn));
    CHECK(overflow_txn.data.ptr.buffer == hidden_ptr);
    overflow_txn = transaction(hidden);
    overflow_txn.data_size = static_cast<binder_size_t>(-1);
    CHECK(!swap_reply_buffer(&overflow_txn));
    CHECK(overflow_txn.data.ptr.buffer == hidden_ptr);
}

void *thread_with_cached_buffer(void *) {
    auto data = parcel_for(u"lineage");
    auto txn = transaction(data);
    const auto original = txn.data.ptr.buffer;
    CHECK(swap_reply_buffer(&txn));
    allocation_test::exit_buffer = g_swap_buf;
    release_reply(txn.data.ptr.buffer, original);
    // pthread_exit performs real thread cleanup, including the key destructor.
    pthread_exit(nullptr);
}

void test_thread_exit_cleanup() {
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, thread_with_cached_buffer, nullptr) == 0);
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(allocation_test::exit_buffer.load() != nullptr);
    CHECK(allocation_test::exit_buffer_frees.load() == 1);
    allocation_test::exit_buffer = nullptr;
}
}

int main() {
    test_matching_and_boundaries();
    test_differential_parcels();
    test_request_filtering();
    test_reply_buffers();
    test_thread_exit_cleanup();
    std::puts("Native regression tests passed (20,000 differential parcels).");
}
