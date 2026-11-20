#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/json.h"
#include "meeting_state/types.h"

// meeting/web/ws.py's wire rules, free of sockets: close codes and limits, the checks every
// inbound frame passes, the messages the hub sends, and the split of a store batch into the
// shared and host-only patches.
// Affinity: thread-safe (constants and functions).
namespace fv::meeting_web {

// Application close codes (4000-4999). 4400 and 4401 end the client's retries; 4409 and 4429
// make it reconnect.
inline constexpr std::uint16_t kWsCloseNameRequired = 4400;
inline constexpr std::uint16_t kWsCloseUnauthorized = 4401;
inline constexpr std::uint16_t kWsCloseResync = 4409;
inline constexpr std::uint16_t kWsCloseTooMany = 4429;
inline constexpr std::uint16_t kWsCloseNormal = 1000;
inline constexpr std::uint16_t kWsCloseServerError = 1011;

// Trailing transcript segments in a hello.
inline constexpr std::int64_t kHelloSegmentCount = 200;
// Trailing agent activity ticks in a host's hello.
inline constexpr std::size_t kHelloActivityCount = 50;
// Largest inbound text frame accepted, in characters (code points), checked before parsing.
inline constexpr std::size_t kMaxMessageChars = 64 * 1024;
// Dashboard sockets one hub serves at once.
inline constexpr std::size_t kMaxConnections = 32;
// Engine calls one socket may have in flight.
inline constexpr int kMaxInflightActions = 4;
// Broadcasts buffered for a socket between its registration and its hello.
inline constexpr std::size_t kMaxPendingMessages = 512;
// Budget of one broadcast send to one socket, and the consecutive misses that drop it.
inline constexpr std::chrono::milliseconds kBroadcastSendTimeout{5000};
inline constexpr int kMaxSendTimeouts = 3;
// Upper bound of a client-supplied event seq (undo).
inline constexpr std::int64_t kMaxEventSeq = std::numeric_limits<std::int64_t>::max();
// Rotation waits this long before closing sockets, so the REST reply with the new links
// reaches the client first.
inline constexpr std::chrono::milliseconds kRotationCloseDelay{250};

// Entities guests never receive: a tailored report may draw on past meetings and the knowledge
// folder, which a guest of this meeting was never party to.
inline constexpr std::array<std::string_view, 1> kHostOnlyEntities = {"custom_report"};

// One message to fan out, and whether only host sockets receive it.
struct Broadcast {
    Json message;
    bool host_only = false;
};

// An error frame the receive loop answers with: {"type": "error", "code", "message"}.
struct FrameError {
    std::string code;
    std::string message;
};

// _clean_guest_id: the stripped key when it matches ^[A-Za-z0-9_-]{1,64}$, else nullopt.
std::optional<std::string> clean_guest_id(std::optional<std::string_view> value);

// _coerce_seq: int(value) (a float truncated, a numeric string parsed, a bool 0 or 1) when it
// lies in 0..kMaxEventSeq; nullopt for anything int() refuses or out of range.
std::optional<std::int64_t> coerce_seq(const Json& value);

// Whether a result's effect is an object whose "entity" is host-only.
bool is_host_only_effect(const meeting_state::OpResult& result);

// _on_store_batch: {"type": "patch", "seq", "results": [{op, target_id, effect, seq}]} without
// the host-only results for everyone (when any remain), then, when any result is host-only, a
// host-only patch carrying every result.
std::vector<Broadcast> split_store_batch(std::int64_t seq,
                                         const std::vector<meeting_state::OpResult>& applied);

// The receive loop's checks of one text frame: message_too_large over kMaxMessageChars,
// bad_json for text json.loads refuses (too deeply nested included), bad_message for JSON that
// is not an object; else the parsed object.
std::variant<Json, FrameError> check_text_frame(std::string_view text);
FrameError binary_frame_error();
// unknown_type: "Unknown message type: <repr of the type>".
FrameError unknown_type_error(const Json& type);
FrameError internal_error();
Json error_message(const FrameError& error);

Json pong_message();
// {"ok", "reason", "target_id", "seq", "effect"} of one result.
Json op_result_wire(const meeting_state::OpResult& result);
// A one-entry results list for a local rejection.
Json rejection(std::string_view reason);
// {"type": "action_result", "client_action_id" (as sent, null when absent), "results"}.
Json action_result(const Json& client_action_id, Json results);
// {"type": "presence", "event": "joined" | "left", "participant"}.
Json presence(std::string_view event, const Json& participant);

// json.dumps(message, ensure_ascii=False): the text every frame carries.
std::string encode(const Json& message);

} // namespace fv::meeting_web
