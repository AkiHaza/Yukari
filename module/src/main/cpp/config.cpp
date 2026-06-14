#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace {
constexpr const char *kConfigPath = "/data/adb/modules/Yukari/config.json";

std::string read_file(const char *path) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::string out;
    char buffer[4096];
    while (true) {
        size_t n = std::fread(buffer, 1, sizeof(buffer), fp);
        if (n > 0) out.append(buffer, n);
        if (n < sizeof(buffer)) break;
    }
    std::fclose(fp);
    return out;
}

void skip_ws(const std::string &text, size_t &pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
}

bool parse_json_string(const std::string &text, size_t &pos, std::string &out) {
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

bool find_value_start(const std::string &text, const char *key, size_t &pos) {
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

bool parse_bool_field(const std::string &text, const char *key, bool &value) {
    size_t pos = 0;
    if (!find_value_start(text, key, pos)) return false;
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
    if (!find_value_start(text, "targets", pos)) return targets;
    if (text[pos] != '[') return targets;
    ++pos;

    while (pos < text.size()) {
        skip_ws(text, pos);
        if (pos >= text.size()) break;
        if (text[pos] == ']') break;

        std::string value;
        if (!parse_json_string(text, pos, value)) break;
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
} // namespace

bool load_config(YukariConfig &out) {
    const std::string text = read_file(kConfigPath);
    if (text.empty()) {
        out = {};
        return false;
    }

    out = {};
    out.enabled = true;
    out.enhanced_mode = false;
    parse_bool_field(text, "enabled", out.enabled);
    parse_bool_field(text, "enhancedMode", out.enhanced_mode);
    out.targets = parse_targets(text);
    return true;
}

bool is_target_package(const YukariConfig &config, const std::string &package_name) {
    if (!config.enabled || package_name.empty()) return false;
    static const std::vector<std::string> protected_packages = {
        "android",
        "system",
        "system_server",
        "com.android.systemui",
        "com.android.settings",
    };
    if (std::find(protected_packages.begin(), protected_packages.end(), package_name) != protected_packages.end()) {
        return false;
    }
    return std::find(config.targets.begin(), config.targets.end(), package_name) != config.targets.end();
}
