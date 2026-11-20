#include "ui_automation/hooks_format.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>

#include "automation/catalog.h"
#include "automation/hooks_model.h"
#include "net/address.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::hooks_format {

namespace {

namespace catalog = automation::catalog;

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_scheme_char(char c) {
    return is_ascii_alpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

std::string_view after_at(std::string_view netloc) {
    const std::size_t at = netloc.rfind('@');
    return at == std::string_view::npos ? netloc : netloc.substr(at + 1);
}

// _check_bracketed_host of Python 3.12.10: an IPvFuture literal, or an IPv6 address (with an
// optional "%zone"); anything else raises.
void check_bracketed_host(std::string_view host) {
    if (host.starts_with('v')) {
        // \Av[a-fA-F0-9]+\..+\Z
        std::size_t i = 1;
        while (i < host.size() && is_hex(host[i])) {
            ++i;
        }
        const bool valid = i > 1 && i < host.size() && host[i] == '.' && i + 1 < host.size() &&
                           host.find('\n', i + 1) == std::string_view::npos;
        if (!valid) {
            throw py::ValueError("IPvFuture address is invalid");
        }
        return;
    }
    const auto address = net::parse_address(host);
    if (!address) {
        throw py::ValueError("does not appear to be an IPv4 or IPv6 address");
    }
    if (address->version != 6) {
        throw py::ValueError("An IPv4 address cannot be in brackets");
    }
}

// _check_bracketed_netloc: nothing before the bracket, only ":port" after it.
void check_bracketed_netloc(std::string_view netloc) {
    const std::string_view host_and_port = after_at(netloc);
    const std::size_t open = host_and_port.find('[');
    std::string_view host;
    if (open != std::string_view::npos) {
        if (open > 0) {
            throw py::ValueError("Invalid IPv6 URL");
        }
        const std::string_view bracketed = host_and_port.substr(open + 1);
        const std::size_t close = bracketed.find(']');
        host = bracketed.substr(0, close);
        const std::string_view port =
            close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
        if (!port.empty() && !port.starts_with(':')) {
            throw py::ValueError("Invalid IPv6 URL");
        }
    } else {
        host = host_and_port.substr(0, host_and_port.find(':'));
    }
    check_bracketed_host(host);
}

// Code points whose NFKC form holds "/", "?", "#", "@" or ":" (Unicode 15.0, as Python 3.12.10's
// unicodedata gives them; the list automation's own urlsplit uses). Composition never yields
// ASCII punctuation, so a netloc gains a separator under NFKC exactly when it holds one of these.
constexpr std::array<char32_t, 19> kNfkcSeparators = {
    0x2047, 0x2048, 0x2049, 0x2100, 0x2101, 0x2105, 0x2106, 0x2A74, 0xFE13, 0xFE16,
    0xFE55, 0xFE56, 0xFE5F, 0xFE6B, 0xFF03, 0xFF0F, 0xFF1A, 0xFF1F, 0xFF20,
};

// _checknetloc: a non-ASCII netloc whose NFKC form gains a separator is refused.
void check_netloc(std::string_view netloc) {
    if (std::all_of(netloc.begin(), netloc.end(),
                    [](char c) { return (static_cast<unsigned char>(c) & 0x80) == 0; })) {
        return;
    }
    std::size_t at = 0;
    while (at < netloc.size()) {
        const char32_t point = py::next_code_point(netloc, at);
        if (std::find(kNfkcSeparators.begin(), kNfkcSeparators.end(), point) !=
            kNfkcSeparators.end()) {
            throw py::ValueError("netloc '" + std::string(netloc) +
                                 "' contains invalid characters under NFKC normalization");
        }
    }
}

// What urlsplit gives for the address, as far as the Hooks page reads it.
struct Split {
    std::string scheme;
    std::string netloc;
};

// urllib.parse.urlsplit of Python 3.12.10. Throws py::ValueError where it raises: an unmatched
// bracket, text before or after a bracketed host, a bracketed host that is no IPv6 address or
// IPvFuture literal, or a non-ASCII netloc that NFKC gives a separator.
Split split(std::string_view url) {
    // Leading control characters and spaces go, and tabs and line breaks anywhere.
    std::size_t start = 0;
    while (start < url.size() && static_cast<unsigned char>(url[start]) <= 0x20) {
        ++start;
    }
    std::string text;
    for (const char c : url.substr(start)) {
        if (c != '\t' && c != '\r' && c != '\n') {
            text += c;
        }
    }
    Split parts;
    const std::size_t colon = text.find(':');
    if (colon != std::string::npos && colon > 0 && is_ascii_alpha(text[0]) &&
        std::all_of(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(colon),
                    is_scheme_char)) {
        parts.scheme = py::lower(text.substr(0, colon));
        text.erase(0, colon + 1);
    }
    if (text.starts_with("//")) {
        const std::size_t end = text.find_first_of("/?#", 2);
        parts.netloc = text.substr(2, end == std::string::npos ? std::string::npos : end - 2);
        const bool open = parts.netloc.find('[') != std::string::npos;
        const bool close = parts.netloc.find(']') != std::string::npos;
        if (open != close) {
            throw py::ValueError("Invalid IPv6 URL");
        }
        if (open) {
            check_bracketed_netloc(parts.netloc);
        }
    }
    check_netloc(parts.netloc);
    return parts;
}

// Whether urlsplit(...).hostname is not None (_hostinfo's hostname is not empty).
bool has_hostname(std::string_view netloc) {
    const std::string_view info = after_at(netloc);
    const std::size_t open = info.find('[');
    if (open != std::string_view::npos) {
        const std::string_view bracketed = info.substr(open + 1);
        return !bracketed.substr(0, bracketed.find(']')).empty();
    }
    return !info.substr(0, info.find(':')).empty();
}

// The chooser's order, as the friendly names were written.
const std::vector<std::pair<std::string_view, std::string_view>> kEventLabels = {
    {catalog::TRANSCRIPTION_COMPLETED, "Finished dictations"},
    {catalog::HISTORY_SAVED, "Saved to history"},
    {catalog::ACTION_RAN, "Actions that ran"},
    {catalog::RECORDING_STARTED, "Recording started"},
    {catalog::RECORDING_STOPPED, "Recording stopped"},
    {catalog::RECORDING_CANCELED, "Recording canceled"},
    {catalog::TRANSCRIPTION_FAILED, "Transcription failed"},
    {catalog::TRANSCRIPTION_PREVIEW, "Live preview text"},
    {catalog::ROUTE_DECIDED, "Spoken routing"},
    {catalog::FILTER_FAILED, "Text filter problems"},
    {catalog::CHANNEL_MESSAGE, "Claude Code channel"},
    {catalog::MEETING_STARTED, "Meeting started"},
    {catalog::MEETING_ENDED, "Meeting ended"},
    {catalog::MEETING_SEGMENTS, "Meeting transcript lines"},
    {catalog::MEETING_VOICE_COMMAND, "Meeting voice commands"},
};

std::string two_digits(long long value) {
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "%02lld", value);
    return buffer;
}

} // namespace

