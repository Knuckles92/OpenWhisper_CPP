#include "speech/detail/wav_pcm.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>

namespace fv::speech::detail {

namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
// KSDATAFORMAT_SUBTYPE_PCM as wave compares it.
constexpr std::array<std::uint8_t, 16> kSubtypePcm{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                   0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

std::uint32_t u32(const char* bytes) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[0])) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[3])) << 24;
}

std::uint16_t u16(const char* bytes) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[0]) |
                                      static_cast<std::uint8_t>(bytes[1]) << 8);
}

// chunk.Chunk over a file: the header, then reads bounded by the chunk's size.
class Chunk {
public:
    // nullopt at the end of the enclosing data (Chunk's EOFError).
    static std::optional<Chunk> open(std::ifstream& file, std::uint64_t limit) {
        char header[8];
        if (limit < 8) {
            return std::nullopt;
        }
        file.read(header, 8);
        if (file.gcount() < 8) {
            return std::nullopt;
        }
        Chunk chunk;
        chunk.name_.assign(header, 4);
        chunk.size_ = u32(header + 4);
        chunk.start_ = static_cast<std::uint64_t>(file.tellg());
        return chunk;
    }

    const std::string& name() const { return name_; }
    std::uint32_t size() const { return size_; }

    std::string read(std::ifstream& file, std::uint64_t count) {
        const std::uint64_t left = size_ - std::min<std::uint64_t>(position_, size_);
        count = std::min(count, left);
        std::string out(count, '\0');
        file.read(out.data(), static_cast<std::streamsize>(count));
        out.resize(static_cast<std::size_t>(file.gcount()));
        position_ += out.size();
        if (file.eof()) {
            file.clear();
        }
        return out;
    }

    // Past the chunk and its pad byte (Chunk's align=True).
    void skip(std::ifstream& file) const {
        std::uint64_t end = start_ + size_;
        if ((size_ & 1u) != 0) {
            ++end;
        }
        file.clear();
        file.seekg(static_cast<std::streamoff>(end));
    }

private:
    std::string name_;
    std::uint32_t size_ = 0;
    std::uint64_t start_ = 0;
    std::uint64_t position_ = 0;
};

} // namespace

WavPcm read_wav_pcm(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                "Cannot open the WAV file");
    }
    auto riff = Chunk::open(file, UINT64_MAX);
    if (!riff) {
        throw WavError("EOFError");
    }
    if (riff->name() != "RIFF") {
        throw WavError("file does not start with RIFF id");
    }
    if (riff->read(file, 4) != "WAVE") {
        throw WavError("not a WAVE file");
    }
    WavPcm wav;
    bool fmt_read = false;
    std::optional<Chunk> data;
    std::uint64_t used = 4;
    for (;;) {
        auto chunk = Chunk::open(file, riff->size() - std::min<std::uint64_t>(used, riff->size()));
        if (!chunk) {
            break;
        }
        if (chunk->name() == "fmt ") {
            const std::string head = chunk->read(file, 14);
            if (head.size() < 14) {
                throw WavError("EOFError");
            }
            const std::uint16_t tag = u16(head.data());
            wav.channels = u16(head.data() + 2);
            wav.rate = static_cast<int>(u32(head.data() + 4));
            if (tag != kFormatPcm && tag != kFormatExtensible) {
                throw WavError("unknown format: " + std::to_string(tag));
            }
            const std::string bits = chunk->read(file, 2);
            if (bits.size() < 2) {
                throw WavError("EOFError");
            }
            if (tag == kFormatExtensible) {
                const std::string extension = chunk->read(file, 8);
                const std::string subformat = chunk->read(file, 16);
                if (extension.size() < 8 || subformat.size() < 16) {
                    throw WavError("EOFError");
                }
                if (std::memcmp(subformat.data(), kSubtypePcm.data(), kSubtypePcm.size()) != 0) {
                    throw WavError("unknown extended format");
                }
            }
            wav.sample_width = (u16(bits.data()) + 7) / 8;
            if (wav.sample_width == 0) {
                throw WavError("bad sample width");
            }
            if (wav.channels == 0) {
                throw WavError("bad # of channels");
            }
            fmt_read = true;
        } else if (chunk->name() == "data") {
            if (!fmt_read) {
                throw WavError("data chunk before fmt chunk");
            }
            data = std::move(chunk);
            break;
        }
        used += 8 + chunk->size() + (chunk->size() & 1u);
        chunk->skip(file);
    }
    if (!fmt_read || !data) {
        throw WavError("fmt chunk and/or data chunk missing");
    }
    const std::int64_t frame_size = static_cast<std::int64_t>(wav.channels) * wav.sample_width;
    wav.frames = static_cast<std::int64_t>(data->size()) / frame_size;
    wav.pcm = data->read(file, static_cast<std::uint64_t>(wav.frames * frame_size));
    return wav;
}

} // namespace fv::speech::detail
