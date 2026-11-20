#include "walkthrough/detail/text_io.h"

#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include "pyport/errors.h"

namespace fv::walkthrough::detail {

void check_utf8(std::string_view text) {
    const auto fail = [&](std::size_t at, std::string_view why) {
        throw py::ValueError(std::format("'utf-8' codec can't decode byte 0x{:02x} in position "
                                         "{}: {}",
                                         static_cast<unsigned char>(text[at]), at, why));
    };
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        if (lead < 0x80) {
            ++at;
            continue;
        }
        int length = 0;
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
            fail(at, "invalid start byte");
        }
        for (int index = 1; index < length; ++index) {
            if (at + index >= text.size()) {
                fail(at, "unexpected end of data");
            }
            const auto next = static_cast<unsigned char>(text[at + index]);
            const unsigned char min = index == 1 ? low : 0x80;
            const unsigned char max = index == 1 ? high : 0xBF;
            if (next < min || next > max) {
                fail(at, "invalid continuation byte");
            }
        }
        at += static_cast<std::size_t>(length);
    }
}

std::string universal_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '\r') {
            out.push_back('\n');
            if (at + 1 < text.size() && text[at + 1] == '\n') {
                ++at;
            }
        } else {
            out.push_back(text[at]);
        }
    }
    return out;
}

std::string read_bytes(const std::filesystem::path& path) {
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

std::string read_text(const std::filesystem::path& path) {
    const std::string bytes = read_bytes(path);
    check_utf8(bytes);
    return universal_newlines(bytes);
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::filesystem::filesystem_error("Could not write", path,
                                                std::make_error_code(std::errc::permission_denied));
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        throw std::filesystem::filesystem_error("Could not write", path,
                                                std::make_error_code(std::errc::io_error));
    }
}

std::vector<std::string> splitlines(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    bool open = false;
    std::size_t at = 0;
    const auto end_line = [&](std::size_t width) {
        lines.push_back(std::move(line));
        line.clear();
        open = false;
        at += width;
    };
    while (at < text.size()) {
        const auto c = static_cast<unsigned char>(text[at]);
        if (c == '\r') {
            end_line(at + 1 < text.size() && text[at + 1] == '\n' ? 2 : 1);
        } else if (c == '\n' || c == '\v' || c == '\f' || c == 0x1c || c == 0x1d || c == 0x1e) {
            end_line(1);
        } else if (c == 0xC2 && at + 1 < text.size() &&
                   static_cast<unsigned char>(text[at + 1]) == 0x85) {
            end_line(2); // U+0085
        } else if (c == 0xE2 && at + 2 < text.size() &&
                   static_cast<unsigned char>(text[at + 1]) == 0x80 &&
                   (static_cast<unsigned char>(text[at + 2]) == 0xA8 ||
                    static_cast<unsigned char>(text[at + 2]) == 0xA9)) {
            end_line(3); // U+2028, U+2029
        } else {
            line.push_back(text[at]);
            open = true;
            ++at;
        }
    }
    if (open) {
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string strip_chars(std::string_view text, std::string_view chars) {
    const auto first = text.find_first_not_of(chars);
    if (first == std::string_view::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(chars);
    return std::string(text.substr(first, last - first + 1));
}

} // namespace fv::walkthrough::detail
