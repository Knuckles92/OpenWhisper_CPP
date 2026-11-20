#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "base/utf8.h"
#include "pyport/datetime.h"

namespace fv::walkthrough::test {

// A new folder under the system temporary folder, removed with what it holds.
class TempFolder {
public:
    TempFolder() {
        std::random_device seed;
        const auto base = std::filesystem::temp_directory_path();
        for (;;) {
            path_ = base / ("fv-walkthrough-test-" + std::to_string(seed()));
            if (std::filesystem::create_directory(path_)) {
                break;
            }
        }
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(std::string_view name) const {
        return path_ / path_from_utf8(name);
    }

private:
    std::filesystem::path path_;
};

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A naive isoformat() time as local wall time.
inline std::chrono::local_seconds local_from_iso(const std::string& iso) {
    const auto value = py::fromisoformat(iso);
    return std::chrono::local_days{std::chrono::year{value.year} /
                                   static_cast<unsigned>(value.month) /
                                   static_cast<unsigned>(value.day)} +
           std::chrono::hours{value.hour} + std::chrono::minutes{value.minute} +
           std::chrono::seconds{value.second};
}

inline constexpr int RATE = 16'000;

// Python's tests' _tone: (8000 * sin(2 pi 220 t)).astype(int16), t = arange(int(s * RATE)) / RATE.
inline std::vector<std::int16_t> tone(double seconds, double scale = 1.0) {
    const auto count = static_cast<std::size_t>(seconds * RATE);
    std::vector<std::int16_t> samples(count);
    const double k = 2 * std::numbers::pi * 220;
    for (std::size_t index = 0; index < count; ++index) {
        const double t = static_cast<double>(index) / RATE;
        const auto value = static_cast<std::int16_t>(8000 * std::sin(k * t));
        samples[index] = static_cast<std::int16_t>(value * scale);
    }
    return samples;
}

inline std::vector<std::int16_t> quiet(double seconds) {
    return std::vector<std::int16_t>(static_cast<std::size_t>(seconds * RATE), 0);
}

inline std::vector<std::int16_t> concat(std::initializer_list<std::vector<std::int16_t>> parts) {
    std::vector<std::int16_t> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// A WAV as Python's wave module writes it: RIFF, a 16-byte PCM fmt chunk, data.
inline std::string wav_bytes(std::string_view data, int rate, int channels, int width) {
    std::string out;
    const auto put32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>(value >> shift));
        }
    };
    const auto put16 = [&](std::uint16_t value) {
        out.push_back(static_cast<char>(value));
        out.push_back(static_cast<char>(value >> 8));
    };
    out += "RIFF";
    put32(static_cast<std::uint32_t>(36 + data.size()));
    out += "WAVEfmt ";
    put32(16);
    put16(1);
    put16(static_cast<std::uint16_t>(channels));
    put32(static_cast<std::uint32_t>(rate));
    put32(static_cast<std::uint32_t>(rate * channels * width));
    put16(static_cast<std::uint16_t>(channels * width));
    put16(static_cast<std::uint16_t>(8 * width));
    out += "data";
    put32(static_cast<std::uint32_t>(data.size()));
    out += data;
    return out;
}

inline std::string pcm_bytes(const std::vector<std::int16_t>& samples) {
    std::string out;
    out.reserve(samples.size() * 2);
    for (const auto sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>(value >> 8));
    }
    return out;
}

inline void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples,
                      int channels = 1, int rate = RATE) {
    write_file(path, wav_bytes(pcm_bytes(samples), rate, channels, 2));
}

// The data chunk's samples and the header's counts of a 16-bit WAV.
struct WavInfo {
    int rate = 0;
    int channels = 0;
    int width = 0;
    std::int64_t frames = 0;
    std::vector<std::int16_t> samples;
};

inline WavInfo read_wav_info(const std::filesystem::path& path) {
    const std::string bytes = read_file(path);
    const auto u16 = [&](std::size_t at) {
        return static_cast<int>(static_cast<unsigned char>(bytes[at]) |
                                static_cast<unsigned char>(bytes[at + 1]) << 8);
    };
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(u16(at)) | static_cast<std::uint32_t>(u16(at + 2)) << 16;
    };
    WavInfo info;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::string name = bytes.substr(at, 4);
        const std::uint32_t size = u32(at + 4);
        if (name == "fmt ") {
            info.channels = u16(at + 10);
            info.rate = static_cast<int>(u32(at + 12));
            info.width = u16(at + 22) / 8;
        } else if (name == "data") {
            const std::size_t count = std::min<std::size_t>(size, bytes.size() - at - 8) / 2;
            for (std::size_t index = 0; index < count; ++index) {
                info.samples.push_back(static_cast<std::int16_t>(u16(at + 8 + index * 2)));
            }
            info.frames = info.channels > 0
                              ? static_cast<std::int64_t>(size) / (info.channels * info.width)
                              : 0;
            break;
        }
        at += 8 + size + (size & 1U);
    }
    return info;
}

inline int max_abs(const std::vector<std::int16_t>& samples) {
    int peak = 0;
    for (const auto sample : samples) {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    return peak;
}

// The golden cases' synthetic signals (tools/parity/cases/services.walkthrough.sections.py).
inline std::vector<std::int16_t> signal_channel(const Json& segments) {
    std::vector<std::int16_t> samples;
    for (const auto& segment : segments) {
        const std::string kind = segment.at(0).get<std::string>();
        const auto frames = segment.at(1).get<std::int64_t>();
        if (kind == "quiet") {
            samples.insert(samples.end(), static_cast<std::size_t>(frames), 0);
        } else if (kind == "square") {
            const auto amplitude = segment.at(2).get<std::int64_t>();
            const auto half = segment.at(3).get<std::int64_t>();
            for (std::int64_t i = 0; i < frames; ++i) {
                samples.push_back(
                    static_cast<std::int16_t>((i / half) % 2 == 0 ? amplitude : -amplitude));
            }
        } else if (kind == "noise") {
            const auto amplitude = segment.at(2).get<std::int64_t>();
            auto x = segment.at(3).get<std::int64_t>();
            for (std::int64_t i = 0; i < frames; ++i) {
                x = (1103515245LL * x + 12345) % (std::int64_t{1} << 31);
                samples.push_back(static_cast<std::int16_t>(x % (2 * amplitude + 1) - amplitude));
            }
        }
    }
    return samples;
}

struct Signal {
    int rate = 0;
    int channels = 0;
    std::vector<std::int16_t> interleaved;
};

inline Signal signal_from(const Json& spec) {
    Signal signal;
    signal.rate = spec.at("rate").get<int>();
    std::vector<std::vector<std::int16_t>> channels;
    for (const auto& channel : spec.at("channels")) {
        channels.push_back(signal_channel(channel));
    }
    signal.channels = static_cast<int>(channels.size());
    const std::size_t frames = channels.empty() ? 0 : channels.front().size();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (const auto& channel : channels) {
            signal.interleaved.push_back(channel[frame]);
        }
    }
    return signal;
}

} // namespace fv::walkthrough::test
