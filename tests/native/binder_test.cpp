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

std::vector<uint8_t> request_for(std::u16string_view name, size_t prefix = 0) {
    auto data = parcel_for(u"android.os.IServiceManager", prefix);
    put_string(data, data.size(), name);
    return data;
}

std::vector<uint8_t> transaction_command(const binder_transaction_data &txn,
                                         uint32_t command = BC_TRANSACTION) {
    std::vector<uint8_t> stream(sizeof(command) + _IOC_SIZE(command));
    std::memcpy(stream.data(), &command, sizeof(command));
    std::memcpy(stream.data() + sizeof(command), &txn, sizeof(txn));
    return stream;
}

binder_transaction_data command_transaction(const uint8_t *stream, size_t offset = 4) {
    binder_transaction_data txn;
    std::memcpy(&txn, stream + offset, sizeof(txn));
    return txn;
}

binder_write_read write_stream(std::vector<uint8_t> &stream) {
    binder_write_read bwr{};
    bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(stream.data());
    bwr.write_size = stream.size();
    return bwr;
}

void reset_contexts() {
    g_pending_sm_reply = false;
    g_reply_contexts = 0;
    g_reply_depth = 0;
    g_reply_overflow = 0;
}

void check_request_unchanged(std::vector<uint8_t> &data, uint32_t handle = 0, uint32_t flags = 0) {
    const auto original = data;
    auto txn = transaction(data);
    txn.target.handle = handle;
    txn.flags = flags;
    auto stream = transaction_command(txn);
    const auto original_stream = stream;
    auto bwr = write_stream(stream);
    allocation_test::calls = 0;
    allocation_test::enabled = true;
    {
        PrivateWrite copy(bwr);
        CHECK(prepare_write(bwr, copy));
        CHECK(copy.commands() == nullptr);
    }
    allocation_test::enabled = false;
    CHECK(allocation_test::calls == 0);
    CHECK(stream == original_stream);
    CHECK(data == original);
}

void test_request_filtering() {
    for (size_t prefix : {size_t{0}, size_t{4}, size_t{8}, size_t{12}, size_t{16}}) {
        auto data = request_for(u"LiNeAgE", prefix);
        const size_t trailing = data.size();
        put_string(data, trailing, u"lineage");
        const auto original = data;
        auto txn = transaction(data);
        // Deliberately avoid a transaction-code allowlist: lookup variants
        // such as waitForService/isDeclared must retain this protection.
        txn.code = 1001;
        for (uint32_t command : {uint32_t{BC_TRANSACTION}, uint32_t{BC_TRANSACTION_SG}}) {
            auto stream = transaction_command(txn, command);
            const auto original_stream = stream;
            auto bwr = write_stream(stream);
            PrivateWrite copy(bwr);
            CHECK(prepare_write(bwr, copy));
            CHECK(copy.commands() != nullptr);
            const auto sent = command_transaction(copy.commands());
            CHECK(sent.code == txn.code);
            CHECK(sent.data.ptr.buffer != txn.data.ptr.buffer);
            CHECK(sent.data_size == txn.data_size);
            CHECK(sent.offsets_size == txn.offsets_size);
            auto expected = original;
            put_string(expected, service_name_position(txn), u"_______");
            CHECK(std::memcmp(reinterpret_cast<const void *>(sent.data.ptr.buffer),
                              expected.data(), expected.size()) == 0);
            CHECK(stream == original_stream);
            CHECK(data == original);
        }
    }
    auto data = request_for(u"lineage");
    check_request_unchanged(data, 1);
    check_request_unchanged(data, 0, TF_ONE_WAY);
    auto ordinary = request_for(u"activity");
    put_string(ordinary, ordinary.size(), u"lineage");
    check_request_unchanged(ordinary);
    auto wrong_interface = parcel_for(u"android.os.IUnknownManager");
    put_string(wrong_interface, wrong_interface.size(), u"lineage");
    check_request_unchanged(wrong_interface);
    auto wrong_header = request_for(u"lineage", 20);
    check_request_unchanged(wrong_header);
    auto malformed = data;
    malformed[4 + std::u16string_view(u"android.os.IServiceManager").size() * 2] = 1;
    check_request_unchanged(malformed);
    for (size_t size = 0; size < data.size(); ++size) {
        auto truncated = std::vector<uint8_t>(data.begin(), data.begin() + size);
        check_request_unchanged(truncated);
    }
    auto oversized = data;
    oversized.resize(MAX_REPLY_BUF + 4);
    check_request_unchanged(oversized);

    const auto original = data;
    auto stream = transaction_command(transaction(data));
    const auto original_stream = stream;
    auto bwr = write_stream(stream);
    allocation_test::enabled = true;
    allocation_test::fail = true;
    {
        PrivateWrite copy(bwr);
        CHECK(prepare_write(bwr, copy));
        const auto sent = command_transaction(copy.commands() ? copy.commands() : stream.data());
        CHECK(sent.data.ptr.buffer == reinterpret_cast<binder_uintptr_t>(data.data()));
    }
    allocation_test::enabled = false;
    allocation_test::fail = false;
    CHECK(data == original);
    CHECK(stream == original_stream);
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
    PrivateWrite copy(bwr);
    CHECK(prepare_write(bwr, copy));
    CHECK(copy.commands() != nullptr);
    binder_uintptr_t restored;
    std::memcpy(&restored, copy.commands() + 4, sizeof(restored));
    CHECK(restored == original);
    std::memcpy(&restored, command + 4, sizeof(restored));
    CHECK(restored == swap);
    CHECK(g_active_swap_ptr != nullptr);
    consumed_write(bwr, bwr.write_size);
}

