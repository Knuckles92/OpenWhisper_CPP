#include "remote_host/reachability.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "base/json.h"
#include "base/log.h"
#include "process/run_once.h"

namespace fv::remote_host {

namespace {

constexpr log::Logger kLog{"services.remote_asr.reachability"};

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// _CATEGORIES: NetworkCategory as a number or a name.
std::optional<std::string> category_of(const Json& raw) {
    if (raw.is_string()) {
        const auto name = lower(raw.get<std::string>());
        if (name == "public") {
            return std::string(CATEGORY_PUBLIC);
        }
        if (name == "private") {
            return std::string(CATEGORY_PRIVATE);
        }
        if (name == "domainauthenticated") {
            return std::string(CATEGORY_DOMAIN);
        }
        return std::nullopt;
    }
    if (raw.is_number_integer() || raw.is_boolean()) {
        const auto value = raw.is_boolean() ? (raw.get<bool>() ? 1 : 0) : raw.get<long long>();
        switch (value) {
        case 0:
            return std::string(CATEGORY_PUBLIC);
        case 1:
            return std::string(CATEGORY_PRIVATE);
        case 2:
            return std::string(CATEGORY_DOMAIN);
        default:
            return std::nullopt;
        }
    }
    if (raw.is_number_float()) {
        // A float equal to 0, 1 or 2 is that key in a Python dict.
        const double value = raw.get<double>();
        if (value == 0.0 || value == 1.0 || value == 2.0) {
            return category_of(Json(static_cast<long long>(value)));
        }
    }
    return std::nullopt;
}

// str(entry.get(key) or "")
std::string text_of(const Json& entry, const char* key) {
    const auto found = entry.find(key);
    if (found == entry.end() || found->is_null()) {
        return "";
    }
    if (found->is_string()) {
        return found->get<std::string>();
    }
    if ((found->is_boolean() && !found->get<bool>()) ||
        (found->is_number() && found->get<double>() == 0.0) ||
        ((found->is_array() || found->is_object()) && found->empty())) {
        return "";
    }
    return found->dump();
}

} // namespace

std::vector<NetworkProfile> parse_profiles(std::string_view text) {
    if (std::ranges::all_of(text, [](unsigned char c) { return std::isspace(c) != 0; })) {
        return {};
    }
    Json data;
    try {
        data = Json::parse(text);
    } catch (const Json::exception&) {
        return {};
    }
    if (data.is_object()) {
        data = Json::array({data});
    }
    std::vector<NetworkProfile> profiles;
    if (!data.is_array()) {
        return profiles;
    }
    for (const auto& entry : data) {
        if (!entry.is_object()) {
            continue;
        }
        const auto raw = entry.find("NetworkCategory");
        const auto category = raw != entry.end() ? category_of(*raw) : std::nullopt;
        if (!category) {
            continue;
        }
        profiles.push_back({text_of(entry, "Name"), text_of(entry, "InterfaceAlias"), *category});
    }
    return profiles;
}

std::vector<NetworkProfile> windows_network_profiles(double timeout_s) {
#ifdef _WIN32
    const std::string command = "Get-NetConnectionProfile | Select-Object "
                                "Name,InterfaceAlias,NetworkCategory | ConvertTo-Json -Compress";
    const auto result = run_once(
        {"powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command}, "", timeout_s);
    if (!result.exit_code || *result.exit_code != 0) {
        kLog.debug("Could not read the Windows network profiles: {}", result.error);
        return {};
    }
    return parse_profiles(result.output);
#else
    (void)timeout_s;
    return {};
#endif
}

std::string settings_page(std::string_view interface_name) {
    const auto name = lower(interface_name);
    if (name.contains("wi-fi") || name.contains("wifi") || name.contains("wireless") ||
        name.contains("wlan")) {
        return "ms-settings:network-wifi";
    }
    if (name.contains("ethernet")) {
        return "ms-settings:network-ethernet";
    }
    return "ms-settings:network-status";
}

std::vector<remote::Note> host_notes(const std::optional<net::LocalAddress>& lan,
                                     const std::vector<net::LocalAddress>& addresses,
                                     const std::optional<std::vector<NetworkProfile>>& profiles,
                                     std::string_view discovery_error) {
    std::vector<remote::Note> notes;
    const bool on_lan = lan && lan->kind == net::kLan;
    if (on_lan && profiles && !profiles->empty()) {
        const auto profile =
            std::ranges::find(*profiles, lan->interface_name, &NetworkProfile::interface_name);
        if (profile != profiles->end() && profile->category == CATEGORY_PUBLIC) {
            const auto network =
                profile->name.empty() ? std::string("this network") : "\"" + profile->name + "\"";
            notes.push_back({"public_network",
                             std::format("Windows treats {} as a public network, which usually "
                                         "keeps other computers from connecting here. If they "
                                         "can't, make it a private network.",
                                         network),
                             "Open network settings", settings_page(profile->interface_name)});
        }
    }
    if (on_lan && std::ranges::any_of(addresses, [](const net::LocalAddress& address) {
            return address.kind == net::kVpn;
        })) {
        notes.push_back({"vpn",
                         "A VPN is on. If other computers can't connect, allow local network "
                         "(LAN) access in its settings.",
                         "", ""});
    }
    if (!discovery_error.empty()) {
        notes.push_back({"discovery",
                         std::format("Other computers can't find this one by searching ({}). They "
                                     "can still enter its address and pairing code.",
                                     discovery_error),
                         "", ""});
    }
    return notes;
}

} // namespace fv::remote_host
