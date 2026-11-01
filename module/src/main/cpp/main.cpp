#include "binder_hook.h"
#include "config.h"
#include "feature_hook.h"
#include "logger.h"
#include "service_cache.h"
#include "zygisk.hpp"

#include <jni.h>
#include <string>
#include <vector>

namespace {
YukariConfig g_config;
std::string g_package_name;
bool g_enabled_for_process = false;

std::string jstr_to_str(JNIEnv *env, jstring value) {
    if (!env || !value) return {};
    if (env->ExceptionCheck()) return {};
    const char *raw = env->GetStringUTFChars(value, nullptr);
    if (!raw) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return {};
    }
    try {
        std::string out = raw;
        env->ReleaseStringUTFChars(value, raw);
        return out;
    } catch (...) {
        env->ReleaseStringUTFChars(value, raw);
        return {};
    }
}
} // namespace

class YukariModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        try {
            load_config(g_config);
        } catch (...) {
            g_config = {};
            log_error("configuration load failed; filtering disabled");
        }
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        g_enabled_for_process = false;
        g_package_name.clear();
        if (!args) return;
        // appId, rather than the full uid, also protects secondary users'
        // system processes. Private app processes keep their app's appId.
        if (args->uid < 0 || args->uid % 100000 < 10000) {
            if (api_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        g_package_name = jstr_to_str(env_, args->nice_name);
        if (!is_target(g_config, g_package_name)) {
            if (api_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        g_enabled_for_process = true;
        if (api_) api_->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
        if (g_config.hide_features) {
            try {
                install_feature_hooks(env_, api_);
            } catch (...) {
                log_error("feature hook setup failed; features stay visible");
            }
        }
        log_info("matched target %s", g_package_name.c_str());
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!g_enabled_for_process) return;
        clear_cache(env_);
        install_hooks(api_);
        log_info("enabled for %s", g_package_name.c_str());

        // These inputs are only needed while specializing this app process.
        std::vector<std::string>{}.swap(g_config.targets);
        std::string{}.swap(g_package_name);
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        if (api_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(YukariModule)
