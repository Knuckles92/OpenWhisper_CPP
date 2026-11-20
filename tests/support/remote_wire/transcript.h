#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// Recorded OpenWhisper remote-engine exchanges (tools/parity/remote_wire.py writes them to
// tests/fixtures/transcripts/remote_<scenario>.json) and the comparison of live frames with them.
// Volatile values stand in the recorded frames as placeholders, JSON strings "<kind#n>"; the
// kinds and their shapes are remote_wire.py's SHAPES. A placeholder of a binding kind (token,
// device_id, nonce, commit, fingerprint, uuid, conn_id) names one value everywhere it appears;
// host_ms and iso_time placeholders are each their own.
namespace fv::testing::remote_wire {

using Json = fv::Json;

// The product name the recorder marks in copy ("as": "product"); recorded frames keep it.
inline constexpr std::string_view kRecordedProduct = "OpenWhisper";

// One WebSocket frame, or one side of a refused upgrade.
struct WireFrame {
    std::string dir;  // "c2s" or "s2c"; an actual frame may leave it empty
    std::string kind; // "text", "binary", "close" or "http"
    // text: the frame; binary: the JSON header when has_header; http c2s: the request path.
    std::string text;
    // binary: the bytes after the header (every byte when there is no header). Empty in a
    // recorded frame whose payload is known only by payload_sha256 and payload_len.
    std::vector<std::byte> payload;
    int code = 0; // close
    std::string reason;
    int status = 0; // http s2c
    std::string body;

    std::string step;  // recorded frames: the step during which it was sent
    int conn = -1;     // recorded frames: the connection, numbered in the order they opened
    bool echo = false; // close: the side answered the other's close (the library, not the app)
    // binary: the frame split as a request frame does (4-byte big-endian length, the header as
    // UTF-8, the payload); false keeps every byte in payload.
    bool has_header = false;
    std::string payload_sha256; // lowercase hex, for a recorded payload over 64 KiB
    std::size_t payload_len = 0;
    int payload_fill = -1; // that payload is this byte repeated, or -1
};

struct Transcript {
    std::string scenario;
    Json setup; // the host's fixed answers, the recorder's patches, each step's outcomes
    std::vector<WireFrame> frames;
    Json normalize; // [{"path": "<frame index><JSON pointer>", "as": kind[, "of": nonce]}]
    // A commit placeholder and the nonce placeholder it is the commitment of.
    std::map<std::string, std::string> commit_of;
};

// tests/fixtures/transcripts/remote_<scenario>.json ("probe" or "remote_probe"). Throws
// std::runtime_error naming the file and the fault when it is missing or malformed.
Transcript load_transcript(std::string_view scenario);
Transcript load_transcript_file(const std::filesystem::path&);
Transcript parse_transcript(const Json&);

// The frames of some steps or connections, in order, with the normalize list re-indexed.
Transcript only_steps(const Transcript&, std::span<const std::string> steps);
Transcript only_connections(const Transcript&, std::span<const int> connections);
// The connections of a transcript in order, and the indexes of one connection's frames.
std::vector<int> connections(const Transcript&);
std::vector<std::size_t> frames_of(const Transcript&, int connection);
// setup.steps[i] of that name, or null.
Json step_setup(const Transcript&, std::string_view step);

// Actual frames, as a test builds them from what it received or sent.
WireFrame text_frame(std::string dir, std::string text);
// Split as the recorder splits binary frames.
WireFrame binary_frame(std::string dir, std::span<const std::byte> bytes);
WireFrame close_frame(std::string dir, int code, std::string reason);
WireFrame http_frame(std::string dir, int status, std::string body);

// Compares an actual frame with recorded frame `index`: bytes equal after replacing normalized
// values, which must have the recorded shape; values named alike must agree across frames (a
// token in `paired` and the next `hello`), and a commit must be the SHA-256 commitment of the
// nonce the transcript names. `bindings` maps placeholders to the values seen so far (strings);
// a test may bind some first (its own fingerprint). Strings that held OpenWhisper's product
// name must hold `product` instead (identity().product by default: a OpenWhisper_CPP host's copy).
// An actual http frame with an empty body is compared by status only. Returns a readable
// difference, empty when equal.
std::string compare(const Transcript&, std::size_t index, const WireFrame& actual, Json& bindings);
std::string compare(const Transcript&, std::size_t index, const WireFrame& actual, Json& bindings,
                    std::string_view product);

// Recorded frame `index` as bytes to send: each placeholder becomes its bound value, or a new
// value of its shape, bound from then on (a commit is computed from its nonce). Product names
// stay as recorded (the peer plays OpenWhisper). Throws std::runtime_error for a payload known
// only by its digest.
WireFrame render(const Transcript&, std::size_t index, Json& bindings);
// A binary frame's bytes: length, header and payload, or the payload alone without a header.
std::vector<std::byte> wire_bytes(const WireFrame&);

// Recorded copy as a host named `product` writes it: each "OpenWhisper" becomes `product`, and
// an "an"/"An" right before it becomes the article `product` takes ("an OpenWhisper update" is
// "a OpenWhisper_CPP update"). compare applies it to the strings the transcript lists as product.
std::string with_product(std::string_view text, std::string_view product);

// The placeholders of a recorded frame's text or header, in order ("<token#1>", ...).
std::vector<std::string> placeholders_in(std::string_view text);

// Whether `text` (a JSON string's content, or a number's digits) has the shape of `kind`, and a
// new random value of that shape.
bool has_shape(std::string_view kind, std::string_view text);
std::string fresh_value(std::string_view kind);
// The commitment the host sends before revealing `nonce_hex`: SHA-256 of the commit domain and
// the nonce's bytes, lowercase hex.
std::string commitment_of(std::string_view nonce_hex);

} // namespace fv::testing::remote_wire
