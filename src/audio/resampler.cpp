#include "audio/resampler.hpp"

#include <soxr.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace acva::audio {

namespace {

unsigned long quality_recipe(Resampler::Quality q) noexcept {
    switch (q) {
        case Resampler::Quality::Quick:    return SOXR_QQ;
        case Resampler::Quality::Low:      return SOXR_LQ;
        case Resampler::Quality::Medium:   return SOXR_MQ;
        case Resampler::Quality::High:     return SOXR_HQ;
        case Resampler::Quality::VeryHigh: return SOXR_VHQ;
    }
    return SOXR_HQ;
}

// Conservative output-buffer size for soxr_process. We round up the
// expected output count by 32 samples so soxr never returns
// "not enough room"; soxr_process reports the actual count back via
// odone, which we use to size the returned vector exactly.
std::size_t output_capacity(std::size_t in_count, double in_rate, double out_rate) {
    const double ratio = out_rate / in_rate;
    const double estimated = static_cast<double>(in_count) * ratio;
    return static_cast<std::size_t>(estimated) + 32;
}

} // namespace

Resampler::Resampler(double in_rate, double out_rate, Quality quality)
    : in_rate_(in_rate), out_rate_(out_rate), quality_(quality) {
    if (in_rate <= 0.0 || out_rate <= 0.0) {
        throw std::runtime_error("resampler: rates must be > 0");
    }
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    soxr_quality_spec_t q_spec = soxr_quality_spec(quality_recipe(quality), 0);
    soxr_runtime_spec_t r_spec = soxr_runtime_spec(/*num_threads=*/1);
    soxr_error_t err = nullptr;
    soxr_ = soxr_create(in_rate, out_rate,
                        /*num_channels=*/1,
                        &err, &io_spec, &q_spec, &r_spec);
    if (err != nullptr || soxr_ == nullptr) {
        const std::string msg = err ? std::string{err}
                                    : std::string{"soxr_create returned null"};
        throw std::runtime_error(std::string{"resampler: "} + msg);
    }
}

Resampler::~Resampler() {
    if (soxr_) {
        soxr_delete(soxr_);
        soxr_ = nullptr;
    }
}

Resampler::Resampler(Resampler&& other) noexcept
    : soxr_(other.soxr_),
      in_rate_(other.in_rate_),
      out_rate_(other.out_rate_),
      quality_(other.quality_),
      in_samples_(other.in_samples_),
      out_samples_(other.out_samples_) {
    other.soxr_ = nullptr;
}

Resampler& Resampler::operator=(Resampler&& other) noexcept {
    if (this != &other) {
        if (soxr_) soxr_delete(soxr_);
        soxr_       = std::exchange(other.soxr_, nullptr);
        in_rate_    = other.in_rate_;
        out_rate_   = other.out_rate_;
        quality_    = other.quality_;
        in_samples_ = other.in_samples_;
        out_samples_= other.out_samples_;
    }
    return *this;
}

std::vector<std::int16_t> Resampler::process(std::span<const std::int16_t> in) {
    if (in.empty()) return {};
    const std::size_t out_cap = output_capacity(in.size(), in_rate_, out_rate_);
    std::vector<std::int16_t> out(out_cap);

    std::size_t idone = 0;
    std::size_t odone = 0;
    soxr_error_t err = soxr_process(soxr_,
        in.data(),  in.size(),  &idone,
        out.data(), out_cap,    &odone);
    if (err != nullptr) {
        // Mirror soxr's contract: short read on error is fine; bubbling
        // the message lets the caller decide whether to abort the turn.
        throw std::runtime_error(std::string{"resampler: process: "} + err);
    }
    in_samples_  += idone;
    out_samples_ += odone;
    out.resize(odone);
    return out;
}

std::vector<std::int16_t> Resampler::flush() {
    // Pull until soxr returns nothing. Each iteration grants soxr a
    // generous output buffer; in practice flush returns ≤ a few hundred
    // samples even at HQ.
    std::vector<std::int16_t> tail;
    std::vector<std::int16_t> chunk(2048);
    while (true) {
        std::size_t odone = 0;
        soxr_error_t err = soxr_process(soxr_,
            /*in=*/nullptr, 0, /*idone=*/nullptr,
            chunk.data(), chunk.size(), &odone);
        if (err != nullptr) {
            throw std::runtime_error(std::string{"resampler: flush: "} + err);
        }
        if (odone == 0) break;
        tail.insert(tail.end(), chunk.begin(), chunk.begin()
                                   + static_cast<std::ptrdiff_t>(odone));
        out_samples_ += odone;
    }
    return tail;
}

} // namespace acva::audio
