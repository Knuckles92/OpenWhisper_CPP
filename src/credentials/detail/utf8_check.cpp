#include "credentials/detail/utf8_check.h"

#include <format>

namespace fv::credentials_detail {

std::optional<Utf8Error> check_utf8(std::string_view bytes) {
    const auto at = [&](std::size_t i) {
        return static_cast<unsigned char>(bytes[i]);
    };
    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char lead = at(i);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        // The second byte's range rules out overlong forms, surrogates and values past U+10FFFF.
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            low = lead == 0xE0 ? 0xA0 : 0x80;
            high = lead == 0xED ? 0x9F : 0xBF;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            low = lead == 0xF0 ? 0x90 : 0x80;
            high = lead == 0xF4 ? 0x8F : 0xBF;
        } else {
            return Utf8Error{i, i + 1, lead, "invalid start byte"};
        }
        for (std::size_t k = 1; k < length; ++k) {
            if (i + k >= bytes.size()) {
                return Utf8Error{i, bytes.size(), lead, "unexpected end of data"};
            }
            const unsigned char next = at(i + k);
            const unsigned char min = k == 1 ? low : 0x80;
            const unsigned char max = k == 1 ? high : 0xBF;
            if (next < min || next > max) {
                return Utf8Error{i, i + k, lead, "invalid continuation byte"};
            }
        }
        i += length;
    }
    return std::nullopt;
}

std::string describe(const Utf8Error& error) {
    if (error.end - error.position <= 1) {
        return std::format("'utf-8' codec can't decode byte 0x{:02x} in position {}: {}",
                           error.byte, error.position, error.reason);
    }
    return std::format("'utf-8' codec can't decode bytes in position {}-{}: {}", error.position,
                       error.end - 1, error.reason);
}

} // namespace fv::credentials_detail
