#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAIL config line %d: %s\n", __LINE__, #expr); \
    std::exit(1); } } while (false)

namespace {
struct ConfigFixture {
    std::string path;
    int dir_fd = -1;

    ConfigFixture() {
        char pattern[] = "/tmp/yukari-config-test-XXXXXX";
        const char *created = mkdtemp(pattern);
        CHECK(created);
        path = created;
        dir_fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(dir_fd >= 0);
    }

    ~ConfigFixture() {
        unlinkat(dir_fd, "config.json", 0);
        unlinkat(dir_fd, "config.json", AT_REMOVEDIR);
        close(dir_fd);
        rmdir(path.c_str());
    }

    void write(const char *text) const {
        FILE *fp = fdopen(openat(dir_fd, "config.json", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), "w");
        CHECK(fp);
        CHECK(std::fputs(text, fp) >= 0);
        CHECK(std::fclose(fp) == 0);
    }
};

size_t open_fd_count() {
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory);
    size_t count = 0;
    while (readdir(directory)) ++count;
    CHECK(closedir(directory) == 0);
    return count;
}

void test_config_loading() {
    ConfigFixture fixture;
    YukariConfig config{true, {"stale.target"}, false, false};

    // Failed reads must never retain a previous process's configuration.
    CHECK(!load_config(config, fixture.dir_fd));
    CHECK(errno == ENOENT);
    CHECK(!config.enabled && config.targets.empty());
    CHECK(fcntl(fixture.dir_fd, F_GETFD) >= 0);

    fixture.write(R"({"enabled":true,"hide_features":true,"hide_resources":true,"targets":["com.example.app","com.example.other:worker"]})");
    // The descriptor remains authoritative even when its original path moves.
    const std::string moved = fixture.path + "-moved";
    CHECK(std::rename(fixture.path.c_str(), moved.c_str()) == 0);
    fixture.path = moved;
    CHECK(load_config(config, fixture.dir_fd));
    CHECK(config.enabled && config.hide_features && config.hide_resources);
    CHECK(config.targets.size() == 2);
    CHECK(is_target(config, "com.example.app:remote"));
    CHECK(is_target(config, "com.example.other:worker"));
    CHECK(!is_target(config, "com.example.other:remote"));

    fixture.write(R"({"enabled":false,"hide_features":false,"hide_resources":false,"targets":["com.disabled.app"]})");
    CHECK(load_config(config, fixture.dir_fd));
    CHECK(!config.enabled && !config.hide_features && !config.hide_resources);
    CHECK(config.targets == std::vector<std::string>{"com.disabled.app"});

    fixture.write(R"({"targets":["com.defaults.app"]})");
    const size_t fd_count = open_fd_count();
    for (int i = 0; i < 128; ++i) CHECK(load_config(config, fixture.dir_fd));
    CHECK(open_fd_count() == fd_count);
    CHECK(config.enabled && config.hide_features && config.hide_resources);
    CHECK(config.targets == std::vector<std::string>{"com.defaults.app"});

    fixture.write("");
    CHECK(!load_config(config, fixture.dir_fd));
    CHECK(errno == EINVAL);
    CHECK(!config.enabled && config.targets.empty());
    CHECK(!load_config(config, -1));
    CHECK(errno == EBADF);

    // An openable but unreadable object must not be parsed as partial config.
    CHECK(unlinkat(fixture.dir_fd, "config.json", 0) == 0);
    CHECK(mkdirat(fixture.dir_fd, "config.json", 0700) == 0);
    CHECK(!load_config(config, fixture.dir_fd));
    CHECK(errno == EISDIR);
    CHECK(!config.enabled && config.targets.empty());
    CHECK(open_fd_count() == fd_count);
    CHECK(fcntl(fixture.dir_fd, F_GETFD) >= 0);
}
} // namespace

int main() {
    test_config_loading();
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
