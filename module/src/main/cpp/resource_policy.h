#pragma once

#include <cstdint>
#include <string_view>

namespace resource_policy {
inline constexpr std::string_view kHiddenPackage = "lineageos.platform";

template <typename Char>
constexpr bool hidden_package(std::basic_string_view<Char> value) {
    if (value.size() != kHiddenPackage.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != static_cast<Char>(kHiddenPackage[i])) return false;
    }
    return true;
}

// An explicit package in the resource name overrides the default package.
template <typename Char>
constexpr bool hidden_name(std::basic_string_view<Char> name,
                           std::basic_string_view<Char> default_package) {
    if (name.empty()) return false;
    const auto colon = name.find(static_cast<Char>(':'));
    if (colon != name.npos) return hidden_package(name.substr(0, colon));
    return hidden_package(default_package);
}

// The caller must obtain confirmed_package_id from this AssetManager's map.
// In particular, a bare resource with high byte 0x3f is not sufficient.
constexpr bool hidden_id(std::uint32_t resource_id, int confirmed_package_id) {
    return confirmed_package_id > 0 && confirmed_package_id <= 255 &&
           (resource_id >> 24) == static_cast<std::uint32_t>(confirmed_package_id);
}
} // namespace resource_policy
