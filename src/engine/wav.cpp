#include "engine/wav.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>

#include "engine/error.h"
#include "engine/platform.h"

namespace fv {

namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool has_tag(const std::uint8_t* p, std::string_view tag) {
    return std::memcmp(p, tag.data(), 4) == 0;
}

struct Format {
    std::uint16_t tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits = 0;
};

float decode_sample(const std::uint8_t* p, const Format& format) {
    if (format.tag == kFormatFloat) {
        if (format.bits == 32) {
            float value;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }
        double value;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<float>(value);
    }
    switch (format.bits) {
    case 8:
        // 8-bit WAVE is unsigned, centred on 128.
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16:
        return static_cast<std::int16_t>(read_u16(p)) / 32768.0f;
    case 24: {
        const std::uint32_t raw = static_cast<std::uint32_t>(p[0]) |
                                  (static_cast<std::uint32_t>(p[1]) << 8) |
                                  (static_cast<std::uint32_t>(p[2]) << 16);
        // Shift the 24-bit value to the top of 32 bits to carry its sign.
        return static_cast<float>(static_cast<std::int32_t>(raw << 8) / 2147483648.0);
    }
    default:
        return static_cast<float>(static_cast<std::int32_t>(read_u32(p)) / 2147483648.0);
    }
}

} // namespace

AudioBuffer parse_wav(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || !has_tag(bytes.data(), "RIFF") || !has_tag(bytes.data() + 8, "WAVE")) {
        throw EngineError("This is not a WAV file.");
    }

    Format format;
    bool have_format = false;
    std::span<const std::uint8_t> data;
    bool have_data = false;

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const std::uint8_t* header = bytes.data() + offset;
        const std::size_t declared = read_u32(header + 4);
        const std::size_t available = bytes.size() - offset - 8;
        // A recorder that was cut off leaves a data size of 0 or 0xFFFFFFFF;
        // either way the chunk runs to the end of the file.
        const std::size_t size = std::min(declared, available);
        const std::uint8_t* body = header + 8;

        if (has_tag(header, "fmt ")) {
            if (size < 16) {
                throw EngineError("The WAV file's format chunk is too short.");
            }
            format.tag = read_u16(body);
            format.channels = read_u16(body + 2);
            format.sample_rate = read_u32(body + 4);
            format.bits = read_u16(body + 14);
            if (format.tag == kFormatExtensible && size >= 26) {
                // The real format is the first two bytes of the sub-format GUID.
                format.tag = read_u16(body + 24);
            }
            have_format = true;
        } else if (has_tag(header, "data")) {
            const bool open_ended = declared == 0 || declared == 0xFFFFFFFFu;
            data = std::span<const std::uint8_t>(body, open_ended ? available : size);
            have_data = true;
            break;
        }
        // Chunks are padded to an even length.
        offset += 8 + size + (size & 1);
    }

    if (!have_format) {
        throw EngineError("The WAV file has no format chunk.");
    }
    if (!have_data) {
        throw EngineError("The WAV file has no audio data.");
    }
    const bool pcm = format.tag == kFormatPcm && (format.bits == 8 || format.bits == 16 ||
                                                  format.bits == 24 || format.bits == 32);
    const bool floating = format.tag == kFormatFloat && (format.bits == 32 || format.bits == 64);
    if (!pcm && !floating) {
        throw EngineError("Unsupported WAV encoding (format " + std::to_string(format.tag) + ", " +
                          std::to_string(format.bits) + "-bit). Use PCM or float WAV.");
    }
    if (format.channels == 0 || format.sample_rate == 0) {
        throw EngineError("The WAV file has no channels or no sample rate.");
    }

    const std::size_t sample_bytes = format.bits / 8u;
    const std::size_t frame_bytes = sample_bytes * format.channels;
    const std::size_t frames = data.size() / frame_bytes;

    AudioBuffer audio;
    audio.sample_rate = static_cast<int>(format.sample_rate);
    audio.samples.resize(frames);
    const float scale = 1.0f / format.channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t* p = data.data() + frame * frame_bytes;
        float sum = 0.0f;
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            sum += decode_sample(p + channel * sample_bytes, format);
        }
        audio.samples[frame] = sum * scale;
    }
    return audio;
}

AudioBuffer read_wav(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw EngineError("Could not open " + path_to_utf8(path) + ".");
    }
    const std::streamoff size = file.tellg();
    file.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max<std::streamoff>(size, 0)));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()),
                                     static_cast<std::streamsize>(bytes.size()))) {
        throw EngineError("Could not read " + path_to_utf8(path) + ".");
    }
    return parse_wav(bytes);
}

} // namespace fv
