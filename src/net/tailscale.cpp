#include "net/tailscale.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::net {

namespace {

const IpAddress kTailscaleV4 = *parse_address("100.64.0.0");
const IpAddress kTailscaleV6 = *parse_address("fd7a:115c:a1e0::");

bool in_prefix(const IpAddress& address, const IpAddress& network, int prefix) {
    if (address.version != network.version) {
        return false;
    }
    for (std::size_t i = 0; prefix > 0; ++i, prefix -= 8) {
        const auto mask = static_cast<std::uint8_t>(prefix >= 8 ? 0xff : (0xff << (8 - prefix)));
        if ((address.bytes[i] & mask) != (network.bytes[i] & mask)) {
            return false;
        }
    }
    return true;
}

// Python's truthiness of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    return !value.empty();
}

std::string py_repr(const Json& value);

// str(value) of what json.loads gives.
std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return py_repr(value);
}

std::string py_repr(const Json& value) {
    if (value.is_string()) {
        return py::repr(value.get_ref<const std::string&>());
    }
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    std::string text;
    if (value.is_array()) {
        text = "[";
        for (std::size_t i = 0; i < value.size(); ++i) {
            text += (i ? ", " : "") + py_repr(value[i]);
        }
        return text + "]";
    }
    text = "{";
    bool first = true;
    for (const auto& [key, item] : value.items()) {
        text += (first ? "" : ", ") + py::repr(key) + ": " + py_repr(item);
        first = false;
    }
    return text + "}";
}

const Json& get(const Json& object, std::string_view key) {
    static const Json none = nullptr;
    const auto found = object.find(key);
    return found == object.end() ? none : *found;
}

// data.get(key) if isinstance(data.get(key), dict) else {}
const Json& object_or_empty(const Json& object, std::string_view key) {
    static const Json empty = Json::object();
    const Json& value = get(object, key);
    return value.is_object() ? value : empty;
}

// str(value or "")
std::string str_or_empty(const Json& value) {
    return truthy(value) ? py_str(value) : std::string();
}

std::string rstrip_dots(std::string text) {
    while (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::string short_name(const std::string& dns_name, const std::string& fallback) {
    const std::string label =
        dns_name.empty() ? std::string() : dns_name.substr(0, dns_name.find('.'));
    return label.empty() ? fallback : label;
}

bool is_ipv4_text(std::string_view text) {
    return text.find('.') != std::string_view::npos && text.find(':') == std::string_view::npos;
}

// _ipv4: the first string with a dot and no colon, without its "/prefix". It iterates whatever
// it is given, so a string is read character by character and a dict by its keys.
std::string ipv4_of(const Json& addresses) {
    if (!truthy(addresses)) {
        return "";
    }
    if (addresses.is_string()) {
        // Only the character "." itself has a dot and no colon.
        return addresses.get_ref<const std::string&>().find('.') != std::string::npos ? "." : "";
    }
    if (addresses.is_array()) {
        for (const auto& address : addresses) {
            if (address.is_string() && is_ipv4_text(address.get_ref<const std::string&>())) {
                const auto& text = address.get_ref<const std::string&>();
                return text.substr(0, text.find('/'));
            }
        }
        return "";
    }
    if (addresses.is_object()) {
        for (const auto& [key, item] : addresses.items()) {
            if (is_ipv4_text(key)) {
                return key.substr(0, key.find('/'));
            }
        }
        return "";
    }
    const std::string type = addresses.is_boolean()        ? "bool"
                             : addresses.is_number_float() ? "float"
                                                           : "int";
    throw py::TypeError("'" + type + "' object is not iterable");
}

const char* const kStates[][2] = {
    {"Running", "running"},        {"Stopped", "stopped"},
    {"NeedsLogin", "needs_login"}, {"NeedsMachineAuth", "needs_login"},
    {"Starting", "starting"},      {"NoState", "starting"},
};

} // namespace

bool is_tailscale_address(const IpAddress& address) {
    const IpAddress ip = unmap_v4(address);
    return in_prefix(ip, kTailscaleV4, 10) || in_prefix(ip, kTailscaleV6, 48);
}

bool is_tailscale_address(std::string_view address) {
    const auto ip = parse_address(strip_scope(address));
    return ip && is_tailscale_address(*ip);
}

bool TailscalePeer::is_desktop() const {
    const std::string lowered = py::lower(os);
    return lowered == "windows" || lowered == "linux" || lowered == "macos";
}

TailscaleStatus parse_status(const Json& data) {
    if (!data.is_object()) {
        throw std::invalid_argument("parse_status needs a JSON object");
    }
    TailscaleStatus status;
    status.state = "unavailable";
    const std::string backend = str_or_empty(get(data, "BackendState"));
    for (const auto& [from, to] : kStates) {
        if (backend == from) {
            status.state = to;
            break;
        }
    }
    const Json& users = object_or_empty(data, "User");
    const auto owner_of = [&](const Json& node) -> std::string {
        const Json& user = get(users, py_str(get(node, "UserID")));
        return user.is_object() ? str_or_empty(get(user, "LoginName")) : std::string();
    };

    const Json& me = object_or_empty(data, "Self");
    const std::string my_owner = truthy(get(me, "Tags")) ? std::string() : owner_of(me);
    const Json& raw_peers = object_or_empty(data, "Peer");
    for (const auto& [key, node] : raw_peers.items()) {
        if (!node.is_object()) {
            continue;
        }
        std::string address = ipv4_of(get(node, "TailscaleIPs"));
        if (address.empty()) {
            continue;
        }
        TailscalePeer peer;
        peer.dns_name = rstrip_dots(str_or_empty(get(node, "DNSName")));
        peer.tagged = truthy(get(node, "Tags"));
        peer.owner = peer.tagged ? std::string() : owner_of(node);
        const Json& host_name = get(node, "HostName");
        peer.name = short_name(peer.dns_name, truthy(host_name) ? py_str(host_name) : address);
        peer.address = std::move(address);
        peer.os = str_or_empty(get(node, "OS"));
        peer.online = truthy(get(node, "Online"));
        // Shared-in nodes belong to someone else even if the login matches.
        peer.mine = !my_owner.empty() && peer.owner == my_owner && !truthy(get(node, "ShareeNode"));
        status.peers.push_back(std::move(peer));
    }
    std::ranges::stable_sort(status.peers, [](const TailscalePeer& a, const TailscalePeer& b) {
        if (a.mine != b.mine) {
            return a.mine;
        }
        return py::lower(a.name) < py::lower(b.name);
    });
    const Json& tailnet = object_or_empty(data, "CurrentTailnet");
    status.dns_name = rstrip_dots(str_or_empty(get(me, "DNSName")));
    status.name = short_name(status.dns_name, str_or_empty(get(me, "HostName")));
    status.address = ipv4_of(get(me, "TailscaleIPs"));
    status.owner = my_owner;
    status.tailnet = str_or_empty(get(tailnet, "Name"));
    return status;
}

} // namespace fv::net
