#include "binder_hook.h"
#include "config.h"
#include "logger.h"
#include "service_cache.h"
#include "zygisk.hpp"

#include <jni.h>
#include <string>

namespace {
YukariConfig g_config;
std::string g_package_name;
bool g_enabled_for_process = false;

std::string jstring_to_string(JNIEnv *env, jstring value) {
    if (!env || !value) return {};
    const char *raw = env->GetStringUTFChars(value, nullptr);
    if (!raw) return {};
    std::string out = raw;
    env->ReleaseStringUTFChars(value, raw);
    return out;
}
} // namespace

class YukariModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        load_config(g_config);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        g_enabled_for_process = false;
        g_package_name.clear();
        if (!args) return;

        g_package_name = jstring_to_string(env_, args->nice_name);
        if (!is_target_package(g_config, g_package_name)) {
            if (api_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        g_enabled_for_process = true;
        if (api_) api_->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
        yukari_log_info("matched target %s enhanced=%d", g_package_name.c_str(), g_config.enhanced_mode ? 1 : 0);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!g_enabled_for_process) return;
        clear_service_manager_cache(env_);
        install_binder_hooks(api_, g_config.enhanced_mode);
        yukari_log_info("enabled for %s", g_package_name.c_str());
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(YukariModule)
