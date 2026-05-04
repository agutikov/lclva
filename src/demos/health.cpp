#include "demos/demo.hpp"

#include "supervisor/probe.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace acva::demos {

namespace {

struct Target {
    std::string name;
    std::string url;
    bool        critical;   // mirrors fail_pipeline_if_down
};

void describe(const Target& t, const supervisor::ProbeResult& r) {
    std::printf(
        "  %-8s %-40s  ok=%s  http=%-3d  latency=%4lldms",
        t.name.c_str(),
        t.url.empty() ? "<not configured>" : t.url.c_str(),
        r.ok ? "yes" : "no ",
        r.http_status,
        static_cast<long long>(r.latency.count()));
    if (!r.ok && !r.body_excerpt.empty()) {
        std::printf("  err=\"%s\"", r.body_excerpt.c_str());
    }
    std::printf("\n");
}

} // namespace

int run_health(const config::Config& cfg, std::span<const std::string> /*args*/) {
    std::vector<Target> targets{
        {"llm", cfg.llm.health.health_url, cfg.llm.health.fail_pipeline_if_down},
        {"stt", cfg.stt.health.health_url, cfg.stt.health.fail_pipeline_if_down},
        {"tts", cfg.tts.health.health_url, cfg.tts.health.fail_pipeline_if_down},
    };

    std::printf(
        "demo[health] probing %zu service(s) with timeout=%ums:\n\n",
        targets.size(), cfg.supervisor.probe_timeout_ms);

    supervisor::HttpProbe probe(
        std::chrono::milliseconds(cfg.supervisor.probe_timeout_ms));

    // Probe each configured /health URL. Speaches' single endpoint
    // serves both STT and TTS, so the stt + tts rows likely point at
    // the same address — that's fine, both are still reported
    // independently.
    bool any_critical_failed = false;
    bool any_probed = false;

    for (const auto& t : targets) {
        if (t.url.empty()) {
            describe(t, supervisor::ProbeResult{});
            continue;
        }
        any_probed = true;
        auto r = probe.get(t.url);
        describe(t, r);
        if (!r.ok && t.critical) any_critical_failed = true;
    }

    std::printf("\n");
    if (!any_probed) {
        std::printf(
            "demo[health] nothing to probe — set health URLs in tts.health / "
            "stt.health / llm.health, or list voices under tts.voices.\n");
        return EXIT_SUCCESS;
    }
    if (any_critical_failed) {
        std::fprintf(stderr,
            "demo[health] FAIL: at least one critical (fail_pipeline_if_down) "
            "service is unhealthy.\n");
        return EXIT_FAILURE;
    }
    std::printf("demo[health] all critical services healthy.\n");
    return EXIT_SUCCESS;
}

} // namespace acva::demos
