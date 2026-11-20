#pragma once

// The test signals the golden case files tools/parity/cases/services.streaming_transcriber.py
// and services.local_asr.audio.py name instead of storing: integer arithmetic only, so both
// sides build the same samples bit for bit.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace fv::dsp::test {

// 64-bit LCG; each step's top 16 bits as int16.
inline std::vector<std::int16_t> lcg_int16(std::size_t count, std::uint64_t seed) {
    std::vector<std::int16_t> out(count);
    std::uint64_t state = seed;
    for (auto& value : out) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        value = static_cast<std::int16_t>(static_cast<std::int64_t>(state >> 48) - 32768);
    }
    return out;
}

// services.streaming_transcriber cases: the float32 input of fft_resample.
inline std::vector<float> resample_signal(const std::string& kind, std::size_t count,
                                          std::uint64_t seed) {
    std::vector<float> out(count, 0.0f);
    if (kind == "noise") {
        const auto noise = lcg_int16(count, seed);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<float>(noise[i]) / 32768.0f;
        }
    } else if (kind == "dc") {
        out.assign(count, 0.25f);
    } else if (kind == "alternating") {
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = i % 2 == 0 ? 0.5f : -0.5f;
        }
    } else if (kind == "impulse") {
        if (count > 0) {
            out[seed % count] = 1.0f;
        }
    } else {
        throw std::invalid_argument("unknown signal kind " + kind);
    }
    return out;
}

// services.local_asr.audio cases: a 16 kHz int16 stream.
inline std::vector<std::int16_t> stream_signal(const std::string& kind, std::size_t count,
                                               std::int64_t seed) {
    constexpr std::int64_t block = 1600;
    const auto amp = [seed](std::int64_t index) {
        return (index / block * 7919 + seed) % 64 + 1;
    };
    std::vector<std::int16_t> out(count, 0);
    if (kind == "silence") {
        return out;
    }
    if (kind == "constant") {
        out.assign(count, static_cast<std::int16_t>(seed));
        return out;
    }
    if (kind == "noise" || kind == "gaps") {
        const auto noise = lcg_int16(count, static_cast<std::uint64_t>(seed));
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::int64_t>(i);
            // >> on a negative value is an arithmetic (floor) shift since C++20, as numpy's.
            std::int64_t value = (static_cast<std::int64_t>(noise[i]) * amp(index)) >> 6;
            if (kind == "gaps" && (index / block) % 23 == 17) {
                value = 0;
            }
            out[i] = static_cast<std::int16_t>(value);
        }
        return out;
    }
    if (kind == "sine") {
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::int64_t>(i);
            const std::int64_t phase = index % 160;
            const std::int64_t q = phase % 80;
            // Bhaskara I's approximation of sin(pi * q / 80), peak 32000.
            std::int64_t base = (16 * q * (80 - q) * 32000) / (5 * 80 * 80 - 4 * q * (80 - q));
            if (phase >= 80) {
                base = -base;
            }
            out[i] = static_cast<std::int16_t>((base * amp(index)) >> 6);
        }
        return out;
    }
    throw std::invalid_argument("unknown signal kind " + kind);
}

} // namespace fv::dsp::test
