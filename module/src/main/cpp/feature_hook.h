#pragma once

#include "zygisk.hpp"

struct FeatureHookStatus {
    bool query = false;
    bool enumeration = false;
};

// Install only in a selected process, while the Zygisk API is still live.
// Missing native methods disable the affected capability and leave it visible.
bool install_feature_hooks(JNIEnv *env, zygisk::Api *api);
FeatureHookStatus feature_hook_status();
