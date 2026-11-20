#include "walkthrough/detail/temp_folder.h"

#include <array>
#include <cstdint>
#include <random>
#include <string_view>
#include <system_error>

#include "base/utf8.h"
#include "walkthrough/detail/text_io.h"

namespace fv::walkthrough::detail {

TemporaryFolder::TemporaryFolder(const std::string& prefix, const std::filesystem::path& dir) {
    static constexpr std::string_view kCharacters = "abcdefghijklmnopqrstuvwxyz0123456789_";
    std::random_device seed;
    std::mt19937 random(seed());
    std::uniform_int_distribution<std::size_t> pick(0, kCharacters.size() - 1);
    const auto base = dir.empty() ? std::filesystem::temp_directory_path() : dir;
    for (int attempt = 0;; ++attempt) {
        std::string name = prefix;
        for (int index = 0; index < 8; ++index) {
            name.push_back(kCharacters[pick(random)]);
        }
        path_ = base / path_from_utf8(name);
        std::error_code error;
        if (std::filesystem::create_directory(path_, error)) {
            return;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error("Could not create a temporary folder", path_,
                                                    error);
        }
        if (attempt > 10000) {
            throw std::filesystem::filesystem_error("No usable temporary folder name", base,
                                                    std::make_error_code(std::errc::file_exists));
        }
    }
}

TemporaryFolder::~TemporaryFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

namespace {

std::uint32_t crc32(std::string_view data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t crc = index;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            }
            values[index] = crc;
        }
        return values;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char c : data) {
        crc = table[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint32_t read32(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

} // namespace

bool readable_png(const std::filesystem::path& path) {
    std::string bytes;
    try {
        bytes = read_bytes(path);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
    static constexpr std::string_view kSignature = "\x89PNG\r\n\x1a\n";
    if (!std::string_view(bytes).starts_with(kSignature)) {
        return false;
    }
    std::size_t at = kSignature.size();
    bool header = false;
    bool data = false;
    while (at + 12 <= bytes.size()) {
        const std::uint32_t length = read32(bytes, at);
        if (length > bytes.size() - at - 12) {
            return false;
        }
        const std::string_view body(bytes.data() + at + 4, length + 4);
        if (crc32(body) != read32(bytes, at + 8 + length)) {
            return false;
        }
        const std::string_view type = body.substr(0, 4);
        if (!header) {
            // IHDR comes first, 13 bytes, with a width and a height.
            if (type != "IHDR" || length != 13 || read32(bytes, at + 8) == 0 ||
                read32(bytes, at + 12) == 0) {
                return false;
            }
            header = true;
        } else if (type == "IDAT") {
            data = true;
        } else if (type == "IEND") {
            return data;
        }
        at += 12 + length;
    }
    return false;
}

} // namespace fv::walkthrough::detail
