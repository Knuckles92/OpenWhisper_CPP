#include "remote/client.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

#include "base/executor.h"
#include "base/ids.h"
#include "net/lan_address.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "remote/detail/client_socket.h"
#include "remote/detail/pyvalue.h"
#include "remote/wire.h"

namespace fv::remote {

namespace {

using detail::ClientSocket;

const Json& member(const Json& object, std::string_view key) {
    static const Json none;
    if (!object.is_object()) {
        return none;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? none : *found;
}

// reply.get(key) if isinstance(reply.get(key), dict) else {}
Json object_member(const Json& object, std::string_view key) {
    const Json& value = member(object, key);
    return value.is_object() ? value : Json::object();
}

bool is_true(const Json& value) {
    return value.is_boolean() && value.get<bool>();
}

// str(value or fallback)
std::string str_or(const Json& value, std::string_view fallback) {
    return detail::truthy(value) ? detail::py_str(value) : std::string(fallback);
}

// int(value), 0 where Python's int() raises TypeError or ValueError (and for a finite value past
// int64, which Python keeps and which is no protocol either); OverflowError for an infinite one.
std::int64_t protocol_version(const Json& value) {
    if (const auto converted = py::py_int_checked(value)) {
        return *converted;
    } else if (converted.error() == py::IntError::Overflow && value.is_number_float() &&
               std::isinf(value.get<double>())) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    return 0;
}

// The digits of the code as str.isdigit() finds them, in any script.
std::string code_digits(std::string_view code) {
    std::u32string kept;
    for (const char32_t cp : py::to_code_points(code)) {
        if (py::is_digit(cp)) {
            kept.push_back(cp);
        }
    }
    return py::to_utf8(kept);
}

// Sends one of the client's text frames. A host that closed right after opening is a closed
// connection, as the read that follows would find it.
void send_json(ClientSocket& socket, const Json& message) {
    if (!socket.send_text(dumps_default(message))) {
        throw RemoteEngineError("The host closed the connection.");
    }
}

// Runs `body` on an open socket and closes it quietly whatever happens.
template <class F> auto with_socket(const std::shared_ptr<ClientSocket>& socket, F&& body) {
    struct Closer {
        ClientSocket& socket;
        ~Closer() { detail::quiet_close(socket); }
    } closer{*socket};
    return body(*socket);
}

// _pairing_result: the host's `paired` reply, checked against the certificate this socket saw.
PairingResult pairing_result(const Json& reply, const std::string& fingerprint,
                             std::string_view host, std::string_view via) {
    if (member(reply, "type") == "error") {
        throw RemoteEngineError(str_or(member(reply, "message"), "The host refused to pair."));
    }
    const Json& token = member(reply, "token");
    const Json host_info = object_member(reply, "host");
    if (member(reply, "type") != "paired" || !token.is_string() ||
        token.get_ref<const std::string&>().empty()) {
        throw RemoteEngineError("The host sent a pairing reply this version can't read.");
    }
    const std::string advertised = detail::py_str_or_empty(member(host_info, "fingerprint"));
    if (!advertised.empty() && !detail::compare_digest(py::upper(advertised), fingerprint)) {
        // The certificate seen here is not the one the host says it has: someone is between the
        // two computers.
        throw RemoteEngineError("The host's certificate didn't match what it reported. Pairing "
                                "was stopped; try again on a network you trust.");
    }
    const Json& addresses = member(host_info, "addresses");
    PairingResult result;
    result.token = token.get<std::string>();
    result.device_id = detail::py_str_or_empty(member(reply, "device_id"));
    result.host_name = str_or(member(host_info, "name"), host);
    result.fingerprint = fingerprint;
    if (addresses.is_array()) {
        for (const Json& address : addresses) {
            if (!address.is_string()) {
                continue;
            }
            const auto& text = address.get_ref<const std::string&>();
            if (text.empty() || text == host) {
                continue;
            }
            if (std::find(result.alternates.begin(), result.alternates.end(), text) ==
                result.alternates.end()) {
                result.alternates.push_back(text);
            }
        }
    }
    result.via = std::string(via);
    return result;
}

// _await_decision: the host's answer once its owner decides; a cancel tells the host and stops.
Json await_decision(ClientSocket& socket, const CancelToken& canceled) {
    const auto deadline = std::chrono::steady_clock::now() + APPROVAL_WAIT;
    while (true) {
        if (canceled.cancelled()) {
            // Whether the host hears it or not, the close that follows tells it.
            (void)socket.send_text(dumps_default(Json{{"type", "pair_cancel"}}));
            throw PairingCanceled("Pairing canceled.");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw RemoteEngineError("The host didn't answer in time. Try again.");
        }
        auto received = socket.receive(APPROVAL_POLL);
        if (!received) {
            if (received.error() == detail::ReceiveError::timeout) {
                continue;
            }
            throw RemoteEngineError("The host closed the connection before answering.");
        }
        std::optional<Json> message;
        if (!received->binary) {
            message = detail::loads_message(*received);
        }
        if (!message || !message->is_object()) {
            throw RemoteEngineError("The host sent a reply this version can't read.");
        }
        return std::move(*message);
    }
}

} // namespace

std::vector<LanCard> system_lan_cards() {
    std::vector<LanCard> cards;
    for (const auto& address : net::local_ipv4_addresses()) {
        LanCard card;
        card.address = address.address;
        card.prefix = address.prefix;
        card.lan = address.kind == net::kLan;
        card.is_private = address.is_private;
        cards.push_back(std::move(card));
    }
    return cards;
}

std::string engine_uri(std::string_view host, int port) {
    return "wss://" + format_address(host, port) + std::string(PATH);
}

ProbeResult probe_host(ClientEnv& env, std::string_view host, int port,
                       std::chrono::milliseconds timeout) {
    assert_not_gui_thread();
    const auto socket = detail::open_engine_socket(env.reactor, host, port, timeout);
    std::string fingerprint;
    const Json reply = with_socket(socket, [&](ClientSocket& open) {
        fingerprint = open.fingerprint();
        send_json(open, Json{{"type", "probe"}, {"protocol", PROTOCOL_VERSION}});
        return detail::read_json(open, timeout);
    });
    if (member(reply, "type") != "probe") {
        throw RemoteEngineError(
            std::format("{} didn't answer the probe.", format_address(host, port)));
    }
    const Json host_info = object_member(reply, "host");
    ProbeResult result;
    result.host_name = str_or(member(host_info, "name"), host);
    result.protocol = protocol_version(member(reply, "protocol"));
    result.engine = object_member(reply, "engine");
    result.tailscale_pairing = is_true(member(reply, "tailscale_pairing"));
    result.fingerprint = std::move(fingerprint);
    result.approval = is_true(member(reply, "approval"));
    return result;
}

PairingResult pair_with_host(ClientEnv& env, std::string_view host, int port, std::string_view code,
                             std::string_view device_name, bool tailscale,
                             std::chrono::milliseconds timeout) {
    assert_not_gui_thread();
    Json request = {{"type", "pair"},
                    {"protocol", PROTOCOL_VERSION},
                    {"device_name", std::string(device_name)}};
    if (tailscale) {
        request["tailscale"] = true;
    } else {
        std::string digits = code_digits(code);
        if (digits.empty()) {
            throw RemoteEngineError("Enter the pairing code shown on the host.");
        }
        request["code"] = std::move(digits);
    }
    const auto socket = detail::open_engine_socket(env.reactor, host, port, timeout);
    std::string fingerprint;
    const Json reply = with_socket(socket, [&](ClientSocket& open) {
        fingerprint = open.fingerprint();
        send_json(open, request);
        return detail::read_json(open, HANDSHAKE_TIMEOUT);
    });
    return pairing_result(reply, fingerprint, host, tailscale ? "tailscale" : "code");
}

PairingResult request_pairing(ClientEnv& env, std::string_view host, int port,
                              std::string_view device_name,
                              std::function<void(std::string sas)> on_code, CancelToken canceled,
                              std::chrono::milliseconds timeout) {
    assert_not_gui_thread();
    const auto socket = detail::open_engine_socket(env.reactor, host, port, timeout);
    std::string fingerprint;
    const Json reply = with_socket(socket, [&](ClientSocket& open) {
        fingerprint = open.fingerprint();
        send_json(open, Json{{"type", "pair_request"},
                             {"protocol", PROTOCOL_VERSION},
                             {"device_name", std::string(device_name)}});
        const Json commit_reply = detail::read_json(open, HANDSHAKE_TIMEOUT);
        if (member(commit_reply, "type") == "error") {
            throw RemoteEngineError(
                str_or(member(commit_reply, "message"), "The host refused to pair."));
        }
        const Json& commitment = member(commit_reply, "commit");
        if (member(commit_reply, "type") != "pair_commit" || !commitment.is_string()) {
            throw RemoteEngineError("The host sent a pairing reply this version can't read.");
        }
        std::array<std::byte, PAIRING_NONCE_BYTES> client_nonce{};
        random_bytes(client_nonce);
        std::vector<std::uint8_t> client_bytes;
        for (const std::byte value : client_nonce) {
            client_bytes.push_back(std::to_integer<std::uint8_t>(value));
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string client_hex;
        for (const std::uint8_t value : client_bytes) {
            client_hex += kHex[value >> 4];
            client_hex += kHex[value & 0xF];
        }
        send_json(open, Json{{"type", "pair_nonce"}, {"nonce", client_hex}});
        const Json reveal = detail::read_json(open, HANDSHAKE_TIMEOUT);
        if (member(reveal, "type") == "error") {
            throw RemoteEngineError(str_or(member(reveal, "message"), "The host refused to pair."));
        }
        const std::vector<std::uint8_t> host_nonce =
            detail::from_hex(detail::py_str_or_empty(member(reveal, "nonce")))
                .value_or(std::vector<std::uint8_t>{});
        if (member(reveal, "type") != "pair_reveal" || host_nonce.size() != PAIRING_NONCE_BYTES ||
            !detail::compare_digest(pairing_commitment(host_nonce),
                                    commitment.get_ref<const std::string&>())) {
            throw RemoteEngineError("The host's pairing answer didn't check out. Pairing was "
                                    "stopped; try again on a network you trust.");
        }
        if (on_code) {
            on_code(pairing_sas(host_nonce, client_bytes, fingerprint));
        }
        return await_decision(open, canceled);
    });
    return pairing_result(reply, fingerprint, host, "approval");
}

} // namespace fv::remote
