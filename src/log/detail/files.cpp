#include "log/detail/files.h"

#include <fstream>
#include <iterator>
#include <system_error>

#include "pyport/datetime.h"
#include "pyport/json.h"

namespace fv::log::detail {

namespace {

// diagnostics._read_json skips anything larger, so a corrupted or hostile file costs nothing.
constexpr std::uintmax_t kMaxJsonBytes = 1024 * 1024;

} // namespace

std::string text_mode_bytes(std::string_view text) {
#if defined(_WIN32)
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (const char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
#else
    return std::string(text);
#endif
}

std::filesystem::path with_suffix(const std::filesystem::path& path, std::string_view suffix) {
    auto result = path;
    result.replace_extension(std::filesystem::path(std::u8string(suffix.begin(), suffix.end())));
    return result;
}

void write_text_replace(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = with_suffix(path, ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::string bytes = text_mode_bytes(text);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            throw std::filesystem::filesystem_error("Cannot write", temporary,
                                                    std::make_error_code(std::errc::io_error));
        }
    }
    std::filesystem::rename(temporary, path);
}

bool valid_utf8(std::string_view bytes) {
    std::size_t i = 0;
    const std::size_t n = bytes.size();
    while (i < n) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) {
                low = 0xA0;
            } else if (lead == 0xED) {
                high = 0x9F; // surrogates
            }
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) {
                low = 0x90;
            } else if (lead == 0xF4) {
                high = 0x8F;
            }
        } else {
            return false;
        }
        if (i + length > n) {
            return false;
        }
        const auto second = static_cast<unsigned char>(bytes[i + 1]);
        if (second < low || second > high) {
            return false;
        }
        for (std::size_t k = 2; k < length; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if (next < 0x80 || next > 0xBF) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

Json read_json_file(const std::filesystem::path& path) {
    try {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > kMaxJsonBytes) {
            return nullptr;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return nullptr;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        if (in.bad() || !valid_utf8(text)) {
            return nullptr;
        }
        return py::loads(text);
    } catch (const std::exception&) {
        // json.loads errors are ValueError and file errors OSError: both mean "no data".
        return nullptr;
    }
}

std::string utc_isoformat(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto micros = floor<microseconds>(time);
    const auto days = floor<std::chrono::days>(micros);
    const year_month_day date{days};
    const hh_mm_ss clock{micros - days};
    py::DateTime value;
    value.year = static_cast<int>(date.year());
    value.month = static_cast<int>(static_cast<unsigned>(date.month()));
    value.day = static_cast<int>(static_cast<unsigned>(date.day()));
    value.hour = static_cast<int>(clock.hours().count());
    value.minute = static_cast<int>(clock.minutes().count());
    value.second = static_cast<int>(clock.seconds().count());
    value.microsecond = static_cast<int>(clock.subseconds().count());
    value.utc_offset_us = 0;
    return value.isoformat();
}

} // namespace fv::log::detail
