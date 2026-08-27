#include "binder_hook.h"
#include "config.h"
#include "logger.h"
#include "service_cache.h"
#include "zygisk.hpp"

#include <jni.h>
#include <array>
#include <cstring>
#include <new>
#include <string>

namespace {
// Keep process-lifetime state trivially destructible.  Zygote children are
// short-lived and registering C++ destructors in libc's atexit array merely
// creates an unnecessary runtime fingerprint.
YukariConfig *g_config = nullptr; // intentionally leaked until process exit
std::array<char, 256> g_package_name{};
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
        if (!g_config) g_config = new (std::nothrow) YukariConfig();
        if (!g_config) {
            log_error("configuration allocation failed; module disabled");
            return;
        }
        load_config(*g_config);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        g_enabled_for_process = false;
        g_jni_hook_ready_ = false;
        g_package_name.fill('\0');
        if (!args) return;

        const std::string package_name = jstr_to_str(env_, args->nice_name);
        if (!package_name.empty()) {
            const size_t count = (package_name.size() < g_package_name.size() - 1)
                                     ? package_name.size()
                                     : g_package_name.size() - 1;
            std::memcpy(g_package_name.data(), package_name.data(), count);
            g_package_name[count] = '\0';
        }
        if (!g_config || !is_target(*g_config, package_name)) {
            if (api_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        g_enabled_for_process = true;
        if (api_ && g_config->force_denylist_unmount) {
            api_->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
        }
        // The Zygisk API is guaranteed to be live in preAppSpecialize.  Hook
        // the boot-class native method here, before post-specialization API
        // calls become implementation-defined.
        g_jni_hook_ready_ = install_jni_hook(env_, api_);
        log_info("matched target %s", g_package_name.data());
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!g_enabled_for_process) return;
        clear_cache(env_);
        // BinderProxy JNI interception leaves libbinder GOT/PLT untouched.
        // Old releases without the stable JNI entry point use the existing
        // ioctl path, whose callback is now reached through an anonymous RX
        // trampoline.
        if (!g_jni_hook_ready_) install_hooks(api_);
        log_info("enabled for %s", g_package_name.data());
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool g_jni_hook_ready_ = false;
};

REGISTER_ZYGISK_MODULE(YukariModule)
