#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "base/json.h"

namespace fv::remote::test {

inline std::vector<std::uint8_t> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("odd hex");
    }
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        throw std::invalid_argument("not hex");
    };
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
    return out;
}

inline std::vector<std::byte> bytes_from_hex(std::string_view hex) {
    std::vector<std::byte> out;
    for (const auto value : from_hex(hex)) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

inline std::string hex_of(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        out += kDigits[value >> 4];
        out += kDigits[value & 0xF];
    }
    return out;
}

inline std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

// A fresh folder under the system's temporary folder, removed with everything in it.
class TempFolder {
public:
    TempFolder() {
        std::random_device random;
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = base / ("fv-remote-test-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("no temporary folder");
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(std::string_view name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

} // namespace fv::remote::test