const std::vector<uint8_t> *ioctl_expected_data = nullptr;
binder_uintptr_t ioctl_original_data = 0;
size_t ioctl_payload_offset = 4;
bool ioctl_expect_rewrite = false;
bool ioctl_check_free = false;
binder_uintptr_t ioctl_expected_free = 0;
binder_size_t ioctl_consumed = 0;
int ioctl_result = 0;
int ioctl_errno = 0;
size_t ioctl_calls = 0;

int inspect_ioctl(int, unsigned long request, void *arg) {
    CHECK(request == BINDER_WRITE_READ);
    ++ioctl_calls;
    auto *bwr = static_cast<binder_write_read *>(arg);
    const auto *stream = reinterpret_cast<const uint8_t *>(bwr->write_buffer);
    if (ioctl_expected_data) {
        const auto txn = command_transaction(stream, ioctl_payload_offset);
        CHECK(txn.data_size == ioctl_expected_data->size());
        CHECK((txn.data.ptr.buffer != ioctl_original_data) == ioctl_expect_rewrite);
        CHECK(std::memcmp(reinterpret_cast<const void *>(txn.data.ptr.buffer),
                          ioctl_expected_data->data(), ioctl_expected_data->size()) == 0);
    }
    if (ioctl_check_free) {
        binder_uintptr_t buffer = 0;
        std::memcpy(&buffer, stream + ioctl_payload_offset, sizeof(buffer));
        CHECK(buffer == ioctl_expected_free);
    }
    bwr->write_consumed = ioctl_consumed;
    errno = ioctl_errno;
    return ioctl_result;
}

