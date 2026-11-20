#include "net/detail/file_response.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <system_error>

#include <openssl/evp.h>

#include "net/detail/wire.h"
#include "pyport/number.h"

namespace fv::net::detail {

namespace {

// FileResponse.max_ranges: a header naming more is answered with the whole file.
constexpr std::size_t kMaxRanges = 100;
// Python's ints are unbounded; clamped here they still compare with any file size alike.
constexpr std::int64_t kHuge = std::int64_t{1} << 62;

constexpr std::string_view kTextPlain = "text/plain; charset=utf-8";

// str.strip() of a header decoded as latin-1: ASCII whitespace, the separators 0x1c-0x1f, NEL
// and NBSP.
bool is_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f) || c == 0x85 ||
           c == 0xa0;
}

std::string_view strip(std::string_view text) {
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// int(text) clamped to +-2**62; nullopt where Python raises ValueError.
std::optional<std::int64_t> python_int(std::string_view text) {
    const auto parsed = py::py_int_checked(text);
    if (parsed) {
        return std::clamp(*parsed, -kHuge, kHuge);
    }
    if (parsed.error() == py::IntError::Overflow) {
        return strip(text).starts_with('-') ? -kHuge : kHuge;
    }
    return std::nullopt;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto at = text.find(separator, start);
        parts.push_back(text.substr(start, at == std::string_view::npos ? at : at - start));
        if (at == std::string_view::npos) {
            return parts;
        }
        start = at + 1;
    }
}

// FileResponse._parse_ranges: [start, end) pairs as Python ints; parts that do not parse are
// skipped.
std::vector<std::pair<std::int64_t, std::int64_t>> parse_ranges(std::string_view text,
                                                                std::int64_t size) {
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    for (std::string_view part : split(text, ',')) {
        part = strip(part);
        if (part.empty() || part == "-") {
            continue;
        }
        const auto dash = part.find('-');
        if (dash == std::string_view::npos) {
            continue;
        }
        const std::string_view start_text = strip(part.substr(0, dash));
        const std::string_view end_text = strip(part.substr(dash + 1));
        std::int64_t start = 0;
        if (!start_text.empty()) {
            const auto value = python_int(start_text);
            if (!value) {
                continue;
            }
            start = *value;
        } else {
            const auto value = python_int(end_text);
            if (!value) {
                continue;
            }
            start = std::max<std::int64_t>(size - *value, 0);
        }
        std::int64_t end = size;
        if (!start_text.empty() && !end_text.empty()) {
            const auto value = python_int(end_text);
            if (!value) {
                continue;
            }
            if (*value < size) {
                end = *value + 1;
            }
        }
        ranges.emplace_back(start, end);
    }
    return ranges;
}

RangeRequest malformed(std::string message) {
    RangeRequest request;
    request.kind = RangeRequest::Kind::Malformed;
    request.message = std::move(message);
    return request;
}

// The checks _parse_range_header makes once the parts are parsed.
RangeRequest check_ranges(std::vector<std::pair<std::int64_t, std::int64_t>> ranges,
                          std::int64_t size) {
    if (ranges.empty()) {
        return malformed("Range header: range must be requested");
    }
    for (const auto& [start, end] : ranges) {
        if (!(0 <= start && start < size)) {
            RangeRequest request;
            request.kind = RangeRequest::Kind::Unsatisfiable;
            return request;
        }
    }
    for (const auto& [start, end] : ranges) {
        if (start >= end) {
            return malformed("Range header: start must be less than end");
        }
    }
    if (ranges.size() > 1) {
        std::ranges::sort(ranges);
        std::vector<std::pair<std::int64_t, std::int64_t>> merged{ranges.front()};
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            auto& last = merged.back();
            if (ranges[i].first <= last.second) {
                last.second = std::max(last.second, ranges[i].second);
            } else {
                merged.push_back(ranges[i]);
            }
        }
        ranges = std::move(merged);
    }
    RangeRequest request;
    request.kind = RangeRequest::Kind::Ranges;
    for (const auto& [start, end] : ranges) {
        request.ranges.emplace_back(static_cast<std::uint64_t>(start),
                                    static_cast<std::uint64_t>(end));
    }
    return request;
}

