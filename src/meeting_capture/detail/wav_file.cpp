#include "meeting_capture/detail/wav_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include "base/utf8.h"
#include "pyport/errors.h"

namespace fv::meeting::detail {

namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
// KSDATAFORMAT_SUBTYPE_PCM as it is stored: 00000001-0000-0010-8000-00aa00389b71.
constexpr std::array<std::uint8_t, 16> kSubtypePcm = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
// A fmt chunk is read whole; nothing a WAV writer produces comes near this.
constexpr std::uint32_t kMaxFormatBytes = 64 * 1024;

void put_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
    }
}

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8) & 0xFFU));
}

std::uint32_t get_u32(const std::byte* at) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | std::to_integer<std::uint32_t>(at[i]);
    }
    return value;
}

std::uint16_t get_u16(const std::byte* at) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(at[0]) |
                                      (std::to_integer<std::uint16_t>(at[1]) << 8));
}

bool id_is(const std::byte* at, const char* id) {
    return std::memcmp(at, id, 4) == 0;
}

// wave's _read_fmt_chunk.
void read_format(BinaryFile& file, std::uint32_t size, WavContents& out) {
    std::vector<std::byte> body(std::min(size, kMaxFormatBytes));
    const std::size_t got = file.read(body);
    if (got < 14) {
        throw WaveEofError("fmt chunk cut short");
    }
    const std::uint16_t tag = get_u16(body.data());
    if (tag != kFormatPcm && tag != kFormatExtensible) {
        throw WaveError(std::format("unknown format: {}", tag));
    }
    if (got < 16) {
        throw WaveEofError("fmt chunk cut short");
    }
    const std::uint16_t bits = get_u16(body.data() + 14);
    if (tag == kFormatExtensible) {
        if (got < 40) {
            throw WaveEofError("fmt chunk cut short");
        }
        const bool pcm = std::equal(kSubtypePcm.begin(), kSubtypePcm.end(), body.begin() + 24,
                                    [](std::uint8_t want, std::byte have) {
                                        return std::to_integer<std::uint8_t>(have) == want;
                                    });
        if (!pcm) {
            throw WaveError("unknown extended format");
        }
    }
    out.channels = get_u16(body.data() + 2);
    out.rate = static_cast<int>(get_u32(body.data() + 4));
    out.sample_width = (bits + 7) / 8;
    if (out.channels == 0) {
        throw WaveError("bad # of channels");
    }
    if (out.sample_width == 0) {
        throw WaveError("bad sample width");
    }
}

} // namespace

std::string wav_header(int rate, int channels, int sample_width, std::uint32_t data_bytes) {
    std::string out;
    out.reserve(44);
    out += "RIFF";
    put_u32(out, 36 + data_bytes);
    out += "WAVEfmt ";
    put_u32(out, 16);
    put_u16(out, kFormatPcm);
    put_u16(out, static_cast<std::uint16_t>(channels));
    put_u32(out, static_cast<std::uint32_t>(rate));
    put_u32(out, static_cast<std::uint32_t>(channels * rate * sample_width));
    put_u16(out, static_cast<std::uint16_t>(channels * sample_width));
    put_u16(out, static_cast<std::uint16_t>(sample_width * 8));
    out += "data";
    put_u32(out, data_bytes);
    return out;
}

WavOut::WavOut(const std::filesystem::path& path, int rate)
    : file_(BinaryFile::open(path, "wb")), rate_(rate) {
    file_.write(wav_header(rate_, 1, 2, 0));
}

void WavOut::write(std::span<const std::int16_t> samples) {
    write(std::as_bytes(samples));
}

void WavOut::write(std::span<const std::byte> frames) {
    file_.write(frames);
    data_bytes_ += frames.size();
}

void WavOut::close() {
    if (!file_.is_open()) {
        return;
    }
    file_.seek(0);
    file_.write(wav_header(rate_, 1, 2, static_cast<std::uint32_t>(data_bytes_)));
    file_.close();
}

