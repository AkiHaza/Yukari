#include "config.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAIL config line %d: %s\n", __LINE__, #expr); \
    std::exit(1); } } while (false)

int main() {
    YukariConfig defaults;
    CHECK(!defaults.enabled);
    CHECK(defaults.targets.empty());
    CHECK(defaults.hide_features);
    CHECK(defaults.hide_resources);
    CHECK(!is_target(defaults, "com.example.app"));
    defaults.enabled = true;
    CHECK(!is_target(defaults, "com.example.app"));
    CHECK(!is_target(defaults, "com.example.app:remote"));

    YukariConfig config{true, {"com.example.app"}};
    CHECK(config.hide_features);
    CHECK(config.hide_resources);
    CHECK(is_target(config, "com.example.app"));
    CHECK(is_target(config, "com.example.app:remote"));
    CHECK(is_target(config, "com.example.app:worker"));
    CHECK(!is_target(config, "com.example.application"));
    CHECK(!is_target(config, "com.example.app.other:remote"));
    CHECK(!is_target(config, ""));
    CHECK(!is_target(config, ":remote"));
    config.targets = {"com.example.app:worker"};
    CHECK(is_target(config, "com.example.app:worker"));
    CHECK(!is_target(config, "com.example.app"));
    CHECK(!is_target(config, "com.example.app:remote"));
    config.targets = {"android", "system", "system_server",
                      "com.android.systemui", "com.android.settings:remote"};
    CHECK(!is_target(config, "android"));
    CHECK(!is_target(config, "system:worker"));
    CHECK(!is_target(config, "system_server"));
    CHECK(!is_target(config, "com.android.systemui:remote"));
    CHECK(!is_target(config, "com.android.settings:remote"));
    config.enabled = false;
    config.targets = {"com.example.app"};
    CHECK(!is_target(config, "com.example.app"));
    YukariConfig explicitly_disabled{false, {"com.example.app"}, false, false};
    CHECK(!explicitly_disabled.enabled);
    CHECK(!explicitly_disabled.hide_features);
    CHECK(!explicitly_disabled.hide_resources);
    CHECK(!is_target(explicitly_disabled, "com.example.app"));
    std::puts("config tests passed");
}
