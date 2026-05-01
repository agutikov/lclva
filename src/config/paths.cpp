#include "config/paths.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace acva::config {

namespace {

// std::getenv treated as empty when the variable is set-but-empty.
// XDG spec is explicit about this: `$VAR=""` means "unset".
std::filesystem::path getenv_path(const char* name) {
    if (const char* v = std::getenv(name); v && *v) {
        return v;
    }
    return {};
}

std::filesystem::path home_dir() {
    return getenv_path("HOME");
}

} // namespace

std::filesystem::path xdg_data_home() {
    if (auto x = getenv_path("XDG_DATA_HOME"); !x.empty()) return x;
    auto h = home_dir();
    return h.empty() ? std::filesystem::path{} : h / ".local" / "share";
}

std::filesystem::path xdg_config_home() {
    if (auto x = getenv_path("XDG_CONFIG_HOME"); !x.empty()) return x;
    auto h = home_dir();
    return h.empty() ? std::filesystem::path{} : h / ".config";
}

std::filesystem::path resolve_data_path(std::string_view input,
                                         std::string_view default_filename) {
    namespace fs = std::filesystem;
    fs::path p;

    if (input.empty()) {
        auto root = xdg_data_home();
        // If XDG resolution failed (no XDG_DATA_HOME, no HOME), fall back
        // to the bare filename in CWD. Callers that need a stable
        // absolute path should set XDG_DATA_HOME or HOME explicitly.
        p = root.empty() ? fs::path{default_filename}
                          : root / "acva" / fs::path{default_filename};
    } else {
        fs::path raw{input};
        if (raw.is_absolute()) {
            p = raw;
        } else {
            auto root = xdg_data_home();
            p = root.empty() ? raw : root / "acva" / raw;
        }
    }

    // Best-effort parent dir. Don't promote filesystem errors: the
    // caller's open() will produce a clearer error message if the
    // directory really cannot be created.
    if (auto parent = p.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    return p;
}

std::variant<std::filesystem::path, LoadError>
resolve_config_path(std::string_view cli) {
    namespace fs = std::filesystem;

    if (!cli.empty()) {
        return fs::path{cli};
    }

    std::array<fs::path, 3> candidates;
    auto ch = xdg_config_home();
    candidates[0] = ch.empty() ? fs::path{} : ch / "acva" / "default.yaml";
    candidates[1] = fs::path{"config"} / "default.yaml";
    candidates[2] = fs::path{"/etc/acva/default.yaml"};

    for (const auto& c : candidates) {
        if (c.empty()) continue;
        std::error_code ec;
        if (fs::exists(c, ec) && !ec) return c;
    }

    std::string msg = "config: no config file found; tried";
    bool first = true;
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        msg += first ? " " : ", ";
        first = false;
        msg += c.string();
    }
    msg += " — pass --config PATH or place a default.yaml in one of those locations";
    return LoadError{std::move(msg)};
}

} // namespace acva::config
