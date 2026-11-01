#include "binder_hook.h"
#include "config.h"
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

        g_package_name = jstr_to_str(env_, args->nice_name);
        if (!is_target(g_config, g_package_name)) {
            if (api_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        g_enabled_for_process = true;
        if (api_) api_->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
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

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(YukariModule)
