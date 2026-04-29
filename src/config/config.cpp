#include "config/config.hpp"

#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace lclva::config {

namespace {

constexpr std::array kValidLogLevels{
    std::string_view{"trace"}, std::string_view{"debug"}, std::string_view{"info"},
    std::string_view{"warn"},  std::string_view{"error"}, std::string_view{"critical"},
    std::string_view{"off"},
};

constexpr std::array kValidSinks{
    std::string_view{"stderr"},
    std::string_view{"journal"},
    std::string_view{"file"},
};

bool contains(std::string_view value, const auto& valid_set) noexcept {
    for (auto v : valid_set) {
        if (v == value) {
            return true;
        }
    }
    return false;
}

std::string read_file(const std::filesystem::path& path, LoadError& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err.message = "config: cannot open file: " + path.string();
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

LogSink parse_log_sink(std::string_view s) {
    if (s == "journal") return LogSink::journal;
    if (s == "file")    return LogSink::file;
    return LogSink::stderr_;
}

std::optional<LoadError> validate(const Config& cfg) {
    if (!contains(cfg.logging.level, kValidLogLevels)) {
        return LoadError{"config: logging.level: invalid value '" + cfg.logging.level + "'"};
    }
    if (!contains(cfg.logging.sink, kValidSinks)) {
        return LoadError{"config: logging.sink: invalid value '" + cfg.logging.sink + "'"};
    }
    if (cfg.logging.sink == "file" && !cfg.logging.file_path) {
        return LoadError{"config: logging.sink='file' requires logging.file_path"};
    }
    if (cfg.control.port == 0) {
        return LoadError{"config: control.port: must be non-zero"};
    }
    if (cfg.pipeline.fake_barge_in_probability < 0.0
        || cfg.pipeline.fake_barge_in_probability > 1.0) {
        return LoadError{"config: pipeline.fake_barge_in_probability: must be in [0, 1]"};
    }
    if (cfg.pipeline.fake_sentences_per_turn == 0) {
        return LoadError{"config: pipeline.fake_sentences_per_turn: must be > 0"};
    }
    return std::nullopt;
}

LoadResult load_from_string(std::string_view yaml) {
    Config cfg;
    auto ec = glz::read_yaml(cfg, yaml);
    if (ec) {
        return LoadError{"config: parse error: " + glz::format_error(ec, yaml)};
    }
    if (auto verr = validate(cfg)) {
        return *verr;
    }
    return cfg;
}

LoadResult load_from_file(const std::filesystem::path& path) {
    LoadError err;
    auto text = read_file(path, err);
    if (!err.message.empty()) {
        return err;
    }
    return load_from_string(text);
}

} // namespace lclva::config
