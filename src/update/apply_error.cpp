#include "update/apply_error.h"

#include <cstdint>
#include <fstream>
#include <system_error>

#include "pyport/text.h"
#include "update/update_contract.h"

namespace fv::update {

namespace {

// Bytes enough for kApplyErrorLimit + 1 characters of four bytes each, plus a CR whose LF comes
// next.
constexpr std::size_t kReadBytes = (kApplyErrorLimit + 1) * 4 + 1;

// Python's universal newlines: "\r\n" and a lone "\r" read as "\n".
std::string translate_newlines(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            out += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++i;
            }
        } else {
            out += raw[i];
        }
    }
    return out;
}

// The length of the well-formed UTF-8 sequence at `at`, 0 for an ill-formed one (strict, as
// Python's codec: no overlong forms, surrogates or values past U+10FFFF).
std::size_t sequence_length(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) {
        return static_cast<std::uint8_t>(text[i]);
    };
    const auto lead = byte(at);
    std::size_t length = 0;
    std::uint8_t low = 0x80;
    std::uint8_t high = 0xBF;
    if (lead < 0x80) {
        return 1;
    }
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
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    if (byte(at + 1) < low || byte(at + 1) > high) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if (byte(at + i) < 0x80 || byte(at + i) > 0xBF) {
            return 0;
        }
    }
    return length;
}

// handle.read(limit): the first `limit` characters, or nullopt where decoding them fails.
std::optional<std::string> first_characters(std::string_view text, std::size_t limit) {
    std::size_t at = 0;
    for (std::size_t count = 0; count < limit && at < text.size(); ++count) {
        const auto length = sequence_length(text, at);
        if (length == 0) {
            return std::nullopt;
        }
        at += length;
    }
    return std::string(text.substr(0, at));
}

} // namespace

std::filesystem::path apply_error_path(const std::filesystem::path& updates_dir) {
    return updates_dir / kErrorName;
}

void write_apply_error(const std::filesystem::path& updates_dir, std::string_view message) {
    auto text = py::strip(message.empty() ? std::string_view("The update failed.") : message);
    text = py::slice(text, std::nullopt, static_cast<std::int64_t>(kApplyErrorLimit));
    std::filesystem::create_directories(updates_dir);
    // Text mode, as Python's open(..., "w") writes: "\n" is "\r\n" on Windows.
    std::ofstream out(apply_error_path(updates_dir), std::ios::out | std::ios::trunc);
    if (!out) {
        throw std::system_error(std::make_error_code(std::errc::io_error),
                                "Could not write the update error");
    }
    out << text << '\n';
    out.close();
    if (!out) {
        throw std::system_error(std::make_error_code(std::errc::io_error),
                                "Could not write the update error");
    }
}

std::optional<std::string> consume_apply_error(const std::filesystem::path& updates_dir) {
    const auto path = apply_error_path(updates_dir);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    std::string raw(kReadBytes, '\0');
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        if (in.bad()) {
            return std::nullopt;
        }
        raw.resize(static_cast<std::size_t>(in.gcount()));
    }
    const auto text = first_characters(translate_newlines(raw), kApplyErrorLimit + 1);
    if (!text) {
        return std::nullopt;
    }
    std::filesystem::remove(path, error);
    auto stripped = py::strip(*text);
    if (stripped.empty()) {
        return std::nullopt;
    }
    return py::slice(stripped, std::nullopt, static_cast<std::int64_t>(kApplyErrorLimit));
}

} // namespace fv::update
