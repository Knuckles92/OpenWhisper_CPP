#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/cancel.h"
#include "http/client.h"

// Resumable, SHA-256-verified downloads (services/verified_download.py): the one download path
// for application updates, optional components and speech models. A transfer lands in
// `<destination>.part`, resumes with a checked Range request, must match the pinned size and
// digest, and is flushed and renamed into place only once it does.
namespace fv::download {

// Each request's connect and idle (between bytes) timeout, as urllib's socket timeout.
inline constexpr std::chrono::seconds kNetworkTimeout{30};

// Progress phases, reported as (phase, done bytes, total bytes).
inline constexpr std::string_view kDownloading = "downloading";
inline constexpr std::string_view kVerifying = "verifying";
using ProgressCallback =
    std::function<void(std::string_view phase, std::int64_t done, std::int64_t total)>;

// The user-facing texts, by key; DownloadOptions::messages replaces any of them. "incomplete"
// takes {done} and {total} (sizes as format_size_bytes writes them).
struct Messages {
    std::string invalid_size = "The download has an invalid size.";
    std::string unsafe_partial = "The download could not be created safely.";
    std::string partial_not_regular = "The partial download is not a regular file.";
    std::string partial_unreadable = "The partial download could not be read.";
    std::string canceled = "The download was canceled.";
    std::string bad_resume = "The download server returned an invalid resume response.";
    std::string bad_length = "The download server returned an invalid download size.";
    std::string unexpected_length = "The download server returned an unexpected download size.";
    std::string too_much_data = "The download server returned more data than expected.";
    std::string incomplete = "The download did not complete ({done} of {total}).";
    std::string integrity =
        "The download failed its integrity check and was discarded. Please try again.";
    std::string finalize = "The verified download could not be finalized. Please retry.";
};

// A network or disk failure as Python's exception types tell them apart, which is what
// describe_network_error words.
struct NetworkFailure {
    enum class Kind {
        Certificate, // ssl.SSLCertVerificationError, bare or inside a URLError
        HttpStatus,  // urllib.error.HTTPError: `code` and `reason`
        Url,         // urllib.error.URLError: `reason` is str() of its reason
        Other,       // any other OSError: `reason` is str() of it, possibly empty
    };
    Kind kind = Kind::Other;
    int code = 0;
    std::string reason;
};

// The failure an http::Error stands for: a certificate failure; before the request was sent
// (connect, handshake, send), a URLError with the error's text; once it was sent (waiting for
// the head, reading the body), the bare OSError.
NetworkFailure network_failure(const http::Error&);

struct NetworkErrorText {
    std::string server = "download server"; // as in "Could not reach the <server>"
    // Hosts to ask IT to allow when certificate verification fails; empty names the server.
    std::vector<std::string> hosts;
    std::map<int, std::string> http_messages; // replacement copy for specific HTTP statuses
};

// Translates a failure into something a user can act on, word for word as
// describe_network_error does.
std::string describe_network_error(const NetworkFailure&, const NetworkErrorText& = {});

// A download failed. The message is written for the user. `failure()` is the network or disk
// failure it came from, when there was one (Python's __cause__).
class DownloadError : public std::runtime_error {
public:
    explicit DownloadError(const std::string& message,
                           std::optional<NetworkFailure> failure = std::nullopt)
        : std::runtime_error(message), failure_(std::move(failure)) {}
    const std::optional<NetworkFailure>& failure() const { return failure_; }

private:
    std::optional<NetworkFailure> failure_;
};

// The transfer stopped short; its prefix stays on disk for a retry to resume.
class DownloadInterrupted : public DownloadError {
public:
    using DownloadError::DownloadError;
};

// The cancel token was cancelled.
class DownloadCanceled : public DownloadError {
public:
    using DownloadError::DownloadError;
};

// What a caller's own sender made of one request: the response (its head and body went to the
// sink, which decides whether a non-2xx head is refused), or why it failed: a transport error,
// or a failure the sender raised itself (a refused redirect), worded by describe_error like any
// other network failure.
using SendResult = std::expected<http::Response, std::variant<http::Error, NetworkFailure>>;
using Sender = std::function<SendResult(const http::Request&, const CancelToken&, http::BodySink&)>;

struct DownloadOptions {
    std::optional<std::int64_t> max_bytes; // largest size_bytes accepted; none: no cap
    Messages messages;
    // Turns a network or disk failure into the user's message; empty: describe_network_error
    // with the default server name and no hosts.
    std::function<std::string(const NetworkFailure&)> describe_error;
    std::string user_agent; // empty: http::user_agent()
    // Adjusts each request before it is sent (a caller's redirect rules, pinned address, ...).
    std::function<void(http::Request&)> prepare;
    // Sends each request in place of client.send (a caller that follows redirects by hand, as
    // the updater's opener does). Empty: client.send with redirects followed.
    Sender send;
    // Asked with the status of a refused (non-2xx) answer until it first returns a URL; that URL
    // then replaces the download URL for the rest of the transfer, Range resume and the 416
    // restart included. At most one switch per transfer; the cancel token is checked before the
    // new route is tried. Empty: no other route.
    std::function<std::optional<std::string>(int status)> switch_route;
};

// Removes `path` when it is a regular file (not following a link); errors are ignored.
void discard_regular_file(const std::filesystem::path& path);

// Fetches `url` to `destination` with bounded resume, exact-size and SHA-256 checks.
//
// size_bytes: the exact expected size; must be positive (and at most options.max_bytes).
// sha256_hex: the expected digest, hex in either case. progress: (phase, done, size_bytes) for
// this file. cancel: checked between pieces and before every commit step.
// keep_partial_on_cancel: leave a cancelled transfer on disk so the next attempt resumes it.
//
// Throws DownloadCanceled when cancelled, DownloadInterrupted when the server sent too little
// (the prefix is kept), DownloadError for anything else: a partial that cannot be trusted is
// discarded, one cut short by a network error is kept for resume.
void download_verified(http::Client& client, std::string_view url,
                       const std::filesystem::path& destination, std::int64_t size_bytes,
                       std::string_view sha256_hex, const ProgressCallback& progress,
                       const CancelToken& cancel, bool keep_partial_on_cancel = false,
                       const DownloadOptions& options = {});

} // namespace fv::download
