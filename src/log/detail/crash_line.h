#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Formatting for the crash log that is safe in a signal handler and on a crashing process's
// watcher thread: no allocation, no locale, no library time conversion.
namespace fv::log::detail {

struct UtcFields {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

// Seconds since 1970-01-01T00:00:00Z as a civil UTC date and time.
UtcFields utc_fields(std::int64_t unix_seconds);

// A bounded output buffer; writes past the end are dropped.
class LineBuffer {
public:
    LineBuffer(char* data, std::size_t capacity) : data_(data), capacity_(capacity) {}
    void put(std::string_view text);
    void put_number(std::uint64_t value, int min_digits = 1);
    void put_hex8(std::uint32_t value);
    std::size_t size() const { return size_; }

private:
    char* data_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

// "YYYYMMDD-HHMMSS"
void put_stamp(LineBuffer&, const UtcFields&);
// "<YYYY-MM-DDTHH:MM:SSZ> Fatal error 0x<code> in <module>; minidump <dump>\n", or
// "; no minidump\n" when dump is empty.
void put_crash_line(LineBuffer&, const UtcFields&, std::uint32_t code, std::string_view module,
                    std::string_view dump);

} // namespace fv::log::detail