void test_ioctl_request_lifetime() {
    reset_contexts();
    auto data = request_for(u"lineage");
    put_string(data, data.size(), u"lineage");
    const auto original = data;
    auto expected = data;
    put_string(expected, service_name_position(transaction(data)), u"_______");
    auto stream = transaction_command(transaction(data));
    const auto original_stream = stream;
    auto bwr = write_stream(stream);
    const auto original_buffer = bwr.write_buffer;
    g_original_ioctl = inspect_ioctl;
    ioctl_expected_data = &expected;
    ioctl_original_data = reinterpret_cast<binder_uintptr_t>(data.data());
    ioctl_expect_rewrite = true;
    ioctl_payload_offset = 4;
    ioctl_consumed = 0;
    ioctl_result = -1;
    ioctl_errno = EINTR;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == -1);
    CHECK(errno == EINTR);
    CHECK(g_reply_depth == 0);
    CHECK(!g_pending_sm_reply);
    CHECK(bwr.write_buffer == original_buffer);
    CHECK(bwr.write_consumed == 0);
    CHECK(stream == original_stream);
    CHECK(data == original);
    ioctl_result = 0;
    ioctl_consumed = stream.size();
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == 0);
    CHECK(g_reply_depth == 1);
    CHECK(g_pending_sm_reply);
    CHECK(bwr.write_buffer == original_buffer);
    CHECK(stream == original_stream);
    CHECK(data == original);

    reset_contexts();
    const uint32_t enter = BC_ENTER_LOOPER;
    auto partial = std::vector<uint8_t>(4 + stream.size());
    std::memcpy(partial.data(), &enter, 4);
    std::memcpy(partial.data() + 4, stream.data(), stream.size());
    const auto original_partial = partial;
    bwr = write_stream(partial);
    ioctl_payload_offset = 8;
    ioctl_consumed = 4;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == 0);
    CHECK(bwr.write_consumed == 4);
    CHECK(g_reply_depth == 0);
    CHECK(partial == original_partial);
    ioctl_consumed = partial.size();
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == 0);
    CHECK(g_reply_depth == 1);
    CHECK(g_pending_sm_reply);
    CHECK(partial == original_partial);
    CHECK(data == original);

    // Progress returned together with an error is still authoritative, and
    // a retry must not register already-consumed transactions a second time.
    reset_contexts();
    auto repeated = stream;
    repeated.insert(repeated.end(), stream.begin(), stream.end());
    const auto original_repeated = repeated;
    bwr = write_stream(repeated);
    ioctl_payload_offset = 4;
    ioctl_consumed = stream.size();
    ioctl_result = -1;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == -1);
    CHECK(g_reply_depth == 1);
    CHECK(repeated == original_repeated);
    ioctl_payload_offset = 4 + stream.size();
    ioctl_consumed = repeated.size();
    ioctl_result = 0;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == 0);
    CHECK(g_reply_depth == 2);
    CHECK(repeated == original_repeated);

    reset_contexts();
    auto ordinary = request_for(u"activity");
    stream = transaction_command(transaction(ordinary));
    bwr = write_stream(stream);
    ioctl_expected_data = &ordinary;
    ioctl_original_data = reinterpret_cast<binder_uintptr_t>(ordinary.data());
    ioctl_expect_rewrite = false;
    ioctl_payload_offset = 4;
    ioctl_consumed = stream.size();
    allocation_test::calls = 0;
    allocation_test::enabled = true;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == 0);
    allocation_test::enabled = false;
    CHECK(allocation_test::calls == 0);
    reset_contexts();
    ioctl_expected_data = nullptr;
    g_original_ioctl = nullptr;
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

