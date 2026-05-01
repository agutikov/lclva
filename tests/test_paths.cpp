#include "config/config.hpp"
#include "config/paths.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#include <unistd.h>

namespace fs = std::filesystem;
using acva::config::LoadError;
using acva::config::resolve_config_path;
using acva::config::resolve_data_path;
using acva::config::xdg_config_home;
using acva::config::xdg_data_home;

namespace {

// RAII guard around an environment variable. Captures the prior value
// (or "unset" sentinel) on construction; restores on destruction. doctest
// runs single-threaded, so concurrent guard instances on different vars
// are safe.
class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        if (const char* prev = std::getenv(name)) {
            had_ = true;
            prev_ = prev;
        }
        if (value) {
            ::setenv(name, value, /*overwrite*/ 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~EnvGuard() {
        if (had_) ::setenv(name_, prev_.c_str(), 1);
        else      ::unsetenv(name_);
    }
    EnvGuard(const EnvGuard&)            = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
private:
    const char* name_;
    bool had_ = false;
    std::string prev_;
};

// Make a fresh empty tmp directory; cleanup is the caller's job.
fs::path fresh_tmp(const char* tag) {
    auto p = fs::temp_directory_path()
           / (std::string{"acva-paths-"} + tag + "-"
              + std::to_string(::getpid()) + "-"
              + std::to_string(std::rand()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void touch(const fs::path& p, std::string_view body = "logging: {}\ncontrol: {}\n") {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << body;
}

} // namespace

TEST_CASE("paths::xdg_data_home: prefers XDG_DATA_HOME") {
    EnvGuard g_xdg("XDG_DATA_HOME", "/tmp/explicit-xdg");
    EnvGuard g_home("HOME", "/tmp/some-home");
    CHECK(xdg_data_home() == fs::path{"/tmp/explicit-xdg"});
}

TEST_CASE("paths::xdg_data_home: falls back to $HOME/.local/share") {
    EnvGuard g_xdg("XDG_DATA_HOME", nullptr);
    EnvGuard g_home("HOME", "/tmp/some-home");
    CHECK(xdg_data_home() == fs::path{"/tmp/some-home/.local/share"});
}

TEST_CASE("paths::xdg_data_home: empty XDG_DATA_HOME treated as unset") {
    // Per XDG spec, an explicitly-empty value means "unset".
    EnvGuard g_xdg("XDG_DATA_HOME", "");
    EnvGuard g_home("HOME", "/tmp/some-home");
    CHECK(xdg_data_home() == fs::path{"/tmp/some-home/.local/share"});
}

TEST_CASE("paths::xdg_data_home: returns empty when neither var set") {
    EnvGuard g_xdg("XDG_DATA_HOME", nullptr);
    EnvGuard g_home("HOME", nullptr);
    CHECK(xdg_data_home().empty());
}

TEST_CASE("paths::xdg_config_home: prefers XDG_CONFIG_HOME, else $HOME/.config") {
    {
        EnvGuard g_xdg("XDG_CONFIG_HOME", "/tmp/cfg");
        EnvGuard g_home("HOME", "/tmp/h");
        CHECK(xdg_config_home() == fs::path{"/tmp/cfg"});
    }
    {
        EnvGuard g_xdg("XDG_CONFIG_HOME", nullptr);
        EnvGuard g_home("HOME", "/tmp/h");
        CHECK(xdg_config_home() == fs::path{"/tmp/h/.config"});
    }
}

TEST_CASE("paths::resolve_data_path: empty → XDG_DATA_HOME/acva/<default>") {
    auto root = fresh_tmp("rdp-empty");
    EnvGuard g_xdg("XDG_DATA_HOME", root.c_str());
    EnvGuard g_home("HOME", "/tmp/anywhere");

    auto p = resolve_data_path("", "acva.db");
    CHECK(p == root / "acva" / "acva.db");
    // Parent must have been created.
    CHECK(fs::exists(p.parent_path()));

    fs::remove_all(root);
}

TEST_CASE("paths::resolve_data_path: absolute is returned verbatim") {
    auto abs = fresh_tmp("rdp-abs") / "deep" / "nested" / "x.db";
    EnvGuard g_xdg("XDG_DATA_HOME", "/tmp/should-be-ignored");
    EnvGuard g_home("HOME", "/tmp/h");

    auto p = resolve_data_path(abs.string(), "default-not-used.db");
    CHECK(p == abs);
    // Parent dir was mkdir-p'd.
    CHECK(fs::exists(abs.parent_path()));

    fs::remove_all(abs.parent_path().parent_path());
}

TEST_CASE("paths::resolve_data_path: relative is rooted under XDG_DATA_HOME/acva") {
    auto root = fresh_tmp("rdp-rel");
    EnvGuard g_xdg("XDG_DATA_HOME", root.c_str());
    EnvGuard g_home("HOME", "/tmp/h");

    auto p = resolve_data_path("subdir/voice.db", "default.db");
    CHECK(p == root / "acva" / "subdir" / "voice.db");
    CHECK(fs::exists(p.parent_path()));

    fs::remove_all(root);
}

TEST_CASE("paths::resolve_data_path: no XDG / no HOME → bare default in CWD") {
    EnvGuard g_xdg("XDG_DATA_HOME", nullptr);
    EnvGuard g_home("HOME", nullptr);
    auto p = resolve_data_path("", "fallback.db");
    CHECK(p == fs::path{"fallback.db"});
}

TEST_CASE("paths::resolve_config_path: explicit --config is used verbatim") {
    EnvGuard g_xdg("XDG_CONFIG_HOME", "/dev/null/no-such");
    EnvGuard g_home("HOME", "/dev/null/no-such");
    auto r = resolve_config_path("/etc/something/explicit.yaml");
    REQUIRE(std::holds_alternative<fs::path>(r));
    CHECK(std::get<fs::path>(r) == fs::path{"/etc/something/explicit.yaml"});
}

TEST_CASE("paths::resolve_config_path: XDG_CONFIG_HOME/acva/default.yaml wins when present") {
    auto cfg_root = fresh_tmp("rcp-xdg");
    auto target = cfg_root / "acva" / "default.yaml";
    touch(target);
    EnvGuard g_xdg("XDG_CONFIG_HOME", cfg_root.c_str());
    EnvGuard g_home("HOME", "/dev/null/no-home");

    auto r = resolve_config_path("");
    REQUIRE(std::holds_alternative<fs::path>(r));
    CHECK(std::get<fs::path>(r) == target);

    fs::remove_all(cfg_root);
}

TEST_CASE("paths::resolve_config_path: in-tree ./config/default.yaml is the fallback") {
    // We don't create the in-tree file here — the project ships it. We
    // rely on the test binary's CWD being the build dir, but since we
    // run resolve from a tmp CWD we also stage a file. doctest
    // doesn't expose the run dir directly, so cd in/out.
    auto stage = fresh_tmp("rcp-intree");
    auto config_dir = stage / "config";
    auto target = config_dir / "default.yaml";
    touch(target);

    auto cwd = fs::current_path();
    fs::current_path(stage);

    // XDG misses; in-tree exists. Should pick in-tree.
    EnvGuard g_xdg("XDG_CONFIG_HOME", "/dev/null/no-such");
    EnvGuard g_home("HOME", "/dev/null/no-home");

    auto r = resolve_config_path("");
    fs::current_path(cwd);            // restore before any assertion failure path
    REQUIRE(std::holds_alternative<fs::path>(r));
    CHECK(std::get<fs::path>(r) == fs::path{"config"} / "default.yaml");

    fs::remove_all(stage);
}

TEST_CASE("paths::resolve_config_path: returns LoadError listing tried paths") {
    // Force every candidate to miss.
    auto cfg_root = fresh_tmp("rcp-miss");      // empty — no acva/ subdir
    EnvGuard g_xdg("XDG_CONFIG_HOME", cfg_root.c_str());
    EnvGuard g_home("HOME", cfg_root.c_str());

    auto cwd = fs::current_path();
    fs::current_path(cfg_root);                  // no ./config either

    auto r = resolve_config_path("");
    fs::current_path(cwd);
    REQUIRE(std::holds_alternative<LoadError>(r));
    auto& err = std::get<LoadError>(r);
    CHECK(err.message.find("no config file found") != std::string::npos);
    CHECK(err.message.find(cfg_root.string()) != std::string::npos);
    CHECK(err.message.find("config/default.yaml") != std::string::npos);

    fs::remove_all(cfg_root);
}
