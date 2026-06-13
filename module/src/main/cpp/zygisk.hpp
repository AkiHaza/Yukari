// This is the public API for Zygisk modules.
// Keep this header ABI-compatible with standard Zygisk loaders.

#pragma once

#include <jni.h>

#define ZYGISK_API_VERSION 1

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

class ModuleBase {
public:
    virtual void onLoad([[maybe_unused]] Api *api, [[maybe_unused]] JNIEnv *env) {}
    virtual void preAppSpecialize([[maybe_unused]] AppSpecializeArgs *args) {}
    virtual void postAppSpecialize([[maybe_unused]] const AppSpecializeArgs *args) {}
    virtual void preServerSpecialize([[maybe_unused]] ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize([[maybe_unused]] const ServerSpecializeArgs *args) {}
};

struct AppSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    jboolean *const is_child_zygote;
    jboolean *const is_top_app;
    jobjectArray *const pkg_data_info_list;
    jobjectArray *const whitelisted_data_info_list;
    jboolean *const mount_data_dirs;
    jboolean *const mount_storage_dirs;

    AppSpecializeArgs() = delete;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

    ServerSpecializeArgs() = delete;
};

namespace internal {
struct api_table;
template <class T> void entry_impl(api_table *, JNIEnv *);
}

enum Option : int {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

struct Api {
    int connectCompanion();
    void setOption(Option opt);
    void hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods);
    void pltHookRegister(const char *regex, const char *symbol, void *newFunc, void **oldFunc);
    void pltHookExclude(const char *regex, const char *symbol);
    bool pltHookCommit();

private:
    internal::api_table *impl;
    template <class T> friend void internal::entry_impl(internal::api_table *, JNIEnv *);
};

#define REGISTER_ZYGISK_MODULE(clazz) \
extern "C" __attribute__((visibility("default"), used)) \
void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
    zygisk::internal::entry_impl<clazz>(table, env); \
}

#define REGISTER_ZYGISK_COMPANION(func) \
extern "C" __attribute__((visibility("default"), used)) \
void zygisk_companion_entry(int client) { func(client); }

namespace internal {

struct module_abi {
    long api_version;
    ModuleBase *_this;

    void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
    void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
    void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
    void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);

    explicit module_abi(ModuleBase *module) : api_version(ZYGISK_API_VERSION), _this(module) {
        preAppSpecialize = [](auto self, auto args) { self->preAppSpecialize(args); };
        postAppSpecialize = [](auto self, auto args) { self->postAppSpecialize(args); };
        preServerSpecialize = [](auto self, auto args) { self->preServerSpecialize(args); };
        postServerSpecialize = [](auto self, auto args) { self->postServerSpecialize(args); };
    }
};

struct api_table {
    void *_this;
    bool (*registerModule)(api_table *, module_abi *);

    void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int);
    void (*pltHookRegister)(const char *, const char *, void *, void **);
    void (*pltHookExclude)(const char *, const char *);
    bool (*pltHookCommit)();

    int (*connectCompanion)(void *);
    void (*setOption)(void *, Option);
};

template <class T>
void entry_impl(api_table *table, JNIEnv *env) {
    ModuleBase *module = new T();
    if (!table->registerModule(table, new module_abi(module))) return;
    auto api = new Api();
    api->impl = table;
    module->onLoad(api, env);
}

} // namespace internal

inline int Api::connectCompanion() {
    return impl->connectCompanion(impl->_this);
}

inline void Api::setOption(Option opt) {
    impl->setOption(impl->_this, opt);
}

inline void Api::hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods,
                                      int numMethods) {
    impl->hookJniNativeMethods(env, className, methods, numMethods);
}

inline void Api::pltHookRegister(const char *regex, const char *symbol, void *newFunc, void **oldFunc) {
    impl->pltHookRegister(regex, symbol, newFunc, oldFunc);
}

inline void Api::pltHookExclude(const char *regex, const char *symbol) {
    impl->pltHookExclude(regex, symbol);
}

inline bool Api::pltHookCommit() {
    return impl->pltHookCommit();
}

} // namespace zygisk
