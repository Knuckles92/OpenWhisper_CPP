#include "audio/wav_metadata.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

#include "audio/detail/raw_file.h"
#include "base/identity.h"

namespace fv::audio {

namespace {

using Bytes = std::vector<std::byte>;

// Byte ranges of the bext payload (EBU Tech 3285).
constexpr std::size_t kBextOriginator = 256;
constexpr std::size_t kBextOriginatorSize = 32;
constexpr std::size_t kBextDate = 320;
constexpr std::size_t kBextTime = 330;
constexpr std::size_t kBextTimeEnd = 338;
// Chunks read while looking for fmt; it is nearly always the first.
constexpr int kFormatSearchChunks = 16;
constexpr std::uint16_t kWaveFormatPcm = 1;

void append_text(Bytes& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

void append_u32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

std::uint32_t read_u32(const std::byte* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint16_t read_u16(const std::byte* data) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(data[0]) |
                                      (static_cast<unsigned>(data[1]) << 8));
}

bool same(const std::byte* data, std::string_view tag) {
    return std::memcmp(data, tag.data(), tag.size()) == 0;
}

Bytes riff_chunk(std::string_view id, const Bytes& payload) {
    Bytes chunk;
    append_text(chunk, id);
    append_u32(chunk, static_cast<std::uint32_t>(payload.size()));
    chunk.insert(chunk.end(), payload.begin(), payload.end());
    if (payload.size() & 1u) {
        chunk.push_back(std::byte{0});
    }
    return chunk;
}

Bytes zstring(std::string_view value) {
    Bytes bytes;
    append_text(bytes, value);
    bytes.push_back(std::byte{0});
    return bytes;
}

std::string date_text(std::chrono::local_seconds when) {
    return std::format("{:%Y-%m-%d}", when);
}

std::string time_text(std::chrono::local_seconds when) {
    return std::format("{:%H:%M:%S}", when);
}

Bytes list_info_chunk(std::chrono::local_seconds when) {
    Bytes items = riff_chunk("ICRD", zstring(date_text(when)));
    const Bytes software = riff_chunk("ISFT", zstring(identity().product));
    items.insert(items.end(), software.begin(), software.end());
    Bytes payload;
    append_text(payload, "INFO");
    payload.insert(payload.end(), items.begin(), items.end());
    return riff_chunk("LIST", payload);
}

Bytes bext_chunk(std::chrono::local_seconds when) {
    Bytes payload(kBextSize, std::byte{0});
    const std::string_view originator =
        identity().product.substr(0, std::min(identity().product.size(), kBextOriginatorSize));
    std::memcpy(payload.data() + kBextOriginator, originator.data(), originator.size());
    const std::string date = date_text(when);
    const std::string time = time_text(when);
    // Python assigns the slices; a longer text would grow the payload, which a year past 9999
    // alone could cause.
    std::memcpy(payload.data() + kBextDate, date.data(), std::min<std::size_t>(date.size(), 10));
    std::memcpy(payload.data() + kBextTime, time.data(), std::min<std::size_t>(time.size(), 8));
    return riff_chunk("bext", payload);
}

void write_riff_size(RiffFile& file, std::uint64_t file_size) {
    file.seek(4);
    Bytes size;
    append_u32(size, static_cast<std::uint32_t>(file_size - 8));
    std::span<const std::byte> rest(size);
    while (!rest.empty()) {
        const std::size_t written = file.write(rest);
        if (written == 0) {
            throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                                    "Recording storage accepted no more data");
        }
        rest = rest.subspan(written);
    }
    file.seek_end();
}

void write_all(RiffFile& file, std::span<const std::byte> data) {
    while (!data.empty()) {
        const std::size_t written = file.write(data);
        if (written == 0) {
            throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                                    "Recording storage accepted no more data");
        }
        data = data.subspan(written);
    }
}

bool is_wave(std::span<const std::byte> data) {
    return data.size() >= 12 && same(data.data(), "RIFF") && same(data.data() + 8, "WAVE");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                "Could not open the file");
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw std::system_error(std::make_error_code(std::errc::io_error),
                                "Could not read the file");
    }
    Bytes bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}

// _iter_chunks: (id, payload) for each whole chunk after the RIFF header.
template <class Visit> bool each_chunk(std::span<const std::byte> data, Visit visit) {
    std::size_t offset = 12;
    const std::size_t length = data.size();
    while (offset + 8 <= length) {
        const std::byte* id = data.data() + offset;
        const std::uint64_t size = read_u32(data.data() + offset + 4);
        const std::size_t start = offset + 8;
        const std::uint64_t end = start + size;
        if (end > length) {
            break;
        }
        if (visit(id, data.subspan(start, static_cast<std::size_t>(size)))) {
            return true;
        }
        offset = static_cast<std::size_t>(end + (size & 1u));
    }
    return false;
}

