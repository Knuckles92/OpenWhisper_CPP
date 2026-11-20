#include "settings/detail/file_text.h"

#include <cstddef>
#include <cstdio>

#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/settings_file.h"

namespace fv::detail {

namespace {

bool continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

// CPython's error codes from its UTF-8 decoder loop (Objects/stringlib/codecs.h): 0 the data
// ends inside a sequence, 1 an invalid start byte, 2..4 the first, second or third
// continuation byte is invalid.
struct Stop {
    int code = -1; // -1: decoded to the end
    std::size_t at = 0;
};

Stop scan(std::string_view bytes) {
    const auto* s = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char ch = s[i];
        const std::size_t left = n - i;
        if (ch < 0x80) {
            ++i;
        } else if (ch < 0xE0) {
            if (ch < 0xC2) {
                return {1, i};
            }
            if (left < 2) {
                return {0, i};
            }
            if (!continuation(s[i + 1])) {
                return {2, i};
            }
            i += 2;
        } else if (ch < 0xF0) {
            // E0 needs A0..BF next (no overlong forms), ED needs 80..9F (no surrogates).
            const auto bad_second = [&](unsigned char ch2) {
                return !continuation(ch2) || (ch2 < 0xA0 ? ch == 0xE0 : ch == 0xED);
            };
            if (left < 3) {
                if (left >= 2 && bad_second(s[i + 1])) {
                    return {2, i};
                }
                return {0, i};
            }
            if (bad_second(s[i + 1])) {
                return {2, i};
            }
            if (!continuation(s[i + 2])) {
                return {3, i};
            }
            i += 3;
        } else if (ch < 0xF5) {
            // F0 needs 90..BF next (no overlong forms), F4 needs 80..8F (nothing past U+10FFFF).
            const auto bad_second = [&](unsigned char ch2) {
                return !continuation(ch2) || (ch2 < 0x90 ? ch == 0xF0 : ch == 0xF4);
            };
            if (left < 4) {
                if (left >= 2 && bad_second(s[i + 1])) {
                    return {2, i};
                }
                if (left >= 3 && !continuation(s[i + 2])) {
                    return {3, i};
                }
                return {0, i};
            }
            if (bad_second(s[i + 1])) {
                return {2, i};
            }
            if (!continuation(s[i + 2])) {
                return {3, i};
            }
            if (!continuation(s[i + 3])) {
                return {4, i};
            }
            i += 4;
        } else {
            return {1, i};
        }
    }
    return {};
}

} // namespace

void check_utf8(std::string_view bytes) {
    const Stop stop = scan(bytes);
    if (stop.code < 0) {
        return;
    }
    const char* reason = "invalid continuation byte";
    std::size_t end = stop.at + static_cast<std::size_t>(stop.code) - 1;
    if (stop.code == 0) {
        reason = "unexpected end of data";
        end = bytes.size();
    } else if (stop.code == 1) {
        reason = "invalid start byte";
        end = stop.at + 1;
    }
    char text[160];
    if (end - stop.at == 1) {
        std::snprintf(
            text, sizeof text, "'utf-8' codec can't decode byte 0x%02x in position %zu: %s",
            static_cast<unsigned>(static_cast<unsigned char>(bytes[stop.at])), stop.at, reason);
    } else {
        std::snprintf(text, sizeof text, "'utf-8' codec can't decode bytes in position %zu-%zu: %s",
                      stop.at, end - 1, reason);
    }
    throw SettingsFileError("UnicodeDecodeError", text);
}

std::string universal_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

Json parse_settings(std::string_view bytes) {
    check_utf8(bytes);
    Json settings;
    try {
        settings = py::loads(universal_newlines(bytes));
    } catch (const py::PyError& error) {
        throw SettingsFileError(std::string(error.type_name()), error.what());
    }
    if (!settings.is_object()) {
        throw SettingsFileError("ValueError", "settings root must be a JSON object");
    }
    return settings;
}

std::string settings_text(const Json& settings) {
    py::DumpOptions options;
    options.indent = 2;
    const std::string text = py::dumps(settings, options) + "\n";
#if defined(_WIN32)
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char ch : text) {
        if (ch == '\n') {
            out.push_back('\r');
        }
        out.push_back(ch);
    }
    return out;
#else
    return text;
#endif
}

} // namespace fv::detail
