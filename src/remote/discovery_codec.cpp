#include "remote/discovery_codec.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>

#include "base/hash.h"
#include "net/address.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "remote/detail/pyvalue.h"
#include "remote/protocol.h"

namespace fv::remote {

namespace {

const Json& member(const Json& object, std::string_view key) {
    static const Json none;
    if (!object.is_object()) {
        return none;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? none : *found;
}

std::string printable_stripped(std::string_view text) {
    std::string kept;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char32_t cp = py::next_code_point(text, pos);
        if (py::is_printable(cp)) {
            py::append_utf8(kept, cp);
        }
    }
    return py::strip(kept);
}

std::int64_t int_or_raise(const Json& value) {
    const auto converted = py::py_int_checked(value);
    if (converted) {
        return *converted;
    }
    switch (converted.error()) {
    case py::IntError::Type:
        throw py::TypeError("int() argument must be a string, a bytes-like object or a real "
                            "number, not '" +
                            std::string(value.type_name()) + "'");
    case py::IntError::Value:
        throw py::ValueError("invalid literal for int()");
    case py::IntError::Overflow:
        break;
    }
    throw py::OverflowError("cannot convert float infinity to integer");
}

std::uint32_t ipv4_bits(std::string_view address) {
    const auto parsed = net::parse_address(address);
    if (!parsed || parsed->version != 4) {
        throw py::ValueError(
            std::format("'{}' does not appear to be an IPv4 or IPv6 network", address));
    }
    return (std::uint32_t{parsed->bytes[0]} << 24) | (std::uint32_t{parsed->bytes[1]} << 16) |
           (std::uint32_t{parsed->bytes[2]} << 8) | std::uint32_t{parsed->bytes[3]};
}

std::uint32_t mask_of(int prefix) {
    if (prefix < 0 || prefix > 32) {
        throw py::ValueError(std::format("{} is not a valid netmask", prefix));
    }
    return prefix == 0 ? 0u : ~std::uint32_t{0} << (32 - prefix);
}

std::string ipv4_text(std::uint32_t bits) {
    return std::format("{}.{}.{}.{}", bits >> 24, (bits >> 16) & 0xFF, (bits >> 8) & 0xFF,
                       bits & 0xFF);
}

void add_unique(std::vector<std::string>& out, std::set<std::string>& seen, std::string value) {
    if (seen.insert(value).second) {
        out.push_back(std::move(value));
    }
}

bool is_multicast(const net::IpAddress& ip) {
    if (ip.version == 4) {
        return (ip.bytes[0] & 0xF0) == 0xE0; // 224.0.0.0/4
    }
    return ip.bytes[0] == 0xFF; // ff00::/8
}

bool is_unspecified(const net::IpAddress& ip) {
    const std::size_t size = ip.version == 4 ? 4 : 16;
    return std::all_of(ip.bytes.begin(), ip.bytes.begin() + static_cast<std::ptrdiff_t>(size),
                       [](std::uint8_t byte) { return byte == 0; });
}

} // namespace

std::string LanHost::where() const {
    return format_address(address, port);
}

std::vector<std::byte> make_query(std::span<const std::uint8_t> nonce) {
    std::vector<std::byte> query;
    query.reserve(std::max(QUERY_BYTES, QUERY_MAGIC.size() + nonce.size()));
    for (const char ch : QUERY_MAGIC) {
        query.push_back(static_cast<std::byte>(ch));
    }
    for (const std::uint8_t byte : nonce) {
        query.push_back(static_cast<std::byte>(byte));
    }
    if (query.size() < QUERY_BYTES) {
        query.resize(QUERY_BYTES, std::byte{0});
    }
    return query;
}

std::optional<std::vector<std::uint8_t>> parse_query(std::span<const std::byte> data) {
    if (data.size() < QUERY_BYTES || !detail::as_chars(data).starts_with(QUERY_MAGIC)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> nonce;
    for (const std::byte byte : data.subspan(QUERY_MAGIC.size(), DISCOVERY_NONCE_BYTES)) {
        nonce.push_back(std::to_integer<std::uint8_t>(byte));
    }
    return nonce;
}

std::vector<std::byte> encode_reply(std::span<const std::uint8_t> nonce, const Json& info) {
    if (!info.is_object()) {
        throw std::invalid_argument("encode_reply: info is not an object");
    }
    const Json& raw_engine = member(info, "engine");
    const Json engine = raw_engine.is_object() ? raw_engine : Json::object();
    const Json& raw_port = member(info, "port");
    Json reply = Json::object();
    reply["n"] = to_hex(nonce);
    reply["name"] = py::slice(detail::py_str_or_empty(member(info, "name")), 0,
                              static_cast<std::int64_t>(MAX_NAME));
    reply["port"] = detail::truthy(raw_port) ? int_or_raise(raw_port) : std::int64_t{0};
    reply["fp"] = detail::py_str_or_empty(member(info, "fingerprint"));
    reply["protocol"] = PROTOCOL_VERSION;
    const Json& approval = member(info, "approval");
    reply["approval"] = approval.is_boolean() && approval.get<bool>();
    Json summary = Json::object();
    for (const std::string_view key : {"label", "device", "available", "family"}) {
        summary[std::string(key)] = member(engine, key);
    }
    reply["engine"] = std::move(summary);
    for (const int trim : {0, 1, 2}) {
        if (trim == 1) {
            reply["engine"] = Json{{"available", member(engine, "available")}};
        } else if (trim == 2) {
            reply["name"] = py::slice(reply["name"].get<std::string>(), 0,
                                      static_cast<std::int64_t>(REPLY_SHORT_NAME));
        }
        const std::string data = std::string(REPLY_MAGIC) + dumps_compact(reply);
        if (data.size() <= QUERY_BYTES) {
            const auto bytes = detail::as_bytes(data);
            return {bytes.begin(), bytes.end()};
        }
    }
    return {};
}

std::optional<LanHost> parse_reply(std::span<const std::byte> data,
                                   std::span<const std::uint8_t> nonce, std::string_view address) {
    const std::string_view chars = detail::as_chars(data);
    if (!chars.starts_with(REPLY_MAGIC)) {
        return std::nullopt;
    }
    const auto text = detail::decode_utf8(chars.substr(REPLY_MAGIC.size()));
    if (!text) {
        return std::nullopt;
    }
    Json reply;
    try {
        reply = py::loads(*text);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    if (!reply.is_object()) {
        return std::nullopt;
    }
    const Json& n = member(reply, "n");
    if (!n.is_string() || n.get_ref<const std::string&>() != to_hex(nonce)) {
        return std::nullopt;
    }
    const Json& fingerprint = member(reply, "fp");
    const Json& port = member(reply, "port");
    if (!fingerprint.is_string()) {
        return std::nullopt;
    }
    const std::string& fp = fingerprint.get_ref<const std::string&>();
    if (fp.size() != 64 || !std::all_of(fp.begin(), fp.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        })) {
        return std::nullopt;
    }
    if (!detail::is_int(port)) {
        return std::nullopt;
    }
    const auto port_value = py::py_int(port);
    if (!port_value || *port_value < 1 || *port_value > 65535) {
        return std::nullopt;
    }
    std::int64_t version = 0;
    const Json& protocol = member(reply, "protocol");
    if (const auto converted = py::py_int_checked(protocol)) {
        version = *converted;
    } else if (converted.error() == py::IntError::Overflow && protocol.is_number_float() &&
               std::isinf(protocol.get<double>())) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    const Json& engine = member(reply, "engine");
    std::string name = py::slice(printable_stripped(detail::py_str_or_empty(member(reply, "name"))),
                                 0, static_cast<std::int64_t>(MAX_NAME));
    LanHost host;
    host.address = std::string(address);
    host.port = static_cast<int>(*port_value);
    host.host_name = name.empty() ? std::string(address) : std::move(name);
    std::string upper = fp;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char ch) {
        return ch >= 'a' && ch <= 'f' ? static_cast<char>(ch - 32) : ch;
    });
    host.fingerprint = std::move(upper);
    host.engine = engine.is_object() ? engine : Json::object();
    host.protocol = version;
    const Json& approval = member(reply, "approval");
    host.approval = approval.is_boolean() && approval.get<bool>();
    return host;
}

