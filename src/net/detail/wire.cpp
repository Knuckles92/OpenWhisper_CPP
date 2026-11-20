#include "net/detail/wire.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace fv::net::detail {

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void append_replacement(std::string& out) {
    out += "\xEF\xBF\xBD";
}

// CPython's UTF-8 decoder with errors="replace": each maximal invalid subpart is one U+FFFD.
std::string decode_utf8_replace(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    const auto at = [&](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    while (i < bytes.size()) {
        const unsigned char lead = at(i);
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            ++i;
            continue;
        }
        std::size_t needed = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            needed = 1;
        } else if (lead == 0xE0) {
            needed = 2;
            low = 0xA0;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            needed = 2;
        } else if (lead == 0xED) {
            needed = 2;
            high = 0x9F;
        } else if (lead == 0xF0) {
            needed = 3;
            low = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            needed = 3;
        } else if (lead == 0xF4) {
            needed = 3;
            high = 0x8F;
        } else {
            append_replacement(out);
            ++i;
            continue;
        }
        std::size_t taken = 1;
        bool valid = true;
        for (std::size_t k = 0; k < needed; ++k) {
            const std::size_t index = i + 1 + k;
            const unsigned char lo = k == 0 ? low : 0x80;
            const unsigned char hi = k == 0 ? high : 0xBF;
            if (index >= bytes.size() || at(index) < lo || at(index) > hi) {
                valid = false;
                break;
            }
            ++taken;
        }
        if (valid) {
            out.append(bytes.substr(i, taken));
        } else {
            append_replacement(out);
        }
        i += taken;
    }
    return out;
}

} // namespace

std::string unquote(std::string_view text, bool plus_as_space) {
    std::string bytes;
    bytes.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size() && hex_digit(text[i + 1]) >= 0 &&
            hex_digit(text[i + 2]) >= 0) {
            bytes += static_cast<char>(hex_digit(text[i + 1]) * 16 + hex_digit(text[i + 2]));
            i += 2;
        } else if (c == '+' && plus_as_space) {
            bytes += ' ';
        } else {
            bytes += c;
        }
    }
    return decode_utf8_replace(bytes);
}

Query parse_query(std::string_view query) {
    Query pairs;
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto end = std::min(query.find('&', start), query.size());
        const auto field = query.substr(start, end - start);
        start = end + 1;
        if (field.empty()) {
            continue;
        }
        const auto equals = field.find('=');
        const auto name = field.substr(0, equals);
        const auto value =
            equals == std::string_view::npos ? std::string_view() : field.substr(equals + 1);
        pairs.emplace_back(unquote(name, true), unquote(value, true));
    }
    return pairs;
}

std::string quote(std::string_view text, std::string_view safe) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool always_safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                 (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
                                 byte == '-' || byte == '~';
        if (always_safe || (byte < 0x80 && safe.find(c) != std::string_view::npos)) {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0xf];
        }
    }
    return out;
}

std::string_view status_phrase(int status) {
    static constexpr std::pair<int, std::string_view> kPhrases[] = {
        {100, "Continue"},
        {101, "Switching Protocols"},
        {102, "Processing"},
        {103, "Early Hints"},
        {200, "OK"},
        {201, "Created"},
        {202, "Accepted"},
        {203, "Non-Authoritative Information"},
        {204, "No Content"},
        {205, "Reset Content"},
        {206, "Partial Content"},
        {207, "Multi-Status"},
        {208, "Already Reported"},
        {226, "IM Used"},
        {300, "Multiple Choices"},
        {301, "Moved Permanently"},
        {302, "Found"},
        {303, "See Other"},
        {304, "Not Modified"},
        {305, "Use Proxy"},
        {307, "Temporary Redirect"},
        {308, "Permanent Redirect"},
        {400, "Bad Request"},
        {401, "Unauthorized"},
        {402, "Payment Required"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {406, "Not Acceptable"},
        {407, "Proxy Authentication Required"},
        {408, "Request Timeout"},
        {409, "Conflict"},
        {410, "Gone"},
        {411, "Length Required"},
        {412, "Precondition Failed"},
        {413, "Request Entity Too Large"},
        {414, "Request-URI Too Long"},
        {415, "Unsupported Media Type"},
        {416, "Requested Range Not Satisfiable"},
        {417, "Expectation Failed"},
        {418, "I'm a Teapot"},
        {421, "Misdirected Request"},
        {422, "Unprocessable Entity"},
        {423, "Locked"},
        {424, "Failed Dependency"},
        {425, "Too Early"},
        {426, "Upgrade Required"},
        {428, "Precondition Required"},
        {429, "Too Many Requests"},
        {431, "Request Header Fields Too Large"},
        {451, "Unavailable For Legal Reasons"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {502, "Bad Gateway"},
        {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
        {505, "HTTP Version Not Supported"},
        {506, "Variant Also Negotiates"},
        {507, "Insufficient Storage"},
        {508, "Loop Detected"},
        {510, "Not Extended"},
        {511, "Network Authentication Required"},
    };
    for (const auto& [code, phrase] : kPhrases) {
        if (code == status) {
            return phrase;
        }
    }
    return {};
}

std::string http_date(std::chrono::system_clock::time_point when) {
    static constexpr std::array<const char*, 7> kDays = {"Sun", "Mon", "Tue", "Wed",
                                                         "Thu", "Fri", "Sat"};
    static constexpr std::array<const char*, 12> kMonths = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto seconds = std::chrono::floor<std::chrono::seconds>(when);
    const auto day = std::chrono::floor<std::chrono::days>(seconds);
    const std::chrono::year_month_day date(day);
    const std::chrono::weekday weekday(day);
    const std::chrono::hh_mm_ss time(seconds - day);
    return std::format("{}, {:02} {} {} {:02}:{:02}:{:02} GMT", kDays[weekday.c_encoding()],
                       static_cast<unsigned>(date.day()),
                       kMonths[static_cast<unsigned>(date.month()) - 1],
                       static_cast<int>(date.year()), time.hours().count(), time.minutes().count(),
                       time.seconds().count());
}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

} // namespace fv::net::detail
