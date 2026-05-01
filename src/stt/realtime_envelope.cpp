#include "stt/realtime_envelope.hpp"

#include <glaze/glaze.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace acva::stt::realtime {

namespace {

struct Envelope {
    std::string                id;
    std::string                type;
    std::string                data;
    std::optional<std::size_t> fragment_index;
    std::optional<std::size_t> total_fragments;
};

constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};

constexpr std::array<std::int8_t, 256> kBase64Lookup = [] {
    std::array<std::int8_t, 256> t{};
    for (auto& b : t) b = -1;
    constexpr std::string_view alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        t[static_cast<unsigned char>(alpha[i])] =
            static_cast<std::int8_t>(i);
    }
    return t;
}();

} // namespace

std::optional<std::string> base64_decode(std::string_view input) {
    if (input.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(input.size() * 3 / 4);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const auto a = kBase64Lookup[static_cast<unsigned char>(input[i])];
        const auto b = kBase64Lookup[static_cast<unsigned char>(input[i + 1])];
        const char c_ch = input[i + 2];
        const char d_ch = input[i + 3];
        if (a < 0 || b < 0) return std::nullopt;
        const std::int32_t triple =
            (static_cast<std::int32_t>(a) << 18) |
            (static_cast<std::int32_t>(b) << 12);
        out.push_back(static_cast<char>((triple >> 16) & 0xFF));

        if (c_ch == '=') {
            if (d_ch != '=') return std::nullopt;
            // 1 byte payload — already wrote above (and the next byte
            // would come from `b`'s low bits; but only the top 4 bits
            // of `b` are payload here; mask not strictly needed for
            // round-trip but guard anyway).
            if ((b & 0x0F) != 0) return std::nullopt;
            break;
        }
        const auto c = kBase64Lookup[static_cast<unsigned char>(c_ch)];
        if (c < 0) return std::nullopt;
        const std::int32_t with_c = triple | (static_cast<std::int32_t>(c) << 6);
        out.push_back(static_cast<char>((with_c >> 8) & 0xFF));

        if (d_ch == '=') {
            if ((c & 0x03) != 0) return std::nullopt;
            break;
        }
        const auto d = kBase64Lookup[static_cast<unsigned char>(d_ch)];
        if (d < 0) return std::nullopt;
        const std::int32_t with_d = with_c | static_cast<std::int32_t>(d);
        out.push_back(static_cast<char>(with_d & 0xFF));
    }
    return out;
}

std::optional<std::string> EnvelopeReassembler::feed(std::string_view raw) {
    Envelope env;
    if (auto ec = glz::read<kReadOpts>(env, raw); ec) {
        return std::nullopt;
    }
    if (env.type == "full_message") {
        return base64_decode(env.data);
    }
    if (env.type != "partial_message") {
        return std::nullopt;
    }
    if (!env.fragment_index || !env.total_fragments
        || *env.total_fragments == 0
        || *env.fragment_index >= *env.total_fragments) {
        return std::nullopt;
    }

    auto [it, inserted] = pending_.try_emplace(env.id);
    Pending& p = it->second;
    if (inserted) {
        p.fragments.assign(*env.total_fragments, std::string{});
    } else if (p.fragments.size() != *env.total_fragments) {
        // Server changed its mind about total_fragments mid-stream;
        // protocol violation — drop the partial state for this id.
        pending_.erase(it);
        return std::nullopt;
    }

    const std::size_t idx = *env.fragment_index;
    if (!p.fragments[idx].empty()) {
        // Duplicate fragment — ignore.
        return std::nullopt;
    }
    p.fragments[idx] = std::move(env.data);
    ++p.received;

    if (p.received == p.fragments.size()) {
        std::string encoded;
        std::size_t total_size = 0;
        for (const auto& f : p.fragments) total_size += f.size();
        encoded.reserve(total_size);
        for (auto& f : p.fragments) encoded.append(f);
        pending_.erase(it);
        return base64_decode(encoded);
    }
    return std::nullopt;
}

} // namespace acva::stt::realtime