const std::vector<std::pair<std::string_view, std::string_view>>& event_labels() {
    return kEventLabels;
}

const std::vector<std::string>& default_events() {
    static const std::vector<std::string> events{std::string(catalog::TRANSCRIPTION_COMPLETED)};
    return events;
}

std::string stage_label(std::string_view stage) {
    if (stage == automation::hooks::BEFORE_CLEANUP) {
        return "Before AI cleanup";
    }
    if (stage == automation::hooks::AFTER_CLEANUP) {
        return "After AI cleanup";
    }
    return std::string(stage);
}

std::string event_label(std::string_view event_type) {
    if (event_type.ends_with(".test")) {
        return "Test";
    }
    for (const auto& [type, label] : kEventLabels) {
        if (type == event_type) {
            return std::string(label);
        }
    }
    return std::string(event_type);
}

std::vector<std::string> event_types() {
    const std::vector<std::string_view>& pub = catalog::PUBLIC_TYPES();
    std::vector<std::string> ordered;
    for (const auto& [type, label] : kEventLabels) {
        if (std::find(pub.begin(), pub.end(), type) != pub.end()) {
            ordered.emplace_back(type);
        }
    }
    for (const std::string_view type : pub) {
        if (std::find(ordered.begin(), ordered.end(), type) == ordered.end()) {
            ordered.emplace_back(type);
        }
    }
    return ordered;
}