std::int64_t signed_size(std::uint64_t size) {
    return static_cast<std::int64_t>(std::min<std::uint64_t>(size, kHuge));
}

std::optional<std::string_view> find(const Headers& headers, std::string_view name) {
    return header(headers, name);
}

// MutableHeaders.setdefault on lower-case names.
void set_default(Headers& headers, std::string_view name, std::string value) {
    for (const auto& [key, existing] : headers) {
        if (key == name) {
            return;
        }
    }
    headers.emplace_back(std::string(name), std::move(value));
}

// MutableHeaders.__setitem__: the first of that name takes the value, the others go; else
// appended.
void set_header(Headers& headers, std::string_view name, std::string value) {
    bool placed = false;
    for (auto it = headers.begin(); it != headers.end();) {
        if (it->first == name) {
            if (!placed) {
                it->second = value;
                placed = true;
                ++it;
            } else {
                it = headers.erase(it);
            }
        } else {
            ++it;
        }
    }
    if (!placed) {
        headers.emplace_back(std::string(name), std::move(value));
    }
}

// PlainTextResponse(content, status_code=status, headers=extra).
FilePlan plain_text(int status, std::string body, Headers extra = {}) {
    FilePlan plan;
    plan.status = status;
    plan.headers = std::move(extra);
    plan.headers.emplace_back("content-length", std::to_string(body.size()));
    plan.headers.emplace_back("content-type", std::string(kTextPlain));
    plan.body = std::move(body);
    return plan;
}

// StaticFiles.is_not_modified
bool is_not_modified(const Headers& response, const Headers& request) {
    if (const auto if_none_match = find(request, "if-none-match");
        if_none_match && !if_none_match->empty()) {
        const std::string_view etag = find(response, "etag").value_or("");
        for (std::string_view tag : split(*if_none_match, ',')) {
            tag = strip(tag);
            if (tag.starts_with("W/")) {
                tag.remove_prefix(2);
            }
            if (tag == etag) {
                return true;
            }
        }
        return false;
    }
    const auto if_modified_since = find(request, "if-modified-since");
    const auto last_modified = find(response, "last-modified");
    if (!if_modified_since || !last_modified) {
        return false;
    }
    const auto since = parsedate(*if_modified_since);
    const auto modified = parsedate(*last_modified);
    return since && modified && *since >= *modified;
}

std::string digits(std::uint64_t value) {
    return std::to_string(value);
}

std::string md5_hex(std::string_view text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(text.data(), text.size(), digest, &length, EVP_md5(), nullptr) != 1) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0xf];
    }
    return out;
}

} // namespace

RangeRequest parse_range_header(std::string_view header, std::uint64_t size) {
    const auto equals = header.find('=');
    if (equals == std::string_view::npos) {
        return malformed("Malformed range header.");
    }
    if (ascii_lower(strip(header.substr(0, equals))) != "bytes") {
        return malformed("Only support bytes range");
    }
    const std::string_view text = header.substr(equals + 1);
    if (static_cast<std::size_t>(std::ranges::count(text, ',')) + 1 > kMaxRanges) {
        return {};
    }
    const std::int64_t length = signed_size(size);
    return check_ranges(parse_ranges(text, length), length);
}

RangeRequest check_byte_range(const ByteRange& range, std::uint64_t size) {
    if (!range.first && !range.last) {
        return {};
    }
    const std::int64_t length = signed_size(size);
    const auto clamp = [](std::uint64_t value) {
        return static_cast<std::int64_t>(std::min<std::uint64_t>(value, kHuge));
    };
    std::int64_t start = 0;
    std::int64_t end = length;
    if (range.first) {
        start = clamp(*range.first);
        if (range.last && clamp(*range.last) < length) {
            end = clamp(*range.last) + 1;
        }
    } else {
        start = std::max<std::int64_t>(length - clamp(*range.last), 0);
    }
    return check_ranges({{start, end}}, length);
}

