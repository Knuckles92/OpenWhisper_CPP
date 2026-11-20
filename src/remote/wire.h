#pragma once

#include <array>
#include <cstddef>
#include <string_view>

// The remote engine's wire constants, byte for byte as OpenWhisper's services/remote_asr
// (protocol.py, discovery.py, tls.py, settings.py) spells them, so that a FioriVoce computer and
// an OpenWhisper one pair, discover and dictate through each other. This is the one C++ file
// that may spell the other product's name (lint.brand): only where the bytes on the wire or a
// peer's own code need it.
namespace fv::remote {

inline constexpr int PROTOCOL_VERSION = 1;
// TCP port of the engine, and the UDP port discovery answers on whatever the TCP port is.
inline constexpr int DEFAULT_PORT = 47821;
inline constexpr int DISCOVERY_PORT = DEFAULT_PORT;
// The WebSocket path; the host answers anything else with HTTP 404 and NOT_AN_ENDPOINT_BODY.
inline constexpr std::string_view PATH = "/openwhisper/v1/engine";
inline constexpr std::string_view NOT_AN_ENDPOINT_BODY =
    "Not an OpenWhisper remote engine endpoint\n";
inline constexpr int SAMPLE_RATE = 16000; // Hz, mono, signed 16-bit little-endian on the wire

// Longest audio accepted in one request, in seconds. Clients send windows of at most 30 s, so
// this only bounds a misbehaving peer.
inline constexpr int MAX_AUDIO_SECONDS = 120;
inline constexpr std::size_t MAX_HEADER_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_AUDIO_BYTES =
    static_cast<std::size_t>(MAX_AUDIO_SECONDS) * SAMPLE_RATE * 2;
inline constexpr std::size_t MAX_REQUEST_BYTES = 4 + MAX_HEADER_BYTES + MAX_AUDIO_BYTES;
inline constexpr std::size_t MAX_REPLY_BYTES = 8 * 1024 * 1024;
// File bytes per records_put frame and per records_fetch reply; well inside both frame limits,
// base64 included.
inline constexpr std::size_t RECORD_CHUNK_BYTES = 1024 * 1024;

// WebSocket close codes the host uses (4000-4999 is the application range).
inline constexpr int CLOSE_UNAUTHORIZED = 4401;
inline constexpr int CLOSE_BAD_REQUEST = 4400;
inline constexpr int CLOSE_TIMEOUT = 4408;
inline constexpr int CLOSE_ENGINE_CHANGED = 4409;
inline constexpr int CLOSE_BUSY = 4429;
// The host rejected an audio request before starting a decode.
inline constexpr std::string_view ENGINE_RELOADING = "engine_reloading";

// Longest name one computer keeps for another, given or chosen, in code points.
inline constexpr std::size_t MAX_NAME = 60;
// Bounds on a transcribe header's "phrases": entries, and code points per entry.
inline constexpr std::size_t MAX_HINT_PHRASES = 50;
inline constexpr std::size_t MAX_HINT_CHARS = 64;

// Approval pairing: bytes in each side's nonce, digits in the number both screens show.
inline constexpr std::size_t PAIRING_NONCE_BYTES = 32;
inline constexpr int SAS_DIGITS = 6;
// Hash domains of the commitment and of the short authentication string.
inline constexpr std::string_view COMMIT_DOMAIN = "openwhisper-pair-commit-v1";
inline constexpr std::string_view SAS_DOMAIN = "openwhisper-pair-sas-v1";

// UDP discovery: a query is QUERY_MAGIC and a nonce, zero-padded to QUERY_BYTES; an answer is
// REPLY_MAGIC and compact JSON, never longer than a query.
inline constexpr std::string_view QUERY_MAGIC = "OWHISPER?1";
inline constexpr std::string_view REPLY_MAGIC = "OWHISPER!1";
inline constexpr std::size_t DISCOVERY_NONCE_BYTES = 16;
inline constexpr std::size_t QUERY_BYTES = 512;
// Receive buffer for one datagram, larger than anything valid.
inline constexpr std::size_t DATAGRAM_MAX_BYTES = 2048;

// The host's TLS identity, in the data root (FioriVoce's own; never OpenWhisper's).
inline constexpr std::string_view CERT_FILENAME = "remote_engine_host_cert.pem";
inline constexpr std::string_view KEY_FILENAME = "remote_engine_host_key.pem";
// The client's device token is the credential <identity().env_prefix>REMOTE_ENGINE_TOKEN
// (OpenWhisper: OPENWHISPER_REMOTE_ENGINE_TOKEN), scoped per data root (pairing_settings.h).
inline constexpr std::string_view TOKEN_CREDENTIAL_STEM = "REMOTE_ENGINE_TOKEN";

// How a computer paired: a code from the host's screen, the owner's own Tailscale account, or
// the host's owner allowing a request on their screen.
inline constexpr std::array<std::string_view, 3> PAIRING_ROUTES = {"code", "tailscale", "approval"};
// Where a paired client keeps its records.
inline constexpr std::array<std::string_view, 3> RECORD_LOCATIONS = {"local", "host", "both"};

} // namespace fv::remote
