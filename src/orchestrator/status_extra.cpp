#include "orchestrator/status_extra.hpp"

#include "audio/apm.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cmath>
#include <cstddef>

namespace acva::orchestrator {

std::function<std::string()>
make_status_extra(const supervisor::Supervisor& sup,
                   const std::unique_ptr<CaptureStack>& capture) {
    return [&sup, &capture]() -> std::string {
        const auto snap = sup.snapshot();
        std::string out = "\"pipeline_state\":\"";
        out.append(supervisor::to_string(snap.pipeline_state));
        out.append("\",\"services\":[");
        for (std::size_t i = 0; i < snap.services.size(); ++i) {
            const auto& s = snap.services[i];
            const auto last_ok_ms_ago =
                s.last_ok_at.time_since_epoch().count() == 0
                    ? -1
                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - s.last_ok_at).count();
            if (i) out.push_back(',');
            out += fmt::format(
                R"({{"name":"{}","state":"{}","last_ok_ms_ago":{},)"
                R"("consecutive_failures":{},"total_probes":{},)"
                R"("total_failures":{},"last_http_status":{}}})",
                s.name,
                supervisor::to_string(s.state),
                last_ok_ms_ago,
                s.consecutive_failures,
                s.total_probes,
                s.total_failures,
                s.last_http_status);
        }
        out.push_back(']');

        // M6 — APM block. Present whenever the pipeline is up and an
        // APM stage was constructed (i.e., capture_enabled + a
        // loopback ring + a non-stub build of webrtc-audio-processing-1).
        if (capture && capture->pipeline()) {
            const auto* apm = capture->pipeline()->apm();
            if (apm != nullptr) {
                const float erle = apm->erle_db();
                if (std::isnan(erle)) {
                    out += fmt::format(
                        R"(,"apm":{{"active":{},"delay_ms":{},)"
                        R"("erle_db":null,"frames_processed":{}}})",
                        apm->aec_active(),
                        apm->aec_delay_estimate_ms(),
                        apm->frames_processed());
                } else {
                    out += fmt::format(
                        R"(,"apm":{{"active":{},"delay_ms":{},)"
                        R"("erle_db":{:.2f},"frames_processed":{}}})",
                        apm->aec_active(),
                        apm->aec_delay_estimate_ms(),
                        erle,
                        apm->frames_processed());
                }
            }
        }
        return out;
    };
}

} // namespace acva::orchestrator
