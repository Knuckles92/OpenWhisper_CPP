#include "net/address.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include "base/executor.h"
#include "net/detail/platform.h"

namespace fv::net {

namespace {

struct Network {
    int version;
    std::array<std::uint8_t, 16> bytes;
    int prefix;
};

constexpr Network v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, int prefix) {
    return {4, {a, b, c, d}, prefix};
}

constexpr Network v6(std::initializer_list<std::uint16_t> hextets, int prefix) {
    Network network{6, {}, prefix};
    std::size_t i = 0;
    for (const std::uint16_t hextet : hextets) {
        network.bytes[i++] = static_cast<std::uint8_t>(hextet >> 8);
        network.bytes[i++] = static_cast<std::uint8_t>(hextet & 0xff);
    }
    return network;
}

// ipaddress._IPv4Constants and _IPv6Constants of Python 3.12.10.
constexpr Network kV4Loopback = v4(127, 0, 0, 0, 8);
constexpr Network kV4LinkLocal = v4(169, 254, 0, 0, 16);
constexpr Network kV4Public = v4(100, 64, 0, 0, 10); // shared address space: neither
constexpr Network kV4Private[] = {
    v4(0, 0, 0, 0, 8),      v4(10, 0, 0, 0, 8),         v4(127, 0, 0, 0, 8),
    v4(169, 254, 0, 0, 16), v4(172, 16, 0, 0, 12),      v4(192, 0, 0, 0, 24),
    v4(192, 0, 0, 170, 31), v4(192, 0, 2, 0, 24),       v4(192, 168, 0, 0, 16),
    v4(198, 18, 0, 0, 15),  v4(198, 51, 100, 0, 24),    v4(203, 0, 113, 0, 24),
    v4(240, 0, 0, 0, 4),    v4(255, 255, 255, 255, 32),
};
constexpr Network kV4PrivateExceptions[] = {v4(192, 0, 0, 9, 32), v4(192, 0, 0, 10, 32)};

constexpr Network kV6LinkLocal = v6({0xfe80}, 10);
constexpr Network kV6Private[] = {
    v6({0, 0, 0, 0, 0, 0, 0, 1}, 128),
    v6({}, 128),
    v6({0, 0, 0, 0, 0, 0xffff}, 96),
    v6({0x64, 0xff9b, 1}, 48),
    v6({0x100}, 64),
    v6({0x2001}, 23),
    v6({0x2001, 0xdb8}, 32),
    v6({0x2002}, 16),
    v6({0x3fff}, 20),
    v6({0xfc00}, 7),
    v6({0xfe80}, 10),
};
constexpr Network kV6PrivateExceptions[] = {
    v6({0x2001, 1, 0, 0, 0, 0, 0, 1}, 128),
    v6({0x2001, 1, 0, 0, 0, 0, 0, 2}, 128),
    v6({0x2001, 3}, 32),
    v6({0x2001, 4, 0x112}, 48),
    v6({0x2001, 0x20}, 28),
    v6({0x2001, 0x30}, 28),
};

bool contains(const Network& network, const IpAddress& address) {
    if (network.version != address.version) {
        return false;
    }
    int bits = network.prefix;
    for (std::size_t i = 0; bits > 0; ++i, bits -= 8) {
        const auto mask = static_cast<std::uint8_t>(bits >= 8 ? 0xff : (0xff << (8 - bits)) & 0xff);
        if ((address.bytes[i] & mask) != (network.bytes[i] & mask)) {
            return false;
        }
    }
    return true;
}

bool in_any(std::span<const Network> networks, const IpAddress& address) {
    return std::ranges::any_of(networks,
                               [&](const Network& network) { return contains(network, address); });
}

std::optional<IpAddress> ipv4_mapped(const IpAddress& address) {
    if (address.version != 6) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 10; ++i) {
        if (address.bytes[i] != 0) {
            return std::nullopt;
        }
    }
    if (address.bytes[10] != 0xff || address.bytes[11] != 0xff) {
        return std::nullopt;
    }
    IpAddress mapped;
    std::copy_n(address.bytes.begin() + 12, 4, mapped.bytes.begin());
    return mapped;
}

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

int hex_value(char c) {
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

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

// IPv4Address._ip_int_from_string.
std::optional<std::array<std::uint8_t, 4>> parse_v4(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto octets = split(text, '.');
    if (octets.size() != 4) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto octet = octets[i];
        if (octet.empty() || octet.size() > 3 || !std::ranges::all_of(octet, is_ascii_digit)) {
            return std::nullopt;
        }
        if (octet.size() > 1 && octet.front() == '0') {
            return std::nullopt; // ambiguous (octal or decimal)
        }
        int value = 0;
        for (const char c : octet) {
            value = value * 10 + (c - '0');
        }
        if (value > 255) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>(value);
    }
    return out;
}

std::optional<std::uint16_t> parse_hextet(std::string_view text) {
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        const int digit = hex_value(c);
        if (digit < 0) {
            return std::nullopt;
        }
        value = value * 16 + digit;
    }
    return static_cast<std::uint16_t>(value);
}

