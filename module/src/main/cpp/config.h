#pragma once

#include <string>
#include <vector>

struct YukariConfig {
    bool enabled = false;
    std::vector<std::string> targets;
    bool hide_features = false;
    bool hide_resources = false;
};

bool load_config(YukariConfig &out);
bool is_target(const YukariConfig &config, const std::string &process_name);
