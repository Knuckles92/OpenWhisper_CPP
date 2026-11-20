#include "base/base64.h"

#include <array>
#include <cstdint>

namespace fv {

namespace {

constexpr std::string_view kStandard =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kUrlSafe =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::uint8_t kInvalid = 0xff;

constexpr std::array<std::uint8_t, 256> decode_table(bool urlsafe) {
    std::array<std::uint8_t, 256> table{};
    table.fill(kInvalid);
    for (std::size_t i = 0; i < kStandard.size(); ++i) {
        table[static_cast<unsigned char>(kStandard[i])] = static_cast<std::uint8_t>(i);
    }
    if (urlsafe) {
        table[static_cast<unsigned char>('-')] = 62;
        table[static_cast<unsigned char>('_')] = 63;
    }
    return table;
}

constexpr auto kStandardTable = decode_table(false);
constexpr auto kUrlSafeTable = decode_table(true);

} // namespace

std::string b64encode(std::span<const std::byte> bytes, bool urlsafe, bool pad) {
    const std::string_view alphabet = urlsafe ? kUrlSafe : kStandard;
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto n = (std::to_integer<std::uint32_t>(bytes[i]) << 16) |
                       (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8) |
                       std::to_integer<std::uint32_t>(bytes[i + 2]);
        out += alphabet[(n >> 18) & 0x3f];
        out += alphabet[(n >> 12) & 0x3f];
        out += alphabet[(n >> 6) & 0x3f];
        out += alphabet[n & 0x3f];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const auto n = std::to_integer<std::uint32_t>(bytes[i]) << 16;
        out += alphabet[(n >> 18) & 0x3f];
        out += alphabet[(n >> 12) & 0x3f];
        if (pad) {
            out += "==";
        }
    } else if (rest == 2) {
        const auto n = (std::to_integer<std::uint32_t>(bytes[i]) << 16) |
                       (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8);
        out += alphabet[(n >> 18) & 0x3f];
        out += alphabet[(n >> 12) & 0x3f];
        out += alphabet[(n >> 6) & 0x3f];
        if (pad) {
            out += '=';
        }
    }
    return out;
}

std::string b64encode(std::string_view bytes, bool urlsafe, bool pad) {
    return b64encode(std::as_bytes(std::span(bytes.data(), bytes.size())), urlsafe, pad);
}

// binascii.a2b_base64(text, strict_mode=True), step for step.
std::optional<std::vector<std::byte>> b64decode(std::string_view text, bool urlsafe) {
    const auto& table = urlsafe ? kUrlSafeTable : kStandardTable;
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    if (!text.empty() && text.front() == '=') {
        return std::nullopt; // "Leading padding not allowed"
    }
    int quad_pos = 0;
    int pads = 0;
    bool padding_started = false;
    std::uint8_t left = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch == '=') {
            padding_started = true;
            if (quad_pos == 0) {
                return std::nullopt; // "Excess padding not allowed"
            }
            if (quad_pos >= 2 && quad_pos + ++pads >= 4) {
                if (i + 1 < text.size()) {
                    return std::nullopt; // "Excess data after padding"
                }
                return out;
            }
            continue;
        }
        const std::uint8_t value = table[ch];
        if (value == kInvalid) {
            return std::nullopt; // "Only base64 data is allowed"
        }
        if (padding_started) {
            return std::nullopt; // "Discontinuous padding not allowed"
        }
        pads = 0;
        switch (quad_pos) {
        case 0:
            quad_pos = 1;
            left = value;
            break;
        case 1:
            quad_pos = 2;
            out.push_back(static_cast<std::byte>((left << 2) | (value >> 4)));
            left = value & 0x0f;
            break;
        case 2:
            quad_pos = 3;
            out.push_back(static_cast<std::byte>((left << 4) | (value >> 2)));
            left = value & 0x03;
            break;
        default:
            quad_pos = 0;
            out.push_back(static_cast<std::byte>((left << 6) | value));
            left = 0;
            break;
        }
    }
    if (quad_pos != 0) {
        return std::nullopt; // one character too many, or "Incorrect padding"
    }
    return out;
}

} // namespace fv
