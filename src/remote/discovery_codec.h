#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "remote/wire.h"

// Finding sharing computers on this network without Tailscale: the datagrams, who gets an
// answer, and where a search and a sweep send (services/remote_asr/discovery.py, pure parts).
//
// A computer that shares its engine answers a small UDP query on DISCOVERY_PORT with who it is:
// its name, engine, TCP port, certificate fingerprint, and whether it takes pairing requests. A
// computer looking for one broadcasts the query on each of its network cards and collects the
// answers for about a second. Nothing in an answer is secret (any TLS connection shows the
// fingerprint) and nothing in one is trusted: pairing pins the certificate, so a forged answer
// can at worst list a computer that then fails to connect. The responder never answers with more
// bytes than the query held, answers only private, link-local, loopback and Tailscale addresses,
// and limits how often it answers each of them.
//
// Every function here is pure and thread-safe.
namespace fv::remote {

// How long a search listens for answers, in seconds.
inline constexpr double SEARCH_S = 1.2;
// When, within a search, the query goes out, in seconds (UDP may drop any one of them).
inline constexpr std::array<double, 3> SEND_AT_S = {0.0, 0.3, 0.7};
// Answers per sender address, and in all, within RATE_WINDOW_S seconds; past
// MAX_TRACKED_SENDERS senders the responder forgets those with nothing recent.
inline constexpr int MAX_ANSWERS_PER_SENDER = 20;
inline constexpr int MAX_ANSWERS = 200;
inline constexpr double RATE_WINDOW_S = 10.0;
inline constexpr std::size_t MAX_TRACKED_SENDERS = 1024;
// The responder's receive timeout, in seconds.
inline constexpr double RESPONDER_POLL_S = 0.5;
// A sweep's TCP connect timeout (seconds), knocks at a time, and networks covered at most.
inline constexpr double SWEEP_CONNECT_S = 0.4;
inline constexpr int SWEEP_WORKERS = 64;
inline constexpr int SWEEP_NETWORKS = 2;
// An answer cut to fit: the name at MAX_NAME code points, then at this many.
inline constexpr std::size_t REPLY_SHORT_NAME = 16;

// A computer on this network that answered: sharing, and who it is.
struct LanHost {
    std::string address;
    int port = 0;
    std::string host_name;
    std::string fingerprint; // uppercase
    Json engine = Json::object();
    std::int64_t protocol = 0;
    // Takes pairing requests that its owner allows on its own screen.
    bool approval = false;

    std::string where() const; // format_address(address, port)
    bool compatible() const { return protocol == PROTOCOL_VERSION; }
    friend bool operator==(const LanHost&, const LanHost&) = default;
};

// QUERY_MAGIC + nonce, zero-padded to QUERY_BYTES (nothing is added past a longer one).
std::vector<std::byte> make_query(std::span<const std::uint8_t> nonce);
// The nonce of a well-formed query: at least QUERY_BYTES long and starting with QUERY_MAGIC;
// nullopt otherwise (short ones included: no answer to amplify).
std::optional<std::vector<std::uint8_t>> parse_query(std::span<const std::byte> data);

// `info` (name, port, fingerprint, approval, engine) as REPLY_MAGIC + compact JSON
// {n, name, port, fp, protocol, approval, engine{label, device, available, family}}, no longer
// than a query: if too long, engine becomes {available}, then the name is cut to
// REPLY_SHORT_NAME; if still too long, empty. port is int(info["port"] or 0): py::ValueError or
// py::TypeError where Python's int() raises.
std::vector<std::byte> encode_reply(std::span<const std::uint8_t> nonce, const Json& info);
// An answer to the query sent with `nonce`, from `address`, else nullopt: the magic, UTF-8 JSON
// object, `n` the nonce's hex, `fp` 64 hex digits, `port` an int (not a bool) in 1..65535. The
// name keeps its printable characters, stripped, at most MAX_NAME, else the address. protocol
// is int(protocol), 0 where Python's int() raises TypeError or ValueError (and for a finite
// value past int64, which Python keeps and which is no protocol either); py::OverflowError for
// an infinite one, as Python raises.
std::optional<LanHost> parse_reply(std::span<const std::byte> data,
                                   std::span<const std::uint8_t> nonce, std::string_view address);

// Whether a query from `address` gets an answer: this network's, never the internet's. Private,
// link-local, loopback, or IPv4 in 100.64.0.0/10; never multicast or unspecified; junk is false.
bool allowed_sender(std::string_view address);

// One of this computer's IPv4 addresses, as services/lan_address reports it.
struct LanCard {
    std::string address;
    int prefix = 0;
    bool lan = true;         // kind "lan" (not a VPN, virtual or Tailscale card)
    bool is_private = false; // ipaddress.ip_address(address).is_private
};

// Each private LAN card's broadcast address (prefix 8..30), then 255.255.255.255, without
// repeats. Windows sends 255.255.255.255 out of one card only, so each card's own broadcast
// address is what reaches the computers beside it. py::ValueError for an address that is not
// IPv4.
std::vector<std::string> broadcast_targets(std::span<const LanCard> cards);
// Every other address of this computer's private LAN networks, each at least a /24 (a prefix of
// 0 counts as 24), the first SWEEP_NETWORKS distinct networks only, without repeats.
// py::ValueError for an address that is not IPv4 or a prefix past 32.
std::vector<std::string> sweep_addresses(std::span<const LanCard> cards);

} // namespace fv::remote
