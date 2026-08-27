#pragma once

#include <string>
#include <vector>

struct YukariConfig {
    bool enabled = false;
    // Keep the historical default, but allow devices whose applications rely
    // on Magisk-provided mounts to opt out without disabling service filtering.
    bool force_denylist_unmount = true;
    std::vector<std::string> targets;
};

bool load_config(YukariConfig &out);
bool is_target(const YukariConfig &config, const std::string &package_name);