WavContents read_wav(const std::filesystem::path& path) {
    WavReader reader(path);
    WavContents out;
    out.channels = reader.channels();
    out.sample_width = reader.sample_width();
    out.rate = reader.rate();
    out.frames = reader.read_frames(0, reader.frame_count());
    return out;
}

WavReader::WavReader(const std::filesystem::path& path) : file_(BinaryFile::open(path, "rb")) {
    std::array<std::byte, 12> riff{};
    const std::size_t head = file_.read(riff);
    if (head < 4 || !id_is(riff.data(), "RIFF")) {
        throw WaveError("file does not start with RIFF id");
    }
    if (head < 12 || !id_is(riff.data() + 8, "WAVE")) {
        throw WaveError("not a WAVE file");
    }
    WavContents format;
    bool have_format = false;
    std::uint64_t offset = 12;
    while (true) {
        std::array<std::byte, 8> header{};
        if (file_.read(header) < header.size()) {
            break; // wave's Chunk() raises EOFError, which ends the scan
        }
        const std::uint32_t size = get_u32(header.data() + 4);
        // Chunks are padded to an even size.
        const std::uint64_t next = offset + 8 + size + (size & 1U);
        if (id_is(header.data(), "fmt ")) {
            read_format(file_, size, format);
            have_format = true;
        } else if (id_is(header.data(), "data")) {
            if (!have_format) {
                throw WaveError("data chunk before fmt chunk");
            }
            channels_ = format.channels;
            sample_width_ = format.sample_width;
            rate_ = format.rate;
            data_offset_ = offset + 8;
            frame_count_ = size / (static_cast<std::uint64_t>(channels_) *
                                   static_cast<std::uint64_t>(sample_width_));
            return;
        }
        file_.seek(next);
        offset = next;
    }
    throw WaveError("fmt chunk and/or data chunk missing");
}

std::vector<std::byte> WavReader::read_frames(std::uint64_t position, std::uint64_t count) {
    if (position > frame_count_) {
        throw WaveError("position not in range");
    }
    const std::uint64_t frame_size =
        static_cast<std::uint64_t>(channels_) * static_cast<std::uint64_t>(sample_width_);
    const std::uint64_t frames = std::min(count, frame_count_ - position);
    std::vector<std::byte> out(static_cast<std::size_t>(frames * frame_size));
    file_.seek(data_offset_ + position * frame_size);
    out.resize(file_.read(out));
    return out;
}

std::pair<std::vector<std::int16_t>, int> load_wav_int16(const std::filesystem::path& path) {
    const WavContents wav = read_wav(path);
    if (wav.sample_width != 2) {
        throw py::ValueError(std::format("Expected 16-bit PCM WAV, got {}-bit: {}",
                                         wav.sample_width * 8, path_to_utf8(path)));
    }
    if (wav.frames.size() % 2 != 0) {
        // np.frombuffer refuses a buffer that is not a whole number of int16 values.
        throw py::ValueError("buffer size must be a multiple of element size");
    }
    const std::size_t total = wav.frames.size() / 2;
    std::vector<std::int16_t> samples(total);
    if (total > 0) {
        // An empty vector's data() may be null, which memcpy may not get even for zero bytes.
        std::memcpy(samples.data(), wav.frames.data(), total * 2);
    }
    if (wav.channels <= 1) {
        return {std::move(samples), wav.rate};
    }
    const auto width = static_cast<std::size_t>(wav.channels);
    const std::size_t frames = total / width;
    std::vector<std::int16_t> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        std::int32_t sum = 0;
        for (std::size_t c = 0; c < width; ++c) {
            sum += samples[i * width + c];
        }
        mono[i] = static_cast<std::int16_t>(sum / wav.channels);
    }
    return {std::move(mono), wav.rate};
}

} // namespace fv::meeting::detail
