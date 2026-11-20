#include "ui_remote/remote_text.h"

#include <array>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>

#include "base/identity.h"
#include "history/calendar.h"
#include "history/format_utils.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "remote/protocol.h"
#include "ui_remote/detail/py_value.h"

namespace fv::remote_text {

namespace {

using ui_remote::detail::get;
using ui_remote::detail::py_str;
using ui_remote::detail::py_str_or_empty;
using ui_remote::detail::truthy;

constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void replace_all(std::string& text, std::string_view from, std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

// int(value or 0)
std::int64_t count_of(const Json& value) {
    if (!truthy(value)) {
        return 0;
    }
    const auto number = py::py_int_checked(value);
    if (!number) {
        throw py::ValueError(std::format("invalid literal for int(): {}", py_str(value)));
    }
    return *number;
}

struct Kind {
    const char* key;
    const char* one;
    const char* many;
};
constexpr std::array<Kind, 2> kKinds = {Kind{"dictation", "dictation", "dictations"},
                                        Kind{"meeting", "meeting", "meetings"}};

std::string phrase(const std::vector<std::string>& parts, std::int64_t total) {
    if (parts.empty()) {
        return "";
    }
    std::string joined = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) {
        joined += " and " + parts[i];
    }
    if (total == 0) {
        return joined;
    }
    return std::format("{} ({})", joined, format_file_size(static_cast<double>(total)));
}

} // namespace

std::string paired_on(std::string_view iso, const std::chrono::time_zone& zone) {
    std::optional<py::DateTime> local;
    try {
        local = history::local_wall_time(py::fromisoformat(iso), zone);
    } catch (const py::ValueError&) {
        return "";
    }
    if (!local) {
        return "";
    }
    // strftime("%b %d, %Y").replace(" 0", " ")
    std::string text =
        std::format("{} {:02}, {}", kMonths.at(static_cast<std::size_t>(local->month - 1)),
                    local->day, local->year);
    replace_all(text, " 0", " ");
    return text;
}

std::string paired_on(const Json& iso, const std::chrono::time_zone& zone) {
    if (!iso.is_string()) {
        return "";
    }
    return paired_on(std::string_view(iso.get_ref<const std::string&>()), zone);
}

std::string stored_phrase(const Json& stored) {
    std::vector<std::string> parts;
    std::int64_t total = 0;
    for (const Kind& kind : kKinds) {
        const Json& summary = get(stored, kind.key);
        const std::int64_t count = count_of(get(summary, "count"));
        total += count_of(get(summary, "bytes"));
        if (count != 0) {
            parts.push_back(std::format("{} {}", count, count == 1 ? kind.one : kind.many));
        }
    }
    return phrase(parts, total);
}

std::string stored_phrase(const std::map<std::string, remote::StoredCount>& stored) {
    std::vector<std::string> parts;
    std::int64_t total = 0;
    for (const Kind& kind : kKinds) {
        const auto found = stored.find(kind.key);
        if (found == stored.end()) {
            continue;
        }
        const std::int64_t count = found->second.count;
        total += found->second.bytes;
        if (count != 0) {
            parts.push_back(std::format("{} {}", count, count == 1 ? kind.one : kind.many));
        }
    }
    return phrase(parts, total);
}

std::string records_location_description(std::string_view location, std::string_view host) {
    if (location == "local") {
        return "History, recordings and meetings stay on this computer.";
    }
    if (location == "host") {
        return std::format("New dictations and meetings move to {} once it has checked them. "
                           "History and Past Meetings list them from there.",
                           host);
    }
    if (location == "both") {
        return std::format("New dictations and meetings stay here and are copied to {}. Deleting "
                           "one here deletes both copies.",
                           host);
    }
    throw std::invalid_argument(std::format("Unknown storage location: '{}'", location));
}

std::string records_host_problem(const remote::SyncStatus& status, std::string_view host) {
    if (status.location == "local") {
        return "";
    }
    if (status.host_supports == false) {
        return std::format("Update {} on {} to keep records there.", identity().product, host);
    }
    if (status.host_keeps == false) {
        return std::format("{0} isn't keeping records for other computers yet. On {0}, turn on "
                           "\"Keep records for paired computers\" under Settings → Remote engine.",
                           host);
    }
    return "";
}

std::string sharing_address(const remote::HostState& state,
                            const std::optional<net::TailscaleStatus>& tailnet, bool on_tailnet) {
    const std::string& host =
        state.address && !state.address->empty() ? *state.address : state.host_name;
    std::string where = remote::format_address(host, state.port);
    if (state.address_kind == "vpn") {
        where += " (a VPN address; computers on your network may not reach it)";
    }
    if (on_tailnet && tailnet) {
        where +=
            std::format(", and on Tailscale as {} ({})",
                        tailnet->name.empty() ? tailnet->address : tailnet->name, tailnet->address);
    }
    return where;
}

std::string sharing_status(const remote::HostState& state, std::string_view where) {
    if (!state.error.empty()) {
        return state.error;
    }
    if (state.running) {
        const Json engine = truthy(state.engine) ? state.engine : Json::object();
        if (truthy(get(engine, "available"))) {
            return std::format("Sharing {} at {}.", engine_phrase(engine), where);
        }
        const Json& status = get(engine, "status");
        return std::format("Sharing is on at {}, but no engine is ready to serve: {}.", where,
                           truthy(status) ? py_str(status) : "select a local engine");
    }
    if (state.enabled) {
        return "Starting...";
    }
    return "";
}

std::string engine_phrase(const Json& engine) {
    const std::string label = py_str_or_empty(get(engine, "label"));
    if (label.empty()) {
        return "";
    }
    const std::string device = py_str_or_empty(get(engine, "device"));
    const std::string compute = py_str_or_empty(get(engine, "compute_type"));
    if (device.empty()) {
        return label;
    }
    return compute.empty() ? std::format("{} on {}", label, device)
                           : std::format("{} on {} ({})", label, device, compute);
}

std::string find_row_detail(const remote::NearbyHost& host) {
    std::vector<std::string> details = {host.name()};
    const std::string engine = engine_phrase(host.engine);
    if (!host.compatible) {
        details.push_back(std::format("needs an {} update to pair", identity().product));
    } else if (!engine.empty() && truthy(get(host.engine, "available"))) {
        details.push_back(engine);
    } else {
        details.emplace_back("sharing, but no engine is ready");
    }
    const net::TailscalePeer* peer = host.tailnet ? &host.tailnet->peer : nullptr;
    if (peer != nullptr && peer->mine) {
        details.emplace_back("your computer");
    } else if (peer != nullptr && !peer->owner.empty()) {
        details.push_back(std::format("shared by {}", peer->owner));
    } else if (host.lan) {
        details.emplace_back("on this network");
    }
    std::string text = details.front();
    for (std::size_t i = 1; i < details.size(); ++i) {
        text += " · " + details[i];
    }
    return text;
}

bool find_row_connects(const remote::NearbyHost& host) {
    return host.compatible && (host.can_pair_without_code() || host.approval);
}

std::string pairing_grants_phrase(const std::vector<std::string>& grants) {
    std::string also;
    if (!grants.empty()) {
        std::string listed;
        if (grants.size() == 1) {
            listed = grants.front();
        } else {
            for (std::size_t i = 0; i + 1 < grants.size(); ++i) {
                listed += i == 0 ? grants[i] : ", " + grants[i];
            }
            listed += (grants.size() > 2 ? ", and " : " and ") + grants.back();
        }
        also = ". It can also " + listed;
    }
    return std::format("Once allowed, it can dictate and transcribe with the engine selected "
                       "here{}. You can remove it any time under Settings → Remote engine.",
                       also);
}

} // namespace fv::remote_text
