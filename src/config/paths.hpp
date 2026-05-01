#pragma once

#include "config/config.hpp"

#include <filesystem>
#include <string_view>
#include <variant>

namespace acva::config {

// XDG Base Directory resolution. Each call reads the environment fresh
// so tests can setenv/unsetenv to drive deterministic outcomes.
//
//   xdg_data_home():   $XDG_DATA_HOME      | $HOME/.local/share
//   xdg_config_home(): $XDG_CONFIG_HOME    | $HOME/.config
//
// If $HOME is also unset the helpers return an empty path; callers that
// need a definite value should treat that as a misconfiguration. We
// intentionally do NOT fall back to "/" — silently writing to / would
// be worse than failing loudly.
[[nodiscard]] std::filesystem::path xdg_data_home();
[[nodiscard]] std::filesystem::path xdg_config_home();

// Resolve a user-supplied data path:
//   • empty       → xdg_data_home() / "acva" / default_filename
//   • absolute    → returned verbatim
//   • relative    → xdg_data_home() / "acva" / input
//
// As a side effect, the resolver `mkdir -p`s the parent directory so
// that subsequent `open()` calls don't hit ENOENT on the very first
// run. Filesystem errors are silently ignored — the caller's open()
// will surface them with a more useful message.
//
// `default_filename` is required and is only used when `input` is
// empty (e.g. "acva.db", "recordings.sqlite", ...).
[[nodiscard]] std::filesystem::path resolve_data_path(
    std::string_view input,
    std::string_view default_filename);

// Decide which config file to load.
//
//   • If `cli` is non-empty: return it verbatim. The user's explicit
//     --config wins; missing-file errors come from load_from_file.
//
//   • Otherwise search this list in order, returning the first that
//     exists:
//       1. ${XDG_CONFIG_HOME:-$HOME/.config}/acva/default.yaml
//       2. ./config/default.yaml                       (in-tree dev)
//       3. /etc/acva/default.yaml                      (system install)
//
// On no-match, returns LoadError listing the candidates that were
// tried. Callers can pass it through their existing error path.
[[nodiscard]] std::variant<std::filesystem::path, LoadError>
resolve_config_path(std::string_view cli);

} // namespace acva::config
