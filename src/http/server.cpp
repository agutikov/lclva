#include "http/server.hpp"

#include "log/log.hpp"

#include <prometheus/text_serializer.h>

#include <httplib.h>

#include <fmt/format.h>

#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lclva::http {

namespace {

std::string serialize_metrics(const prometheus::Registry& reg) {
    prometheus::TextSerializer serializer;
    std::ostringstream os;
    serializer.Serialize(os, reg.Collect());
    return os.str();
}

std::string serialize_status(const dialogue::Fsm* fsm) {
    if (!fsm) {
        return R"({"state":"unknown"})" "\n";
    }
    const auto snap = fsm->snapshot();
    return fmt::format(
        R"({{"state":"{}","active_turn":{},"outcome":"{}",)"
        R"("sentences_played":{},"turns_completed":{},)"
        R"("turns_interrupted":{},"turns_discarded":{}}}{})",
        dialogue::to_string(snap.state),
        snap.active_turn,
        dialogue::to_string(snap.outcome),
        snap.sentences_played,
        snap.turns_completed,
        snap.turns_interrupted,
        snap.turns_discarded,
        "\n");
}

} // namespace

// PIMPL-ish — keep httplib symbols out of the header so users of
// http/server.hpp don't pull a 10k-line include.
struct ControlServer::Impl {
    httplib::Server server;
    std::thread listen_thread;
};

ControlServer::ControlServer(const config::ControlConfig& cfg,
                             std::shared_ptr<metrics::Registry> registry,
                             const dialogue::Fsm* fsm)
    : registry_(std::move(registry)), fsm_(fsm), impl_(std::make_unique<Impl>()) {

    auto& server = impl_->server;

    server.Get("/metrics", [reg = registry_->registry()](const httplib::Request&, httplib::Response& res) {
        res.set_content(serialize_metrics(*reg), "text/plain; version=0.0.4");
    });

    server.Get("/status", [fsm = fsm_](const httplib::Request&, httplib::Response& res) {
        res.set_content(serialize_status(fsm), "application/json");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok\n", "text/plain");
    });

    if (!server.bind_to_port(cfg.bind, cfg.port)) {
        throw std::runtime_error(
            fmt::format("control server: failed to bind {}:{}", cfg.bind, cfg.port));
    }
    bound_port_ = cfg.port;

    impl_->listen_thread = std::thread([this] {
        impl_->server.listen_after_bind();
    });

    log::info("http", fmt::format("control plane listening on {}:{} (port {})",
                                    cfg.bind, cfg.port, bound_port_));
}

ControlServer::~ControlServer() {
    if (impl_) {
        impl_->server.stop();
        if (impl_->listen_thread.joinable()) {
            impl_->listen_thread.join();
        }
    }
}

} // namespace lclva::http