bool allowed_sender(std::string_view address) {
    const auto parsed = net::parse_address(address);
    if (!parsed) {
        return false;
    }
    const net::IpAddress plain = net::unmap_v4(*parsed);
    if (is_multicast(plain) || is_unspecified(plain)) {
        return false;
    }
    if (net::is_private(*parsed) || net::is_link_local(*parsed) || net::is_loopback(*parsed)) {
        return true;
    }
    // 100.64.0.0/10, Tailscale's addresses: IPv4 only, as Python checks ip.version == 4.
    return parsed->version == 4 && parsed->bytes[0] == 100 && (parsed->bytes[1] & 0xC0) == 64;
}

std::vector<std::string> broadcast_targets(std::span<const LanCard> cards) {
    std::vector<std::string> targets;
    std::set<std::string> seen;
    for (const LanCard& card : cards) {
        if (!card.lan || !card.is_private || card.prefix < 8 || card.prefix > 30) {
            continue;
        }
        const std::uint32_t bits = ipv4_bits(card.address);
        add_unique(targets, seen, ipv4_text(bits | ~mask_of(card.prefix)));
    }
    add_unique(targets, seen, "255.255.255.255");
    return targets;
}

std::vector<std::string> sweep_addresses(std::span<const LanCard> cards) {
    std::vector<std::pair<std::uint32_t, int>> networks;
    std::vector<std::string> addresses;
    std::set<std::string> seen;
    for (const LanCard& card : cards) {
        if (!card.lan || !card.is_private) {
            continue;
        }
        const int prefix = std::max(card.prefix == 0 ? 24 : card.prefix, 24);
        const std::uint32_t mask = mask_of(prefix);
        const std::uint32_t network = ipv4_bits(card.address) & mask;
        if (std::find(networks.begin(), networks.end(), std::pair(network, prefix)) !=
            networks.end()) {
            continue;
        }
        networks.emplace_back(network, prefix);
        // IPv4Network.hosts(): without the network and broadcast addresses, except that a /31
        // has both of its addresses and a /32 its one.
        const std::uint32_t last = network | ~mask;
        std::uint32_t first = network;
        std::uint32_t end = last;
        if (prefix < 31) {
            first = network + 1;
            end = last - 1;
        }
        for (std::uint64_t ip = first; ip <= end; ++ip) {
            std::string text = ipv4_text(static_cast<std::uint32_t>(ip));
            if (text != card.address) {
                add_unique(addresses, seen, std::move(text));
            }
        }
        if (networks.size() >= static_cast<std::size_t>(SWEEP_NETWORKS)) {
            break;
        }
    }
    return addresses;
}

} // namespace fv::remote
