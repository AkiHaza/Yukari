#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace yukari::features {

// Android feature identifiers are case-sensitive. Only known Lineage entries
// are hidden; an arbitrary application string containing "lineage" is not.
inline constexpr std::string_view names[] = {
    "org.lineageos.livedisplay", "org.lineageos.profiles",
    "org.lineageos.hardware", "org.lineageos.globalactions",
    "org.lineageos.trust", "org.lineageos.health",
    "org.lineageos.android", "org.lineageos.settings",
};

template <typename Char>
constexpr bool hidden(std::basic_string_view<Char> value) {
    for (const auto name : names) {
        if (value.size() != name.size()) continue;
        bool equal = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (value[i] != static_cast<Char>(name[i])) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

// The caller serializes access. Saturation fails visible rather than evicting
// another live Parcel. Entries are removed on recycle, destroy, reposition,
// and before a Parcel is reused for any Binder reply.
template <std::size_t Capacity = 64>
class ReplyRegistry {
public:
    bool mark(std::uintptr_t parcel, std::size_t size) {
        if (parcel == 0 || size == 0) return false;
        forget(parcel);
        for (auto &entry : entries_) {
            if (entry.parcel == 0) {
                entry = {parcel, size};
                return true;
            }
        }
        return false;
    }

    bool contains(std::uintptr_t parcel, std::size_t size) {
        if (parcel == 0) return false;
        for (auto &entry : entries_) {
            if (entry.parcel != parcel) continue;
            if (entry.size != size) {
                entry = {};
                return false;
            }
            return true;
        }
        return false;
    }

    void forget(std::uintptr_t parcel) {
        for (auto &entry : entries_) {
            if (entry.parcel == parcel) entry = {};
        }
    }

private:
    struct Entry {
        std::uintptr_t parcel = 0;
        std::size_t size = 0;
    };
    std::array<Entry, Capacity> entries_{};
};

} // namespace yukari::features