// IPv6Address._ip_int_from_string, after the scope id is split off.
std::optional<std::array<std::uint8_t, 16>> parse_v6(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    for (const auto part : split(text, ':')) {
        parts.emplace_back(part);
    }
    constexpr std::size_t kMinParts = 3;
    if (parts.size() < kMinParts) {
        return std::nullopt;
    }
    if (parts.back().find('.') != std::string::npos) {
        const auto tail = parse_v4(parts.back());
        if (!tail) {
            return std::nullopt;
        }
        parts.pop_back();
        static constexpr char kHex[] = "0123456789abcdef";
        for (std::size_t half = 0; half < 2; ++half) {
            const unsigned value = (unsigned{(*tail)[half * 2]} << 8) | (*tail)[half * 2 + 1];
            std::string hextet;
            bool started = false;
            for (int shift = 12; shift >= 0; shift -= 4) {
                const unsigned digit = (value >> shift) & 0xf;
                if (digit != 0 || started || shift == 0) {
                    hextet += kHex[digit];
                    started = true;
                }
            }
            parts.push_back(hextet);
        }
    }
    constexpr std::size_t kHextets = 8;
    if (parts.size() > kHextets + 1) {
        return std::nullopt;
    }
    std::optional<std::size_t> skip_index;
    for (std::size_t i = 1; i + 1 < parts.size(); ++i) {
        if (parts[i].empty()) {
            if (skip_index) {
                return std::nullopt; // at most one "::"
            }
            skip_index = i;
        }
    }
    std::size_t parts_hi = 0;
    std::size_t parts_lo = 0;
    std::size_t parts_skipped = 0;
    if (skip_index) {
        auto hi = static_cast<std::ptrdiff_t>(*skip_index);
        auto lo = static_cast<std::ptrdiff_t>(parts.size() - *skip_index - 1);
        if (parts.front().empty()) {
            if (--hi != 0) {
                return std::nullopt; // a leading ':' only as part of "::"
            }
        }
        if (parts.back().empty()) {
            if (--lo != 0) {
                return std::nullopt;
            }
        }
        const auto skipped = static_cast<std::ptrdiff_t>(kHextets) - (hi + lo);
        if (skipped < 1) {
            return std::nullopt;
        }
        parts_hi = static_cast<std::size_t>(hi);
        parts_lo = static_cast<std::size_t>(lo);
        parts_skipped = static_cast<std::size_t>(skipped);
    } else {
        if (parts.size() != kHextets || parts.front().empty() || parts.back().empty()) {
            return std::nullopt;
        }
        parts_hi = parts.size();
    }
    std::array<std::uint16_t, kHextets> hextets{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < parts_hi; ++i) {
        const auto hextet = parse_hextet(parts[i]);
        if (!hextet) {
            return std::nullopt;
        }
        hextets[out++] = *hextet;
    }
    out += parts_skipped;
    for (std::size_t i = parts.size() - parts_lo; i < parts.size(); ++i) {
        const auto hextet = parse_hextet(parts[i]);
        if (!hextet) {
            return std::nullopt;
        }
        hextets[out++] = *hextet;
    }
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < kHextets; ++i) {
        bytes[i * 2] = static_cast<std::uint8_t>(hextets[i] >> 8);
        bytes[i * 2 + 1] = static_cast<std::uint8_t>(hextets[i] & 0xff);
    }
    return bytes;
}

} // namespace

std::string IpAddress::to_string() const {
    if (version == 4) {
        return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
               std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
    }
    std::array<unsigned, 8> hextets{};
    for (std::size_t i = 0; i < 8; ++i) {
        hextets[i] = (unsigned{bytes[i * 2]} << 8) | bytes[i * 2 + 1];
    }
    // _compress_hextets: the first longest run of two or more zero hextets becomes "::".
    int best_start = -1;
    int best_len = 0;
    int start = -1;
    int len = 0;
    for (int i = 0; i < 8; ++i) {
        if (hextets[static_cast<std::size_t>(i)] == 0) {
            ++len;
            if (start == -1) {
                start = i;
            }
            if (len > best_len) {
                best_len = len;
                best_start = start;
            }
        } else {
            len = 0;
            start = -1;
        }
    }
    static constexpr char kHex[] = "0123456789abcdef";
    const auto format = [](unsigned value) {
        std::string text;
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            const unsigned digit = (value >> shift) & 0xf;
            if (digit != 0 || started || shift == 0) {
                text += kHex[digit];
                started = true;
            }
        }
        return text;
    };
    std::string text;
    if (best_len > 1) {
        for (int i = 0; i < best_start; ++i) {
            text += format(hextets[static_cast<std::size_t>(i)]);
            text += ':';
        }
        if (best_start == 0) {
            text += ':';
        }
        text += ':';
        for (int i = best_start + best_len; i < 8; ++i) {
            text += format(hextets[static_cast<std::size_t>(i)]);
            if (i < 7) {
                text += ':';
            }
        }
    } else {
        for (std::size_t i = 0; i < 8; ++i) {
            if (i > 0) {
                text += ':';
            }
            text += format(hextets[i]);
        }
    }
    if (!scope_id.empty()) {
        text += '%';
        text += scope_id;
    }
    return text;
}

