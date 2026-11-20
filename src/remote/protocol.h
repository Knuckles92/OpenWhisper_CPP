#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "pyport/errors.h"
#include "remote/wire.h"

// Wire format for the remote engine (services/remote_asr/protocol.py).
//
// One TLS WebSocket per client. The first message is a JSON text frame, either `hello` (a paired
// device's token) or `pair` (a pairing code from the host's screen). After the host answers
// `ready`, every request is a single binary frame and every reply a JSON text frame. The
// operations are those the local speech worker takes: `transcribe`, `stream` and
// `cancel_stream`, plus `describe` and `select_model`. `ready` lists the models the host can
// switch to as `models`, and `select_model` (`family`, `model`) switches it and answers once the
// model has loaded; the client then reconnects. Hosts from before model switching send no
// `models` and answer `select_model` with an unknown-operation error. A successful reply carries
// `host_ms`, the time the host spent on the request from receiving it, waiting for a work slot
// included, which older hosts leave out. Some older hosts started the clock after that wait, so
// their replies count it as network time.
//
// While its engine loads, a host can advertise it as unavailable in `ready` and return
// `engine_reloading` to audio requests it refused before decoding.
//
// Clients and hosts advertise `recording_activity` in hello and ready capabilities. A paired
// client reserves its recording with `recording_begin` before capture and releases it with
// `recording_end` after saving or canceling. Both send a `recording_id` and no audio. The
// reservation belongs to the speech connection, and a reconnect can reassert it with the same ID.
//
// Opt-in model management is advertised only to authenticated clients as
// `ready.capabilities.model_management` (missing means unsupported). `model_catalog` returns
// bundled models, cache/runtime readiness, the active engine and the last remote download's
// progress. `download_model` takes `family` and `model` from that catalog and acknowledges a
// host-owned background download; poll `model_catalog` for completion. Neither operation changes
// download policy or selects a model. Authorization is checked on every request. Accepted
// downloads continue after disconnect or permission changes. These additive operations keep
// protocol version 1.
//
// Hosts with `capabilities.runtime_installation` also accept `install_runtime` with a bundled
// `family`, `model` and explicit `device` (cpu/cuda; an Apple Silicon host uses auto for the
// Apple GPU or cpu). The host resolves a platform-compatible, pinned component and installs it in
// the background using its shared component coordinator. Clients cannot supply URLs, paths or
// commands. `model_catalog` includes per-device dependency readiness and reasons, download sizes,
// plus `installation` progress, failure and restart status. The same host model-management
// opt-in is rechecked for every install request. Accepted installations survive disconnect and
// permission changes. On these hosts, `select_model` may include `device` to load a cached model
// with a ready runtime in one change; this extension also requires model-management permission.
//
// Runtime controls are advertised to paired clients as `ready.capabilities.engine_controls`.
// `ready.runtime` describes the host's saved device, supported devices and precisions, optional
// language choices, and GPU name/memory; `ready.engine` describes what is actually running.
// `configure_runtime` takes the current `family` and `model` plus a bounded `settings` object
// (device, compute_type, or language). It follows the same paired-client authorization as model
// selection, rejects stale engine choices, and answers after the host reloads. The client
// reconnects to authoritative state, including CPU fallback. Older hosts have no editable runtime
// controls.
//
// Record storage is advertised as `ready.capabilities.records`: true when the host keeps records
// (dictation history, meetings) for paired computers, false when its owner hasn't turned that
// on, and missing on older hosts. `ready.records` then counts what this device has stored there.
// Every `records_*` request is scoped to the authenticated device, which can only see, fetch or
// delete its own records, and the permission is rechecked per request. A record is a set of
// files, one of them `record.json`; uploads are resumable and verified:
//
// * `records_stat` (`kind`, `record_id`) lists the files an earlier copy left on the host, so an
//   edited meeting is sent without its audio again (record.json then says `update`).
// * `records_begin` (`kind`, `record_id`, `files`, `bytes`) opens or resumes an upload and
//   answers with what the host already has of each file.
// * `records_put` (`record_id`, `name`, `size`, `sha256`, `offset`) carries up to
//   `RECORD_CHUNK_BYTES` of one file as the frame's payload; offsets are append-only and a
//   finished file is checked against its hash.
// * `records_commit` (`record_id`) imports the complete record into the host's own history or
//   meetings, badged with the device it came from.
// * `records_list` (`kind`, `query`, `limit`), `records_open` and `records_fetch` (`name`,
//   `offset`) read records back, the latter as base64 in the JSON reply; `records_delete`
//   removes one, `records_clear` every one of a kind, and `records_abort` drops an unfinished
//   upload.
//
// Vocabulary hints are advertised as `ready.capabilities.recognition_hints`: true when the host's
// engine uses them (Whisper's hotwords, or word boosting where it is verified), false or missing
// otherwise. A client may then add `phrases` to a `transcribe` header: at most
// `MAX_HINT_PHRASES` strings of at most `MAX_HINT_CHARS` characters, the client's own dictionary
// words with its starred ones first. The host drops entries that break those limits, ignores the
// field when its engine takes no hints, and never puts phrases in its engine identity, so they
// never disconnect anyone. Older hosts ignore the unknown field. Text read from other apps is
// never sent.
//
// MCP control is advertised as `ready.capabilities.mcp_control`: true when the host's owner
// allows paired computers to manage its MCP server (off by default), false when they haven't,
// and missing on older hosts. The permission is rechecked on every request. `mcp_state` returns
// whether MCP is on, its status, port, Tailscale access, the connection URLs, the access token
// while it is running, and which agent permissions and preferences are granted. `mcp_configure`
// takes a bounded `settings` object (`enabled`, `port`, `tailscale`, `retitle_transcriptions`,
// `retitle_meetings`, `settings_access` and `writable`, a map of preference key to bool), applies
// it all or none of it, and answers with the new state. Port and Tailscale can change only while
// MCP is off, as on the host's own page.
//
// A request frame is a 4-byte big-endian header length, the JSON header, then a payload. For
// decoding operations the payload is the audio as 16 kHz mono signed 16-bit little-endian PCM.
// Dictation audio is recorded as 16-bit and resampled to 16-bit before it is decoded, so the
// conversion loses nothing and halves what float32 would send. For `records_put` it is file
// bytes.
//
// Direct history uses a separate paired connection: `hello` includes `purpose: "history"` and
// the client's `history_enabled` opt-in. A supporting host replies with
// `ready.capabilities.client_history: true`, then sends JSON `history_query` messages (id,
// operation, params). The client returns JSON `history_result` with that id and a public History
// API result or sanitized error code. This channel carries no audio, accepts only read
// operations, and the client rechecks its permission for every query. Older hosts omit the
// capability, so clients close the dedicated connection without sending history.
//
// Pairing by approval, for a host found on the network (`probe` answers `approval: true`): the
// first message is `pair_request` (`device_name`). The host answers `pair_commit` with the
// SHA-256 commitment to a random nonce, the client sends its own nonce in `pair_nonce`, and the
// host reveals its nonce in `pair_reveal`. Both sides then show `pairing_sas` of the two nonces
// and the certificate fingerprint, and the host's owner allows the request only if the numbers
// match; the host answers `paired` exactly as for a code, or an error (`pair_denied`,
// `pair_timeout`). The host committed to its nonce before seeing the client's, so a computer in
// the middle can't steer the two numbers to match, and the fingerprint is in the hash, so
// relaying the nonces unchanged gives two different numbers. The client may send `pair_cancel`
// while it waits. Older hosts close the connection.
//
// Every function here is pure and thread-safe.
namespace fv::remote {

// A frame that does not follow this protocol; what() is the text the host sends back as the
// reply's "error". A ValueError, as Python's ProtocolError is.
class ProtocolError : public py::ValueError {
public:
    explicit ProtocolError(const std::string& message) : py::ValueError("ProtocolError", message) {}
};

// A request frame taken apart: the header object and the payload bytes.
struct Frame {
    Json header;
    std::vector<std::byte> payload;
};

// json.dumps with ensure_ascii and insertion order: compact separators ("," ":"), which every
// frame the host sends and every request header and discovery answer uses; and the default ones
// (", " ": "), which every text frame the client sends uses.
std::string dumps_compact(const Json& value);
std::string dumps_default(const Json& value);

// Float samples in [-1, 1] as little-endian int16 PCM: rint(x * 32768.0) in float32, half to
// even, clipped to [-32768, 32767]. NaN, which numpy leaves undefined, gives 0.
std::vector<std::byte> encode_audio(std::span<const float> samples);
// int16 PCM back to float32 samples (int16 / 32768.0). ProtocolError "Audio payload has an odd
// byte count".
std::vector<float> decode_audio(std::span<const std::byte> data);

// A request frame: the header's length (4 bytes, big-endian), the compact JSON header, then the
// audio as PCM16 or `payload` as it is. ProtocolError "Request header is too large" past
// MAX_HEADER_BYTES.
std::vector<std::byte> pack_request(const Json& header);
std::vector<std::byte> pack_request(const Json& header, std::span<const float> audio);
std::vector<std::byte> pack_request(const Json& header, std::span<const std::byte> payload);

// The header and the payload of a frame, without reading the payload as audio. `binary` false is
// a text frame: "Requests must be binary frames". Otherwise ProtocolError "Request frame is
// truncated", "Request header length is invalid", "Request header is not JSON" (read as
// json.loads reads bytes: UTF-8, UTF-16 or UTF-32 by their BOM or zero bytes) or "Request header
// must be an object".
Frame unpack_frame(std::span<const std::byte> frame, bool binary = true);
// A decoding request's payload as audio: ProtocolError "Request audio is too long" past
// MAX_AUDIO_BYTES, then as decode_audio.
std::vector<float> payload_audio(std::span<const std::byte> payload);
// unpack_frame, then payload_audio of its payload.
std::pair<Json, std::vector<float>> unpack_request(std::span<const std::byte> frame,
                                                   bool binary = true);

// A request header's valid vocabulary hints: each string entry's whitespace runs collapsed to
// one space, kept when 1..MAX_HINT_CHARS code points, at most MAX_HINT_PHRASES of them; anything
// malformed is left out. A header that is not an object has none.
std::vector<std::string> header_phrases(const Json& header);

// SHA-256 of a DER certificate, as 64 uppercase hex digits.
std::string certificate_fingerprint(std::span<const std::byte> der);
// The first `groups` blocks of four of the fingerprint's alphanumerics (uppercased), joined by
// "-", for people to compare by eye. Five blocks are 80 bits: enough that a forged certificate
// cannot be made to match what the host's screen shows.
std::string short_fingerprint(std::string_view fingerprint, int groups = 5);
// What the host stores for a device token (lowercase hex SHA-256); the token itself never is.
std::string token_digest(std::string_view token);
// A computer's name with only printable characters (str.isprintable), stripped, cut to MAX_NAME
// code points; "" if none are left.
std::string clean_name(std::string_view name);
inline std::string clean_name(const std::string& name) {
    return clean_name(std::string_view(name));
}
inline std::string clean_name(const char* name) {
    return clean_name(std::string_view(name));
}
// The same over str(name or "") of a JSON value: null, false, 0 and "" are "".
std::string clean_name(const Json& name);

// What the host sends before it has seen the client's nonce: lowercase hex
// SHA-256(COMMIT_DOMAIN + nonce).
std::string pairing_commitment(std::span<const std::uint8_t> nonce);
// The six digits both screens show while a pairing waits for approval: SHA-256(SAS_DOMAIN + host
// nonce + client nonce + the fingerprint uppercased, each non-ASCII code point as "?"), its first
// 8 bytes big-endian modulo 10**6, zero-padded. A computer in the middle has its own certificate,
// so the client hashes a different fingerprint than the host does.
std::string pairing_sas(std::span<const std::uint8_t> host_nonce,
                        std::span<const std::uint8_t> client_nonce, std::string_view fingerprint);
// "123 456", easier to compare across a room; anything not six code points as it is.
std::string format_sas(std::string_view sas);

// Splits "host", "host:port" or "[v6]:port" into host and port; a scheme (http, https, ws, wss,
// any case) and a path are dropped, and an unbracketed IPv6 address is a bare host. Throws
// py::ValueError with the text the Settings page shows.
std::pair<std::string, int> parse_address(std::string_view text, int default_port = DEFAULT_PORT);
// "host:port", or "[host]:port" when the host has a colon.
std::string format_address(std::string_view host, int port);

// What a pairing gave this computer.
struct PairingResult {
    std::string token;
    std::string device_id;
    std::string host_name;
    std::string fingerprint;
    // The host's other addresses, tried when the paired one is out of reach.
    std::vector<std::string> alternates;
    std::string via = "code"; // one of PAIRING_ROUTES

    friend bool operator==(const PairingResult&, const PairingResult&) = default;
};

// What a host's `probe` answer says about it.
struct ProbeResult {
    std::string host_name;
    std::int64_t protocol = 0;
    Json engine = Json::object();
    bool tailscale_pairing = false;
    // The certificate the host presented, so finds by different routes merge.
    std::string fingerprint;
    // Takes pairing requests its owner allows on its own screen.
    bool approval = false;

    bool compatible() const { return protocol == PROTOCOL_VERSION; }
    friend bool operator==(const ProbeResult&, const ProbeResult&) = default;
};

} // namespace fv::remote
