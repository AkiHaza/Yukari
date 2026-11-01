#include "service_match.h"

namespace {
template <typename Char>
Char ascii_lower(Char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<Char>(c + ('a' - 'A')) : c;
}

// The caller has already matched the first character. Compare in place so
// neither String8 nor String16 names need a temporary lowercase string.
template <typename Char>
bool matches_at(std::basic_string_view<Char> name, size_t off, std::string_view keyword) {
    if (name.size() - off < keyword.size()) return false;
    for (size_t i = 1; i < keyword.size(); ++i) {
        if (ascii_lower(name[off + i]) != keyword[i]) return false;
    }
    return true;
}

template <typename Char>
bool matches_service(std::basic_string_view<Char> name) {
    if (name.size() == 7 && ascii_lower(name.front()) == 'p' &&
        matches_at(name, 0, "profile")) {
        return true;
    }

    for (size_t off = 0; off < name.size(); ++off) {
        switch (ascii_lower(name[off])) {
            case 'l':
                if (matches_at(name, off, "lineage")) return true;
                break;
            case 'c':
                if (matches_at(name, off, "crdroid")) return true;
                break;
            case 'a':
                if (matches_at(name, off, "aospa")) return true;
                break;
            case 'p':
                if (matches_at(name, off, "pixelexperience") ||
                    matches_at(name, off, "protonaosp")) return true;
                break;
            case 'o':
                if (matches_at(name, off, "omnirom")) return true;
                break;
        }
    }
    return false;
}
} // namespace

bool hide_service(std::string_view service_name) {
    return matches_service(service_name);
}

bool hide_service(std::u16string_view service_name) {
    return matches_service(service_name);
}