void test_reply_release_retry_and_contexts() {
    auto hidden = parcel_for(u"lineage");
    auto txn = transaction(hidden);
    const auto original = txn.data.ptr.buffer;
    CHECK(swap_reply_buffer(&txn));
    const auto swapped = txn.data.ptr.buffer;
    const uint32_t command = BC_FREE_BUFFER;
    std::vector<uint8_t> stream(4 + sizeof(swapped));
    std::memcpy(stream.data(), &command, 4);
    std::memcpy(stream.data() + 4, &swapped, sizeof(swapped));
    const auto original_stream = stream;
    auto bwr = write_stream(stream);
    ioctl_expected_data = nullptr;
    ioctl_check_free = true;
    ioctl_expected_free = original;
    ioctl_payload_offset = 4;
    ioctl_consumed = 0;
    ioctl_result = -1;
    ioctl_errno = EINTR;
    g_original_ioctl = inspect_ioctl;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == -1);
    CHECK(g_active_swap_ptr == reinterpret_cast<void *>(swapped));
    CHECK(g_original_reply_buffer == original);
    CHECK(stream == original_stream);
    ioctl_result = 0;
    ioctl_consumed = stream.size();
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == 0);
    CHECK(g_active_swap_ptr == nullptr);
    CHECK(g_original_reply_buffer == 0);
    CHECK(stream == original_stream);

    txn = transaction(hidden);
    CHECK(swap_reply_buffer(&txn));
    // A large command stream cannot use the inline copy. Failure must leave
    // the swap tracked and must not send its userspace address to Binder.
    std::vector<uint8_t> large(512, 0);
    const uint32_t enter = BC_ENTER_LOOPER;
    for (size_t off = 0; off < large.size(); off += 4) std::memcpy(large.data() + off, &enter, 4);
    large.insert(large.end(), original_stream.begin(), original_stream.end());
    bwr = write_stream(large);
    const auto previous_calls = ioctl_calls;
    allocation_test::enabled = true;
    allocation_test::fail = true;
    CHECK(hook_ioctl(1, BINDER_WRITE_READ, &bwr) == -1);
    CHECK(errno == ENOMEM);
    allocation_test::enabled = false;
    allocation_test::fail = false;
    CHECK(ioctl_calls == previous_calls);
    CHECK(g_active_swap_ptr != nullptr);
    release_reply(txn.data.ptr.buffer, original);
    ioctl_check_free = false;
    g_original_ioctl = nullptr;

    reset_contexts();
    push_reply_context(true);
    push_reply_context(false);
    txn = transaction(hidden);
    process_reply(&txn);
    CHECK(txn.data.ptr.buffer == original);
    CHECK(g_pending_sm_reply);
    process_reply(&txn);
    CHECK(txn.data.ptr.buffer != original);
    CHECK(!g_pending_sm_reply);
    CHECK(g_reply_depth == 0);
    release_reply(txn.data.ptr.buffer, original);

    // Non-reply notifications must not prevent the matching reply in the
    // same read stream from consuming its synchronous call context.
    push_reply_context(true);
    txn = transaction(hidden);
    const uint32_t notification = BR_SPAWN_LOOPER;
    const uint32_t reply = BR_REPLY;
    std::vector<uint8_t> incoming(8 + sizeof(txn));
    std::memcpy(incoming.data(), &notification, 4);
    std::memcpy(incoming.data() + 4, &reply, 4);
    std::memcpy(incoming.data() + 8, &txn, sizeof(txn));
    binder_write_read read{};
    read.read_buffer = reinterpret_cast<binder_uintptr_t>(incoming.data());
    read.read_consumed = incoming.size();
    process_read(&read);
    txn = command_transaction(incoming.data(), 8);
    CHECK(txn.data.ptr.buffer != original);
    CHECK(g_reply_depth == 0);
    release_reply(txn.data.ptr.buffer, original);
}

void test_frozen_reply_contexts() {
    reset_contexts();
    auto hidden = parcel_for(u"lineage");
    auto txn = transaction(hidden);
    const auto original = txn.data.ptr.buffer;
    const uint32_t frozen = BR_FROZEN_REPLY;
    const uint32_t reply = BR_REPLY;
    std::vector<uint8_t> incoming(8 + sizeof(txn));
    std::memcpy(incoming.data(), &frozen, 4);
    std::memcpy(incoming.data() + 4, &reply, 4);
    std::memcpy(incoming.data() + 8, &txn, sizeof(txn));
    binder_write_read read{};
    read.read_buffer = reinterpret_cast<binder_uintptr_t>(incoming.data());
    read.read_consumed = incoming.size();
    push_reply_context(true);
    push_reply_context(false);
    // A failed nested ordinary call must not consume or hide its outer
    // ServiceManager context. Both terminal responses can share one read.
    process_read(&read);
    txn = command_transaction(incoming.data(), 8);
    CHECK(txn.data.ptr.buffer != original);
    CHECK(g_reply_depth == 0);
    CHECK(g_reply_overflow == 0);
    CHECK(!g_pending_sm_reply);
    release_reply(txn.data.ptr.buffer, original);

    // With no outer call, a frozen target leaves no stale false frame.
    push_reply_context(false);
    read.read_consumed = sizeof(frozen);
    process_read(&read);
    CHECK(g_reply_depth == 0);
    CHECK(g_reply_overflow == 0);
    CHECK(!g_pending_sm_reply);

    // BR_TRANSACTION_PENDING_FROZEN reports successful one-way queuing;
    // unlike BR_FROZEN_REPLY, it cannot finish a synchronous context.
    const uint32_t pending_frozen = _IO('r', 20);
    std::memcpy(incoming.data(), &pending_frozen, 4);
    push_reply_context(true);
    process_read(&read);
    CHECK(g_reply_depth == 1);
    CHECK(g_pending_sm_reply);
    reset_contexts();
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
    test_ioctl_request_lifetime();
    test_reply_buffers();
    test_reply_release_retry_and_contexts();
    test_frozen_reply_contexts();
    test_thread_exit_cleanup();
    std::puts("Native regression tests passed (20,000 differential parcels).");
}
