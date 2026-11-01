#pragma once

#include "zygisk.hpp"

struct ResourceHookStatus {
    int modern_methods = 0;
    int legacy_methods = 0;
    bool modern_mapping = false;
    bool legacy_mapping = false;
    bool complete() const {
        return (modern_methods == 6 && modern_mapping) ||
               (legacy_methods == 6 && legacy_mapping);
    }
};

ResourceHookStatus resource_hook_status();

// Call once, in preAppSpecialize, only for a configured target. Missing JNI
// signatures disable that entry point; map lookup failures leave IDs intact.
bool install_resource_hooks(JNIEnv *env, zygisk::Api *api);
