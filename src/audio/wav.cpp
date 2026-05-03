#include "audio/wav.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace acva::audio {

namespace {

std::string u32le(std::uint32_t v) {
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) {
        s[static_cast<std::size_t>(i)] =
            static_cast<char>((v >> (i * 8)) & 0xFF);
    }
    return s;
}

std::string u16le(std::uint16_t v) {
    std::string s(2, '\0');
    s[0] = static_cast<char>(v & 0xFF);
    s[1] = static_cast<char>((v >> 8) & 0xFF);
    return s;
}

std::uint32_t read_u32le(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t read_u16le(const unsigned char* p) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0])
      | (static_cast<std::uint16_t>(p[1]) << 8));
}

} // namespace

std::string make_wav(std::span<const std::int16_t> samples,
                      std::uint32_t sample_rate_hz) {
    constexpr std::uint32_t kBytesPerSample = 2;
    constexpr std::uint32_t kChannels       = 1;
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(samples.size()) * kBytesPerSample;
    const std::uint32_t fmt_chunk    = 16;
    const std::uint32_t riff_payload = 4 + 8 + fmt_chunk + 8 + data_bytes;

    std::string out;
    out.reserve(44 + data_bytes);
    out.append("RIFF");                     out.append(u32le(riff_payload));
    out.append("WAVE");
    out.append("fmt ");                     out.append(u32le(fmt_chunk));
    out.append(u16le(1));                   // PCM
    out.append(u16le(static_cast<std::uint16_t>(kChannels)));
    out.append(u32le(sample_rate_hz));
    out.append(u32le(sample_rate_hz * kChannels * kBytesPerSample));
    out.append(u16le(static_cast<std::uint16_t>(kChannels * kBytesPerSample)));
    out.append(u16le(static_cast<std::uint16_t>(kBytesPerSample * 8)));
    out.append("data");                     out.append(u32le(data_bytes));
    out.append(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return out;
}

bool write_wav_file(std::string_view path,
                     std::span<const std::int16_t> samples,
                     std::uint32_t sample_rate_hz) {
    const auto wav = make_wav(samples, sample_rate_hz);
    const std::string path_str(path);
    std::FILE* f = std::fopen(path_str.c_str(), "wb");
    if (!f) return false;
    const auto n = std::fwrite(wav.data(), 1, wav.size(), f);
    const bool ok = (n == wav.size());
    std::fclose(f);
    return ok;
}

std::vector<std::int16_t>
read_wav_file(std::string_view path, std::uint32_t& sample_rate_hz) {
    const std::string path_str(path);
    std::FILE* f = std::fopen(path_str.c_str(), "rb");
    if (!f) return {};

    std::vector<unsigned char> buf;
    buf.reserve(1 << 16);
    unsigned char chunk[4096];
    while (auto n = std::fread(chunk, 1, sizeof(chunk), f)) {
        buf.insert(buf.end(), chunk, chunk + n);
    }
    std::fclose(f);
    if (buf.size() < 44) return {};

    if (std::memcmp(buf.data(),     "RIFF", 4) != 0) return {};
    if (std::memcmp(buf.data() + 8, "WAVE", 4) != 0) return {};

    // Walk chunks looking for "fmt " then "data".
    std::size_t pos = 12;
    std::uint16_t channels   = 0;
    std::uint16_t bits       = 0;
    std::uint32_t rate       = 0;
    std::size_t   data_off   = 0;
    std::size_t   data_bytes = 0;
    while (pos + 8 <= buf.size()) {
        const auto* tag = buf.data() + pos;
        const auto sz   = read_u32le(buf.data() + pos + 4);
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            if (pos + 8 + 16 > buf.size()) return {};
            const auto fmt_code = read_u16le(buf.data() + pos + 8);
            if (fmt_code != 1) return {};        // PCM only
            channels = read_u16le(buf.data() + pos + 10);
            rate     = read_u32le(buf.data() + pos + 12);
            bits     = read_u16le(buf.data() + pos + 22);
        } else if (std::memcmp(tag, "data", 4) == 0) {
            data_off   = pos + 8;
            data_bytes = sz;
            break;
        }
        pos += 8 + sz + (sz & 1U);   // chunks are padded to even size
    }
    if (channels != 1 || bits != 16 || rate == 0
        || data_off == 0
        || data_off + data_bytes > buf.size()) {
        return {};
    }

    sample_rate_hz = rate;
    std::vector<std::int16_t> samples(data_bytes / 2);
    std::memcpy(samples.data(), buf.data() + data_off, data_bytes);
    return samples;
}

} // namespace acva::audio
