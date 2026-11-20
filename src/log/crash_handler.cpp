#include "log/crash_handler.h"

#include <fstream>
#include <iterator>
#include <vector>

#include "log/detail/crash_line.h"
#include "log/detail/files.h"

namespace fv::log {

namespace detail {

UtcFields utc_fields(std::int64_t unix_seconds) {
    // Howard Hinnant's civil_from_days, which needs nothing but integer arithmetic.
    std::int64_t days = unix_seconds / 86400;
    std::int64_t rest = unix_seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
    UtcFields fields;
    fields.day = static_cast<int>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
    fields.month = static_cast<int>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
    fields.year = static_cast<int>(year_of_era + era * 400 + (fields.month <= 2 ? 1 : 0));
    fields.hour = static_cast<int>(rest / 3600);
    fields.minute = static_cast<int>(rest % 3600 / 60);
    fields.second = static_cast<int>(rest % 60);
    return fields;
}

void LineBuffer::put(std::string_view text) {
    for (const char c : text) {
        if (size_ < capacity_) {
            data_[size_++] = c;
        }
    }
}

void LineBuffer::put_number(std::uint64_t value, int min_digits) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < 24);
    while (count < min_digits && count < 24) {
        digits[count++] = '0';
    }
    while (count > 0) {
        put(std::string_view(&digits[--count], 1));
    }
}

void LineBuffer::put_hex8(std::uint32_t value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        put(std::string_view(&kHex[(value >> shift) & 0xF], 1));
    }
}

void put_stamp(LineBuffer& out, const UtcFields& t) {
    out.put_number(static_cast<std::uint64_t>(t.year), 4);
    out.put_number(static_cast<std::uint64_t>(t.month), 2);
    out.put_number(static_cast<std::uint64_t>(t.day), 2);
    out.put("-");
    out.put_number(static_cast<std::uint64_t>(t.hour), 2);
    out.put_number(static_cast<std::uint64_t>(t.minute), 2);
    out.put_number(static_cast<std::uint64_t>(t.second), 2);
}

void put_crash_line(LineBuffer& out, const UtcFields& t, std::uint32_t code,
                    std::string_view module, std::string_view dump) {
    out.put_number(static_cast<std::uint64_t>(t.year), 4);
    out.put("-");
    out.put_number(static_cast<std::uint64_t>(t.month), 2);
    out.put("-");
    out.put_number(static_cast<std::uint64_t>(t.day), 2);
    out.put("T");
    out.put_number(static_cast<std::uint64_t>(t.hour), 2);
    out.put(":");
    out.put_number(static_cast<std::uint64_t>(t.minute), 2);
    out.put(":");
    out.put_number(static_cast<std::uint64_t>(t.second), 2);
    out.put("Z Fatal error 0x");
    out.put_hex8(code);
    out.put(" in ");
    out.put(module.empty() ? std::string_view("unknown") : module);
    if (dump.empty()) {
        out.put("; no minidump\n");
    } else {
        out.put("; minidump ");
        out.put(dump);
        out.put("\n");
    }
}

} // namespace detail

std::filesystem::path CrashHandler::crash_log_file(const Paths& paths,
                                                   const AppIdentity& identity) {
    return detail::with_suffix(paths.user_file(identity.log_file), ".crash.log");
}

std::filesystem::path CrashHandler::dump_folder(const Paths& paths) {
    return paths.data_root() / "crashes";
}

std::optional<CrashInfo> CrashHandler::parse_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    constexpr std::string_view kError = " Fatal error 0x";
    constexpr std::string_view kIn = " in ";
    constexpr std::string_view kDump = "; minidump ";
    constexpr std::string_view kNoDump = "; no minidump";
    const auto error_at = line.find(kError);
    if (error_at == std::string_view::npos || error_at == 0) {
        return std::nullopt;
    }
    CrashInfo info;
    info.utc = std::string(line.substr(0, error_at));
    std::string_view rest = line.substr(error_at + kError.size());
    if (rest.size() < 8) {
        return std::nullopt;
    }
    std::uint32_t code = 0;
    for (const char c : rest.substr(0, 8)) {
        code <<= 4;
        if (c >= '0' && c <= '9') {
            code |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            code |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
    }
    info.code = code;
    rest.remove_prefix(8);
    if (!rest.starts_with(kIn)) {
        return std::nullopt;
    }
    rest.remove_prefix(kIn.size());
    if (rest.ends_with(kNoDump)) {
        info.module = std::string(rest.substr(0, rest.size() - kNoDump.size()));
    } else if (const auto dump_at = rest.rfind(kDump); dump_at != std::string_view::npos) {
        info.module = std::string(rest.substr(0, dump_at));
        info.dump = std::string(rest.substr(dump_at + kDump.size()));
    } else {
        return std::nullopt;
    }
    return info;
}

std::optional<CrashInfo> CrashHandler::last_crash(const Paths& paths, const AppIdentity& identity) {
    std::ifstream in(crash_log_file(paths, identity), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<CrashInfo> last;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        if (auto info = parse_line(std::string_view(text).substr(start, end - start))) {
            last = std::move(info);
        }
        start = end + 1;
    }
    return last;
}

} // namespace fv::log
