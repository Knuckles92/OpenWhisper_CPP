#include "walkthrough/detail/pcm_wav.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include "audio/wav_writer.h"

namespace fv::walkthrough {

namespace detail {

namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
constexpr std::array<unsigned char, 16> kSubtypePcm{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

std::uint32_t le32(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24;
}

std::uint16_t le16(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) |
                                      static_cast<unsigned char>(bytes[at + 1]) << 8);
}

// The file as Python's wave reads it: the RIFF chunk is itself a chunk, and every sub-chunk is
// read through it, so nothing past its declared size (or the file's end) is ever read.
class Riff {
public:
    explicit Riff(std::string bytes) : bytes_(std::move(bytes)) {}

    // _Chunk(file) on the file itself: the RIFF header.
    void open() {
        if (bytes_.size() < 4) {
            throw WaveTruncated("EOFError");
        }
        name_ = bytes_.substr(0, 4);
        if (bytes_.size() < 8) {
            throw WaveTruncated("EOFError");
        }
        size_ = le32(bytes_, 4);
    }
    const std::string& name() const { return name_; }

    // _Chunk.read on the RIFF chunk: at most n bytes, never past its size or the file.
    std::string read(std::uint64_t n) {
        if (read_ >= size_) {
            return {};
        }
        n = std::min<std::uint64_t>(n, size_ - read_);
        const std::uint64_t at = 8 + read_;
        std::string data;
        if (at < bytes_.size()) {
            data = bytes_.substr(
                static_cast<std::size_t>(at),
                static_cast<std::size_t>(std::min<std::uint64_t>(n, bytes_.size() - at)));
        }
        read_ += data.size();
        if (read_ == size_ && (size_ & 1U) != 0) {
            read_ += (8 + read_ < bytes_.size()) ? 1 : 0; // the pad byte
        }
        return data;
    }

    // _Chunk.seek(n, 1) on the RIFF chunk: RuntimeError past its declared end.
    void skip(std::uint64_t n) {
        if (read_ + n > size_) {
            throw WaveTruncated("RuntimeError");
        }
        read_ += n;
    }

private:
    std::string bytes_;
    std::string name_;
    std::uint64_t size_ = 0;
    std::uint64_t read_ = 0;
};

// A sub-chunk read through the RIFF chunk (wave's _Chunk(self._file)).
class Chunk {
public:
    // EOFError (a short header) ends the chunk list, as wave's loop does.
    static std::optional<Chunk> next(Riff& riff) {
        Chunk chunk(riff);
        chunk.name_ = riff.read(4);
        if (chunk.name_.size() < 4) {
            return std::nullopt;
        }
        const std::string size = riff.read(4);
        if (size.size() < 4) {
            return std::nullopt;
        }
        chunk.size_ = le32(size, 0);
        return chunk;
    }

    const std::string& name() const { return name_; }
    std::uint32_t size() const { return size_; }

    std::string read(std::uint64_t n) {
        if (read_ >= size_) {
            return {};
        }
        n = std::min<std::uint64_t>(n, size_ - read_);
        std::string data = riff_->read(n);
        read_ += data.size();
        if (read_ == size_ && (size_ & 1U) != 0) {
            read_ += riff_->read(1).size();
        }
        return data;
    }

    void skip() {
        std::uint64_t n = size_ - std::min<std::uint64_t>(read_, size_);
        if ((size_ & 1U) != 0) {
            n += 1;
        }
        // A pad byte already read leaves read_ one past the size: nothing more to skip then.
        if (read_ > size_) {
            n -= std::min<std::uint64_t>(n, read_ - size_);
        }
        riff_->skip(n);
        read_ += n;
    }

private:
    explicit Chunk(Riff& riff) : riff_(&riff) {}

    Riff* riff_;
    std::string name_;
    std::uint32_t size_ = 0;
    std::uint64_t read_ = 0;
};

std::string uuid_from_bytes_le(std::string_view b) {
    const std::array<std::size_t, 16> order{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    std::string hex;
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            hex.push_back('-');
        }
        hex += std::format("{:02x}", static_cast<unsigned char>(b[order[index]]));
    }
    return hex;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::filesystem::filesystem_error(
            "Could not open", path, std::make_error_code(std::errc::no_such_file_or_directory));
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw std::filesystem::filesystem_error("Could not read", path,
                                                std::make_error_code(std::errc::io_error));
    }
    return bytes;
}

} // namespace

