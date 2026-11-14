#pragma once

#include <string>
#include <vector>

struct YukariConfig {
    bool enabled = false;
    std::vector<std::string> targets;
    bool hide_features = true;
    bool hide_resources = true;
};

// Borrows the Zygisk module directory descriptor; the caller retains ownership.
bool load_config(YukariConfig &out, int module_dir_fd);
bool is_target(const YukariConfig &config, const std::string &process_name);