std::optional<std::array<std::int64_t, 6>> parsedate(std::string_view text) {
    static constexpr std::array<std::string_view, 24> kMonths = {
        "jan", "feb",  "mar",  "apr",    "may",       "jun",      "jul",      "aug",
        "sep", "oct",  "nov",  "dec",    "january",   "february", "march",    "april",
        "may", "june", "july", "august", "september", "october",  "november", "december"};
    static constexpr std::array<std::string_view, 7> kDays = {"mon", "tue", "wed", "thu",
                                                              "fri", "sat", "sun"};
    // data.split(): runs of whitespace separate, none are empty.
    std::vector<std::string> data;
    for (std::size_t i = 0; i < text.size();) {
        while (i < text.size() && is_space(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        std::size_t j = i;
        while (j < text.size() && !is_space(static_cast<unsigned char>(text[j]))) {
            ++j;
        }
        if (j > i) {
            data.emplace_back(text.substr(i, j - i));
        }
        i = j;
    }
    if (data.empty()) {
        return std::nullopt;
    }
    const auto is_day = [&](const std::string& word) {
        return std::ranges::find(kDays, ascii_lower(word)) != kDays.end();
    };
    if (data[0].ends_with(',') || is_day(data[0])) {
        data.erase(data.begin());
    } else if (const auto comma = data[0].rfind(','); comma != std::string::npos) {
        data[0] = data[0].substr(comma + 1);
    }
    if (data.size() == 3) { // RFC 850 date
        const auto stuff = split(data[0], '-');
        if (stuff.size() == 3) {
            std::vector<std::string> joined(stuff.begin(), stuff.end());
            joined.insert(joined.end(), data.begin() + 1, data.end());
            data = std::move(joined);
        }
    }
    if (data.size() == 4) {
        const std::string s = data[3];
        auto at = s.find('+');
        if (at == std::string::npos) {
            at = s.find('-');
        }
        if (at != std::string::npos && at > 0) {
            data[3] = s.substr(0, at);
            data.push_back(s.substr(at));
        } else {
            data.emplace_back();
        }
    }
    if (data.size() < 5) {
        return std::nullopt;
    }
    std::string dd = data[0];
    std::string mm = data[1];
    std::string yy = data[2];
    std::string tm = data[3];
    std::string tz = data[4];
    if (dd.empty() || mm.empty() || yy.empty()) {
        return std::nullopt;
    }
    mm = ascii_lower(mm);
    auto month = std::ranges::find(kMonths, mm);
    if (month == kMonths.end()) {
        std::swap(dd, mm);
        mm = ascii_lower(mm);
        month = std::ranges::find(kMonths, mm);
        if (month == kMonths.end()) {
            return std::nullopt;
        }
    }
    std::int64_t month_number = (month - kMonths.begin()) + 1;
    if (month_number > 12) {
        month_number -= 12;
    }
    if (!dd.empty() && dd.back() == ',') {
        dd.pop_back();
    }
    if (const auto colon = yy.find(':'); colon != std::string::npos && colon > 0) {
        std::swap(yy, tm);
    }
    if (!yy.empty() && yy.back() == ',') {
        yy.pop_back();
        if (yy.empty()) {
            return std::nullopt;
        }
    }
    if (yy.empty() || !(yy[0] >= '0' && yy[0] <= '9')) {
        std::swap(yy, tz);
    }
    if (!tm.empty() && tm.back() == ',') {
        tm.pop_back();
    }
    auto fields = split(tm, ':');
    std::string_view hour;
    std::string_view minute;
    std::string_view second = "0";
    if (fields.size() == 2) {
        hour = fields[0];
        minute = fields[1];
    } else if (fields.size() == 3) {
        hour = fields[0];
        minute = fields[1];
        second = fields[2];
    } else if (fields.size() == 1 && fields[0].contains('.')) {
        fields = split(fields[0], '.');
        if (fields.size() == 2) {
            hour = fields[0];
            minute = fields[1];
        } else if (fields.size() == 3) {
            hour = fields[0];
            minute = fields[1];
            second = fields[2];
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    const auto year = py::py_int(yy);
    const auto day = py::py_int(dd);
    const auto hours = py::py_int(hour);
    const auto minutes = py::py_int(minute);
    const auto seconds = py::py_int(second);
    if (!year || !day || !hours || !minutes || !seconds) {
        return std::nullopt;
    }
    std::int64_t full_year = *year;
    if (full_year < 100) {
        full_year += full_year > 68 ? 1900 : 2000;
    }
    return std::array<std::int64_t, 6>{full_year, month_number, *day, *hours, *minutes, *seconds};
}

std::optional<FileFacts> stat_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    FileFacts facts;
    facts.size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
#ifdef _MSC_VER
    // MSVC's file_clock converts through utc_clock only.
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
    const auto system = std::chrono::file_clock::to_sys(written);
#endif
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch());
    const auto whole = std::chrono::floor<std::chrono::seconds>(since_epoch);
    facts.mtime_seconds = whole.count();
    facts.mtime_nanoseconds = (since_epoch - whole).count();
    return facts;
}

std::string file_etag(const FileFacts& facts) {
    // CPython's st_mtime: sec + nsec * 1e-9 in double arithmetic.
    const double mtime = static_cast<double>(facts.mtime_seconds) +
                         static_cast<double>(facts.mtime_nanoseconds) * 1e-9;
    return "\"" + md5_hex(py::float_repr(mtime) + "-" + digits(facts.size)) + "\"";
}

std::string file_last_modified(const FileFacts& facts) {
    return http_date(
        std::chrono::system_clock::time_point(std::chrono::seconds(facts.mtime_seconds)));
}

FilePlan plan_file_response(const FileFacts& facts, std::string_view content_type,
                            const std::optional<ByteRange>& range, const FileOptions& options,
                            const Headers& request_headers, std::string_view boundary) {
    // FileResponse.__init__: the caller's headers, content-type, accept-ranges, the
    // disposition, then the stat headers, each only where the caller gave none.
    Headers headers;
    for (const auto& [name, value] : options.headers) {
        headers.emplace_back(ascii_lower(name), value);
    }
    if (!find(headers, "content-type")) {
        std::string media(content_type);
        if (media.starts_with("text/") && !ascii_lower(media).contains("charset=")) {
            media += "; charset=utf-8";
        }
        headers.emplace_back("content-type", std::move(media));
    }
    set_default(headers, "accept-ranges", "bytes");
    if (options.filename) {
        const std::string quoted = quote(*options.filename, "/");
        set_default(headers, "content-disposition",
                    quoted != *options.filename
                        ? options.content_disposition_type + "; filename*=utf-8''" + quoted
                        : options.content_disposition_type + "; filename=\"" + *options.filename +
                              "\"");
    }
    set_default(headers, "content-length", digits(facts.size));
    set_default(headers, "last-modified", file_last_modified(facts));
    set_default(headers, "etag", file_etag(facts));

    if (options.not_modified_check && is_not_modified(headers, request_headers)) {
        // NotModifiedResponse keeps these of the file's headers and nothing else.
        static constexpr std::array<std::string_view, 6> kKept = {
            "cache-control", "content-location", "date", "etag", "expires", "vary"};
        FilePlan plan;
        plan.status = 304;
        for (const auto& [name, value] : headers) {
            if (std::ranges::find(kKept, name) != kKept.end()) {
                plan.headers.emplace_back(name, value);
            }
        }
        return plan;
    }

    RangeRequest ranges;
    if (range) {
        ranges = check_byte_range(*range, facts.size);
    } else if (const auto http_range = find(request_headers, "range")) {
        const auto if_range = find(request_headers, "if-range");
        const bool use_range = !if_range || *if_range == find(headers, "last-modified") ||
                               *if_range == find(headers, "etag");
        if (use_range) {
            ranges = parse_range_header(*http_range, facts.size);
        }
    }

    FilePlan plan;
    switch (ranges.kind) {
    case RangeRequest::Kind::Malformed:
        return plain_text(400, ranges.message);
    case RangeRequest::Kind::Unsatisfiable:
        return plain_text(416, "", {{"content-range", "bytes */" + digits(facts.size)}});
    case RangeRequest::Kind::Whole:
        plan.status = 200;
        plan.headers = std::move(headers);
        plan.content_length = facts.size;
        if (facts.size > 0) {
            plan.pieces.push_back({{}, 0, facts.size});
        }
        return plan;
    case RangeRequest::Kind::Ranges:
        break;
    }
    plan.status = 206;
    if (ranges.ranges.size() == 1) {
        const auto [start, end] = ranges.ranges.front();
        set_header(headers, "content-range",
                   "bytes " + digits(start) + "-" + digits(end - 1) + "/" + digits(facts.size));
        set_header(headers, "content-length", digits(end - start));
        plan.headers = std::move(headers);
        plan.content_length = end - start;
        plan.pieces.push_back({{}, start, end - start});
        return plan;
    }
    const std::string part_type(find(headers, "content-type").value_or(""));
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < ranges.ranges.size(); ++i) {
        const auto [start, end] = ranges.ranges[i];
        std::string literal = i == 0 ? std::string() : std::string("\r\n");
        literal += "--" + std::string(boundary) + "\r\nContent-Type: " + part_type +
                   "\r\nContent-Range: bytes " + digits(start) + "-" + digits(end - 1) + "/" +
                   digits(facts.size) + "\r\n\r\n";
        length += literal.size() + (end - start);
        plan.pieces.push_back({std::move(literal), start, end - start});
    }
    std::string closing = "\r\n--" + std::string(boundary) + "--";
    length += closing.size();
    plan.pieces.push_back({std::move(closing), 0, 0});
    set_header(headers, "content-type", "multipart/byteranges; boundary=" + std::string(boundary));
    set_header(headers, "content-length", digits(length));
    plan.headers = std::move(headers);
    plan.content_length = length;
    return plan;
}

FilePlan not_found_plan() {
    FilePlan plan;
    plan.status = 404;
    plan.body = R"({"detail":"Not Found"})";
    plan.headers = {{"content-length", digits(plan.body.size())},
                    {"content-type", "application/json"}};
    return plan;
}

std::string random_boundary() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device device;
    std::string out;
    for (int i = 0; i < 13; ++i) {
        const auto byte = static_cast<unsigned>(device()) & 0xffu;
        out += kHex[byte >> 4];
        out += kHex[byte & 0xf];
    }
    return out;
}

std::optional<std::string> FileBody::next(std::size_t chunk) {
    while (index_ < pieces_.size()) {
        const BodyPiece& piece = pieces_[index_];
        if (!literal_sent_) {
            literal_sent_ = true;
            if (!piece.literal.empty()) {
                return piece.literal;
            }
        }
        if (sent_ < piece.length) {
            if (sent_ == 0) {
                file_.seekg(static_cast<std::streamoff>(piece.offset));
            }
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(chunk, piece.length - sent_));
            std::string data(count, '\0');
            file_.read(data.data(), static_cast<std::streamsize>(count));
            if (!file_ || static_cast<std::size_t>(file_.gcount()) != count) {
                return std::nullopt;
            }
            sent_ += count;
            return data;
        }
        ++index_;
        literal_sent_ = false;
        sent_ = 0;
    }
    return std::string();
}

} // namespace fv::net::detail