WaveFile read_wave(const std::filesystem::path& path) {
    Riff riff(read_file(path));
    riff.open();
    if (riff.name() != "RIFF") {
        throw WaveError("file does not start with RIFF id");
    }
    if (riff.read(4) != "WAVE") {
        throw WaveError("not a WAVE file");
    }
    WaveFile wave;
    bool fmt_read = false;
    std::optional<Chunk> data;
    int frame_size = 0;
    while (auto chunk = Chunk::next(riff)) {
        if (chunk->name() == "fmt ") {
            const std::string head = chunk->read(14);
            if (head.size() < 14) {
                throw WaveTruncated("EOFError");
            }
            const std::uint16_t tag = le16(head, 0);
            wave.channels = le16(head, 2);
            wave.rate = static_cast<int>(le32(head, 4));
            if (tag != kFormatPcm && tag != kFormatExtensible) {
                throw WaveError(std::format("unknown format: {}", tag));
            }
            const std::string bits = chunk->read(2);
            if (bits.size() < 2) {
                throw WaveTruncated("EOFError");
            }
            if (tag == kFormatExtensible) {
                if (chunk->read(8).size() < 8) {
                    throw WaveTruncated("EOFError");
                }
                const std::string subformat = chunk->read(16);
                if (subformat.size() < 16) {
                    throw WaveTruncated("EOFError");
                }
                if (!std::equal(kSubtypePcm.begin(), kSubtypePcm.end(), subformat.begin(),
                                [](unsigned char a, char b) {
                                    return a == static_cast<unsigned char>(b);
                                })) {
                    throw WaveError("unknown extended format: " + uuid_from_bytes_le(subformat));
                }
            }
            wave.sample_width = (le16(bits, 0) + 7) / 8;
            if (wave.sample_width == 0) {
                throw WaveError("bad sample width");
            }
            if (wave.channels == 0) {
                throw WaveError("bad # of channels");
            }
            frame_size = wave.channels * wave.sample_width;
            fmt_read = true;
        } else if (chunk->name() == "data") {
            if (!fmt_read) {
                throw WaveError("data chunk before fmt chunk");
            }
            data = std::move(chunk);
            break;
        }
        chunk->skip();
    }
    if (!fmt_read || !data) {
        throw WaveError("fmt chunk and/or data chunk missing");
    }
    const std::uint64_t frames = data->size() / static_cast<std::uint32_t>(frame_size);
    wave.pcm = data->read(frames * static_cast<std::uint64_t>(frame_size));
    wave.pcm.resize(wave.pcm.size() - wave.pcm.size() % static_cast<std::size_t>(frame_size));
    return wave;
}

void write_pcm16_wav(const std::filesystem::path& path, const Pcm16& audio,
                     std::span<const std::pair<std::int64_t, std::int64_t>> spans) {
    audio::WavWriter writer(path, {audio.rate, audio.channels, 2});
    for (const auto& [start, end] : spans) {
        const auto first = static_cast<std::size_t>(start * audio.channels);
        const auto count = static_cast<std::size_t>((end - start) * audio.channels);
        writer.write(std::span<const std::int16_t>(audio.samples.data() + first, count));
    }
    writer.commit(std::nullopt);
}

} // namespace detail

Pcm16 read_pcm16_wav(const std::filesystem::path& path) {
    const detail::WaveFile wave = detail::read_wave(path);
    if (wave.sample_width != 2) {
        throw std::invalid_argument(
            std::format("expected 16-bit audio, got {}-bit", 8 * wave.sample_width));
    }
    Pcm16 audio;
    audio.rate = wave.rate;
    audio.channels = wave.channels;
    audio.samples.resize(wave.pcm.size() / 2);
    for (std::size_t index = 0; index < audio.samples.size(); ++index) {
        audio.samples[index] = static_cast<std::int16_t>(detail::le16(wave.pcm, index * 2));
    }
    return audio;
}

} // namespace fv::walkthrough
