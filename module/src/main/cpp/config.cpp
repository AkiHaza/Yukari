#include "config.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
struct FileCloser {
    void operator()(FILE *file) const { std::fclose(file); }
};

bool read_config(int module_dir_fd, std::string &out) {
    // Zygote cannot traverse /data/adb on enforcing devices. Zygisk supplies
    // an already-open module directory so only the config file is accessed.
    if (module_dir_fd < 0) {
        errno = EBADF;
        return false;
    }
    const int fd = openat(module_dir_fd, "config.json", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    FILE *raw = fdopen(fd, "rb");
    if (!raw) {
        const int error = errno;
        close(fd);
        errno = error;
        return false;
    }
    std::unique_ptr<FILE, FileCloser> fp(raw);
    char buffer[4096];
    while (true) {
        const size_t n = std::fread(buffer, 1, sizeof(buffer), fp.get());
        if (n > 0) out.append(buffer, n);
        if (n < sizeof(buffer)) {
            const int error = std::ferror(fp.get()) ? (errno ? errno : EIO) : 0;
            fp.reset();
            errno = error;
            return error == 0;
        }
    }
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
} // namespace

bool load_config(YukariConfig &out, int module_dir_fd) {
    out = {};
    std::string text;
    if (!read_config(module_dir_fd, text)) return false;
    if (text.empty()) {
        errno = EINVAL;
        return false;
    }

    out.enabled = true;
    parse_bool(text, "enabled", out.enabled);
    parse_bool(text, "hide_features", out.hide_features);
    parse_bool(text, "hide_resources", out.hide_resources);
    out.targets = parse_targets(text);
    return true;
}

bool is_target(const YukariConfig &config, const std::string &process_name) {
    if (!config.enabled || process_name.empty()) return false;
    const std::string_view process(process_name);
    const auto colon = process.find(':');
    const auto package = process.substr(0, colon);
    if (package.empty()) return false;
    static constexpr std::string_view protected_packages[] = {
        "android",
        "system",
        "system_server",
        "com.android.systemui",
        "com.android.settings",
    };
    for (const auto protected_package : protected_packages) {
        if (protected_package == package) return false;
    }
    for (const auto &target : config.targets) {
        // A package rule includes its private processes. An explicit process
        // rule remains exact, so com.example:worker does not match :remote.
        if (target == process || (target.find(':') == std::string::npos && target == package)) {
            return true;
        }
    }
    return false;
}