std::string events_summary(const std::vector<std::string>& events) {
    if (events.empty()) {
        return "No events";
    }
    const std::string first = event_label(events.front());
    return events.size() == 1 ? first : first + " +" + std::to_string(events.size() - 1);
}

std::string clock(std::string_view at, const Clock& now) {
    std::string text(at);
    for (std::size_t pos = text.find('Z'); pos != std::string::npos; pos = text.find('Z', pos)) {
        text.replace(pos, 1, "+00:00");
        pos += 6;
    }
    py::DateTime moment;
    try {
        moment = py::fromisoformat(text);
    } catch (const py::PyError&) {
        return std::string(at);
    }
    namespace chrono = std::chrono;
    long long seconds_of_day =
        static_cast<long long>(moment.hour) * 3600 + moment.minute * 60 + moment.second;
    if (moment.utc_offset_us) {
        try {
            const chrono::sys_days day{chrono::year{moment.year} / chrono::month(moment.month) /
                                       chrono::day(moment.day)};
            // The offset's microseconds are below what the clock shows.
            const auto instant = chrono::sys_seconds{day} + chrono::seconds{seconds_of_day} -
                                 chrono::seconds{*moment.utc_offset_us / 1'000'000};
            const auto local = now.zone()->to_local(instant);
            const auto since_midnight = local - chrono::floor<chrono::days>(local);
            seconds_of_day = chrono::duration_cast<chrono::seconds>(since_midnight).count();
        } catch (const std::exception&) {
            // A moment the zone database cannot place is shown as it was written.
            return std::string(at);
        }
    }
    return two_digits(seconds_of_day / 3600) + ":" + two_digits(seconds_of_day / 60 % 60) + ":" +
           two_digits(seconds_of_day % 60);
}

Outcome delivery_result(const automation::DeliveryRecord& record) {
    std::string text;
    if (record.status) {
        text = std::to_string(*record.status) + " · " + std::to_string(record.elapsed_ms) + " ms";
    } else {
        text = record.error.empty() ? std::string("No answer") : record.error;
    }
    if (record.attempt > 1) {
        text += " · try " + std::to_string(record.attempt);
    }
    return {std::move(text), record.ok};
}

Outcome run_result(const automation::RunRecord& record) {
    if (record.exit_code) {
        return {"Exit " + std::to_string(*record.exit_code) + " · " +
                    std::to_string(record.elapsed_ms) + " ms",
                record.ok};
    }
    return {record.error.empty() ? std::string("Didn't run") : record.error, record.ok};
}

std::string unencrypted_note(std::string_view url) {
    const std::string stripped = py::strip(url);
    Split parts;
    try {
        parts = split(stripped);
    } catch (const py::ValueError&) {
        return {};
    }
    if (parts.scheme != "http" || !has_hostname(parts.netloc) ||
        automation::hooks::url_is_private(stripped)) {
        return {};
    }
    return "Sent unencrypted. That's fine on your own network; use https:// for anything "
           "farther away.";
}

std::string host_of(std::string_view url) {
    try {
        const Split parts = split(url);
        return parts.netloc.empty() ? std::string(url) : parts.netloc;
    } catch (const py::ValueError&) {
        return std::string(url);
    }
}

} // namespace fv::hooks_format
