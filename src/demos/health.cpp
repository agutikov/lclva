#include "demos/demo.hpp"

#include "supervisor/probe.hpp"

#include <httplib.h>

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

// Liveness probe for a Piper voice URL: POST `{"text":"a"}`. Piper
// rejects empty/whitespace/punct-only input with HTTP 500, so we send
// the smallest single-letter phrase that makes it through the
// phoneme stage and produces a real WAV (~20 KB at amy-medium). A
// 200 here means the synth path actually works, not just that the
// socket is bound — matches what `demo tts` would observe.
supervisor::ProbeResult tts_synth_probe(const std::string& url,
                                          std::chrono::milliseconds timeout) {
    supervisor::ProbeResult r;
    auto parsed = supervisor::parse_url(url);
    if (parsed.authority.empty()) {
        r.body_excerpt = "probe: invalid url (missing scheme)";
        return r;
    }
    httplib::Client cli(parsed.authority);
    cli.set_connection_timeout(timeout);
    cli.set_read_timeout(timeout);

    const auto t0 = std::chrono::steady_clock::now();
    auto res = cli.Post(parsed.path, R"({"text":"a"})", "application/json");
    r.latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0);
    if (!res) {
        r.body_excerpt = std::string{"probe: "} + httplib::to_string(res.error());
        return r;
    }
    r.http_status = res->status;
    r.ok = (res->status >= 200 && res->status < 300);
    if (!r.ok) {
        r.body_excerpt = "non-2xx";
    }
    return r;
}

} // namespace

int run_health(const config::Config& cfg) {
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

    // Build the final probe list: configured GET-probed services
    // first, then one row per configured TTS voice (POST-probed —
    // Piper exposes no /health route, so we exercise the real synth
    // path with a single-letter prompt).
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

    const auto voice_timeout = std::chrono::milliseconds(cfg.supervisor.probe_timeout_ms);
    for (const auto& [lang, voice] : cfg.tts.voices) {
        Target row{"tts/" + lang, voice.url, /*critical*/ false};
        auto r = tts_synth_probe(voice.url, voice_timeout);
        describe(row, r);
        any_probed = true;
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
