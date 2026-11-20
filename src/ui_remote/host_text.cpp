#include "ui_remote/host_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <string>

#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "ui_remote/detail/py_value.h"

namespace fv::host_text {

namespace {

using ui_remote::detail::get;
using ui_remote::detail::py_str;
using ui_remote::detail::truthy;

constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// float(value or 0): a number, or text float() reads; anything else is 0.
double number(const Json& value) {
    if (!truthy(value)) {
        return 0.0;
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    return py::py_float(value).value_or(0.0);
}

std::chrono::zoned_time<std::chrono::milliseconds> zoned(double at,
                                                         const std::chrono::time_zone& zone) {
    const auto moment = std::chrono::sys_time<std::chrono::milliseconds>(
        std::chrono::milliseconds(static_cast<long long>(std::floor(at * 1000.0))));
    return {&zone, moment};
}

// datetime.fromisoformat(iso).timestamp(): a naive time is local to `zone`.
std::optional<double> timestamp(std::string_view iso, const std::chrono::time_zone& zone) {
    py::DateTime parsed;
    try {
        parsed = py::fromisoformat(iso);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const auto day = year_month_day(year(parsed.year), month(static_cast<unsigned>(parsed.month)),
                                    std::chrono::day(static_cast<unsigned>(parsed.day)));
    if (!day.ok()) {
        return std::nullopt;
    }
    const auto clock = hours(parsed.hour) + minutes(parsed.minute) + seconds(parsed.second) +
                       microseconds(parsed.microsecond);
    sys_time<microseconds> moment;
    if (parsed.utc_offset_us) {
        moment = sys_days(day) + clock - microseconds(*parsed.utc_offset_us);
    } else {
        moment = zone.to_sys(local_days(day) + clock, choose::earliest);
    }
    return duration<double>(moment.time_since_epoch()).count();
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

std::string count(long long n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

std::string span(double seconds) {
    seconds = std::max(0.0, seconds);
    if (seconds < 60) {
        return seconds >= 10 ? std::format("{:.0f} s", seconds) : std::format("{:.1f} s", seconds);
    }
    const auto minutes = static_cast<long long>(seconds / 60);
    if (minutes < 60) {
        return std::format("{} min", minutes);
    }
    const auto hours = minutes / 60;
    const auto rest = minutes % 60;
    return rest != 0 ? std::format("{} h {} min", hours, rest) : std::format("{} h", hours);
}

std::string quick(double seconds) {
    seconds = std::max(0.0, seconds);
    return seconds < 1 ? std::format("{:.0f} ms", seconds * 1000) : span(seconds);
}

std::string ago(double at, double now, const std::chrono::time_zone& zone) {
    if (at == 0.0) {
        return "";
    }
    const double delta = std::max(0.0, now - at);
    if (delta < 45) {
        return "just now";
    }
    if (delta < 3600) {
        return std::format("{} min ago",
                           std::max(1LL, static_cast<long long>(py::round_half_even(delta / 60))));
    }
    if (delta < 86400) {
        return std::format("{} h ago", static_cast<long long>(py::round_half_even(delta / 3600)));
    }
    // strftime("%b %d").replace(" 0", " ")
    const std::chrono::year_month_day date(
        std::chrono::floor<std::chrono::days>(zoned(at, zone).get_local_time()));
    return std::format("{} {}", kMonths.at(static_cast<unsigned>(date.month()) - 1),
                       static_cast<unsigned>(date.day()));
}

std::string clock(double at, const std::chrono::time_zone& zone) {
    const auto local = zoned(at, zone).get_local_time();
    const std::chrono::hh_mm_ss time(local - std::chrono::floor<std::chrono::days>(local));
    auto hour = time.hours().count() % 12;
    if (hour == 0) {
        hour = 12;
    }
    // strftime("%I:%M %p").lstrip("0")
    return std::format("{}:{:02} {}", hour, time.minutes().count(),
                       time.hours().count() < 12 ? "AM" : "PM");
}

std::string iso_ago(std::string_view iso, double now, const std::chrono::time_zone& zone) {
    const auto at = timestamp(iso, zone);
    return at ? ago(*at, now, zone) : std::string();
}

std::string device_label(std::string_view device) {
    const auto folded = lower(device);
    if (folded == "cuda" || folded == "cpu") {
        std::string upper(device);
        std::ranges::transform(upper, upper.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return upper;
    }
    return std::string(device);
}

std::string speed(double audio_s, double host_s) {
    if (host_s <= 0 || audio_s <= 0) {
        return "—";
    }
    const double ratio = audio_s / host_s;
    return ratio >= 10 ? std::format("{:.0f}×", ratio) : std::format("{:.1f}×", ratio);
}

std::string event_text(const Json& event) {
    const Json& named = get(event, "name");
    const std::string name = truthy(named) ? py_str(named) : "A paired computer";
    const Json& kind = get(event, "kind");
    const std::string what = kind.is_string() ? kind.get<std::string>() : std::string();
    if (what == "transcribed") {
        return std::format("{} · {} of audio, back in {}", name,
                           span(number(get(event, "audio_s"))),
                           quick(number(get(event, "host_s"))));
    }
    if (what == "failed") {
        const Json& reason = get(event, "reason");
        return std::format("{} · couldn't transcribe: {}", name,
                           truthy(reason) ? py_str(reason) : "unknown error");
    }
    if (what == "connected") {
        return name + " connected";
    }
    if (what == "disconnected") {
        return name + " disconnected";
    }
    if (what == "paired") {
        const Json& via = get(event, "via");
        return "Paired " + name +
               (via.is_string() && via.get<std::string>() == "tailscale" ? " over Tailscale" : "");
    }
    if (what == "switched") {
        const Json& label = get(event, "label");
        return std::format("{} switched the engine to {}", name,
                           truthy(label) ? py_str(label) : "another model");
    }
    return name;
}

std::string event_tone(std::string_view kind) {
    static const std::map<std::string, std::string, std::less<>> tones = {
        {"transcribed", "success"},     {"failed", "danger"}, {"connected", "accent"},
        {"disconnected", "text-muted"}, {"paired", "purple"}, {"switched", "warning"},
    };
    const auto found = tones.find(kind);
    return found != tones.end() ? found->second : "text-muted";
}

std::vector<GroupedClient> group_clients(const std::vector<remote::ConnectedClient>& clients) {
    std::vector<GroupedClient> grouped;
    for (const auto& client : clients) {
        const std::string key = !client.device_id.empty() ? client.device_id : client.name;
        const auto entry = std::ranges::find(grouped, key, &GroupedClient::device_id);
        if (entry == grouped.end()) {
            grouped.push_back({key, client.name.empty() ? "A paired computer" : client.name,
                               client.address, client.busy, client.since, 1});
            continue;
        }
        entry->busy = entry->busy || client.busy;
        if (client.since != 0.0 && (entry->since == 0.0 || client.since < entry->since)) {
            entry->since = client.since;
        }
        entry->connections += 1;
    }
    // sorted(key=name.lower()): stable, so equal names keep their first-seen order.
    std::ranges::stable_sort(grouped, {},
                             [](const GroupedClient& entry) { return lower(entry.name); });
    return grouped;
}

} // namespace fv::host_text