std::string ascii(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte b : bytes) {
        if (static_cast<unsigned>(b) > 0x7F) {
            throw WavMetadataError("The WAV tag is not ASCII text");
        }
        text.push_back(static_cast<char>(b));
    }
    return text;
}

std::optional<std::string> info_item_text(std::span<const std::byte> items, std::string_view tag) {
    std::size_t offset = 0;
    const std::size_t length = items.size();
    while (offset + 8 <= length) {
        const std::byte* id = items.data() + offset;
        const std::uint64_t size = read_u32(items.data() + offset + 4);
        const std::size_t start = offset + 8;
        const std::uint64_t end = start + size;
        if (end > length) {
            break;
        }
        if (same(id, tag)) {
            auto value = items.subspan(start, static_cast<std::size_t>(size));
            const auto nul = std::find(value.begin(), value.end(), std::byte{0});
            return ascii(value.first(static_cast<std::size_t>(nul - value.begin())));
        }
        offset = static_cast<std::size_t>(end + (size & 1u));
    }
    return std::nullopt;
}

std::optional<std::string> read_info_field(const std::filesystem::path& path,
                                           std::string_view tag) {
    const Bytes data = read_file(path);
    if (!is_wave(data)) {
        return std::nullopt;
    }
    std::optional<std::string> found;
    each_chunk(data, [&](const std::byte* id, std::span<const std::byte> payload) {
        if (!same(id, "LIST") || payload.size() < 4 || !same(payload.data(), "INFO")) {
            return false;
        }
        found = info_item_text(payload.subspan(4), tag);
        return true;
    });
    return found;
}

} // namespace

std::unique_ptr<RiffFile> open_riff_file(const std::filesystem::path& path) {
    return std::make_unique<detail::RawFile>(detail::RawFile::open_existing(path));
}

void append_origination(RiffFile& wav, std::chrono::local_seconds when,
                        std::uint64_t max_riff_size) {
    const std::uint64_t end = wav.seek_end();
    // Python's wave writer can omit the final pad byte for odd-length 8-bit PCM. Insert it
    // before adding another RIFF chunk.
    Bytes metadata;
    if (end & 1u) {
        metadata.push_back(std::byte{0});
    }
    const Bytes info = list_info_chunk(when);
    const Bytes bext = bext_chunk(when);
    metadata.insert(metadata.end(), info.begin(), info.end());
    metadata.insert(metadata.end(), bext.begin(), bext.end());
    if (end + metadata.size() - 8 > max_riff_size) {
        throw WavMetadataError("Recording is too large for RIFF metadata");
    }
    try {
        write_all(wav, metadata);
        write_riff_size(wav, end + metadata.size());
    } catch (...) {
        wav.truncate(end);
        write_riff_size(wav, end);
        throw;
    }
}

bool is_mono_pcm16(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::array<std::byte, 16> buffer{};
    const auto read = [&](std::size_t count) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
        return static_cast<std::size_t>(in.gcount()) == count;
    };
    if (!read(12) || !is_wave(std::span(buffer).first(12))) {
        return false;
    }
    for (int chunk = 0; chunk < kFormatSearchChunks; ++chunk) {
        if (!read(8)) {
            return false;
        }
        const std::uint32_t size = read_u32(buffer.data() + 4);
        if (same(buffer.data(), "fmt ")) {
            if (!read(16)) {
                return false;
            }
            const std::uint16_t tag = read_u16(buffer.data());
            const std::uint16_t channels = read_u16(buffer.data() + 2);
            const std::uint16_t bits = read_u16(buffer.data() + 14);
            return tag == kWaveFormatPcm && channels == 1 && bits == 16;
        }
        if (same(buffer.data(), "data")) {
            return false;
        }
        in.seekg(static_cast<std::streamoff>(size) + (size & 1u), std::ios::cur);
        if (!in) {
            return false;
        }
    }
    return false;
}

std::optional<std::string> read_info_icrd(const std::filesystem::path& path) {
    return read_info_field(path, "ICRD");
}

std::optional<std::string> read_info_isft(const std::filesystem::path& path) {
    return read_info_field(path, "ISFT");
}

std::optional<std::pair<std::string, std::string>>
read_bext_origination(const std::filesystem::path& path) {
    const Bytes data = read_file(path);
    if (!is_wave(data)) {
        return std::nullopt;
    }
    std::optional<std::pair<std::string, std::string>> found;
    each_chunk(data, [&](const std::byte* id, std::span<const std::byte> payload) {
        if (!same(id, "bext") || payload.size() < kBextTimeEnd) {
            return false;
        }
        found.emplace(ascii(payload.subspan(kBextDate, kBextTime - kBextDate)),
                      ascii(payload.subspan(kBextTime, kBextTimeEnd - kBextTime)));
        return true;
    });
    return found;
}

} // namespace fv::audio
