#pragma once

#include <string>
#include <vector>

struct YukariConfig {
    bool enabled = false;
    bool enhanced_mode = false;
    std::vector<std::string> targets;
};

bool load_config(YukariConfig &out);
bool is_target_package(const YukariConfig &config, const std::string &package_name);
