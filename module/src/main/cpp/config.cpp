#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr const char *kConfigPath = "/data/adb/modules/Yukari/config.json";
constexpr size_t kMaxConfigBytes = 64 * 1024;

std::string read_file(const char *path) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::string out;
    char buffer[4096];
    while (true) {
        size_t n = std::fread(buffer, 1, sizeof(buffer), fp);
        if (n > 0) out.append(buffer, n);
        if (out.size() > kMaxConfigBytes) {
            std::fclose(fp);
            return {};
        }
        if (n < sizeof(buffer)) break;
    }
    std::fclose(fp);
    return out;
}

void skip_ws(const std::string &text, size_t &pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
}

bool parse_string(const std::string &text, size_t &pos, std::string &out) {
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= text.size()) return false;
            const char escaped = text[pos++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool find_value(const std::string &text, const char *key, size_t &pos) {
    std::string quoted_key = std::string("\"") + key + "\"";
    const auto key_pos = text.find(quoted_key);
    if (key_pos == std::string::npos) return false;
    pos = key_pos + quoted_key.size();
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] != ':') return false;
    ++pos;
    skip_ws(text, pos);
    return pos < text.size();
}

bool parse_bool(const std::string &text, const char *key, bool &value) {
    size_t pos = 0;
    if (!find_value(text, key, pos)) return false;
    if (text.compare(pos, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (text.compare(pos, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

std::vector<std::string> parse_targets(const std::string &text) {
    std::vector<std::string> targets;
    size_t pos = 0;
    if (!find_value(text, "targets", pos)) return targets;
    if (text[pos] != '[') return targets;
    ++pos;

    while (pos < text.size()) {
        skip_ws(text, pos);
        if (pos >= text.size()) break;
        if (text[pos] == ']') break;

        std::string value;
        if (!parse_string(text, pos, value)) break;
        if (!value.empty()) targets.push_back(value);

        skip_ws(text, pos);
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < text.size() && text[pos] == ']') break;
        break;
    }
    return targets;
}

bool is_protected_package(const std::string &package_name) {
    // These components are queried very early by framework/application
    // startup code.  Filtering them can turn a recoverable lookup into an
    // NPE or an unusable process.  action.sh enumerates -3 packages, but keep
    // this guard for hand-edited configurations as well.
    constexpr const char *kProtected[] = {
        "android",
        "system",
        "system_server",
        "com.android.systemui",
        "com.android.settings",
        "com.android.permissioncontroller",
        "com.android.packageinstaller",
        "com.android.providers.settings",
        "com.android.providers.media",
        "com.android.providers.downloads",
        "com.android.providers.contacts",
        "com.android.providers.calendar",
        "com.android.providers.telephony",
        "com.android.providers.blockednumber",
        "com.android.documentsui",
        "com.android.externalstorage",
        "com.android.phone",
        "com.android.server.telecom",
        "com.android.bluetooth",
        "com.android.nfc",
        "com.android.inputmethod.latin",
        "com.google.android.permissioncontroller",
    };
    for (const char *protected_name : kProtected) {
        if (std::strcmp(package_name.c_str(), protected_name) == 0) return true;
    }
    return false;
}
} // namespace

bool load_config(YukariConfig &out) {
    const std::string text = read_file(kConfigPath);
    if (text.empty()) {
        out = {};
        return false;
    }

    out = {};
    out.enabled = true;
    parse_bool(text, "enabled", out.enabled);
    parse_bool(text, "force_denylist_unmount", out.force_denylist_unmount);
    out.targets = parse_targets(text);
    return true;
}

bool is_target(const YukariConfig &config, const std::string &package_name) {
    if (!config.enabled || package_name.empty()) return false;
    if (is_protected_package(package_name)) return false;
    return std::find(config.targets.begin(), config.targets.end(), package_name) != config.targets.end();
}