std::optional<IpAddress> parse_address(std::string_view text) {
    if (const auto quad = parse_v4(text)) {
        IpAddress address;
        std::copy(quad->begin(), quad->end(), address.bytes.begin());
        return address;
    }
    if (text.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    // _split_scope_id: one '%' followed by a non-empty scope without another '%'.
    std::string_view scope;
    const auto percent = text.find('%');
    if (percent != std::string_view::npos) {
        scope = text.substr(percent + 1);
        text = text.substr(0, percent);
        if (scope.empty() || scope.find('%') != std::string_view::npos) {
            return std::nullopt;
        }
    }
    const auto bytes = parse_v6(text);
    if (!bytes) {
        return std::nullopt;
    }
    IpAddress address;
    address.version = 6;
    address.bytes = *bytes;
    address.scope_id = std::string(scope);
    return address;
}

bool is_loopback(const IpAddress& address) {
    if (const auto mapped = ipv4_mapped(address)) {
        return is_loopback(*mapped);
    }
    if (address.version == 4) {
        return contains(kV4Loopback, address);
    }
    return contains(kV6Private[0], address); // ::1/128
}

bool is_private(const IpAddress& address) {
    if (const auto mapped = ipv4_mapped(address)) {
        return is_private(*mapped);
    }
    if (address.version == 4) {
        return in_any(kV4Private, address) && !in_any(kV4PrivateExceptions, address);
    }
    return in_any(kV6Private, address) && !in_any(kV6PrivateExceptions, address);
}

bool is_link_local(const IpAddress& address) {
    if (const auto mapped = ipv4_mapped(address)) {
        return is_link_local(*mapped);
    }
    return address.version == 4 ? contains(kV4LinkLocal, address) : contains(kV6LinkLocal, address);
}

bool is_global(const IpAddress& address) {
    if (const auto mapped = ipv4_mapped(address)) {
        return is_global(*mapped);
    }
    if (address.version == 4) {
        return !contains(kV4Public, address) && !is_private(address);
    }
    return !is_private(address);
}

std::string_view strip_scope(std::string_view text) {
    return text.substr(0, text.find('%'));
}

IpAddress unmap_v4(const IpAddress& address) {
    if (auto mapped = ipv4_mapped(address)) {
        return std::move(*mapped);
    }
    return address;
}

namespace {

using LookupFunction =
    std::function<std::expected<std::vector<IpAddress>, std::string>(const std::string&)>;

std::mutex& lookup_override_mutex() {
    static std::mutex mutex;
    return mutex;
}

LookupFunction& lookup_override() {
    static LookupFunction lookup;
    return lookup;
}

} // namespace

void detail::set_lookup_override(LookupFunction lookup) {
    std::lock_guard lock(lookup_override_mutex());
    lookup_override() = std::move(lookup);
}

std::expected<std::vector<IpAddress>, std::string> resolve_host(std::string_view name,
                                                                const CancelToken& cancel) {
    assert_not_gui_thread();
    if (cancel.cancelled()) {
        return std::unexpected(std::string("cancelled"));
    }
    struct Lookup {
        std::mutex mutex;
        std::condition_variable done_or_cancelled;
        bool done = false;
        bool cancelled = false;
        std::expected<std::vector<IpAddress>, std::string> result;
    };
    auto lookup = std::make_shared<Lookup>();
    const auto subscription = cancel.on_cancel([lookup] {
        {
            std::lock_guard lock(lookup->mutex);
            lookup->cancelled = true;
        }
        lookup->done_or_cancelled.notify_all();
    });
    LookupFunction replaced;
    {
        std::lock_guard lock(lookup_override_mutex());
        replaced = lookup_override();
    }
    // getaddrinfo cannot be interrupted: a cancelled wait leaves the thread to finish alone. It
    // owns everything it touches (the shared Lookup, its own copies of the name and function),
    // so it may outlive this call, the caller and the token.
    std::thread([lookup, host = std::string(name), replaced = std::move(replaced)] {
        auto result = replaced ? replaced(host) : detail::getaddrinfo_stream(host);
        {
            std::lock_guard lock(lookup->mutex);
            lookup->result = std::move(result);
            lookup->done = true;
        }
        lookup->done_or_cancelled.notify_all();
    }).detach();
    std::unique_lock lock(lookup->mutex);
    lookup->done_or_cancelled.wait(lock, [&] { return lookup->done || lookup->cancelled; });
    if (!lookup->done) {
        return std::unexpected(std::string("cancelled"));
    }
    return lookup->result;
}

bool can_bind(const IpAddress& address) {
    return detail::probe_bind(address);
}

} // namespace fv::net
