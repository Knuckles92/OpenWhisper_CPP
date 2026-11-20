#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/cancel.h"

namespace fv::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

// One part of a multipart/form-data body: inline bytes or a file streamed from disk.
struct MultipartPart {
    std::string name;
    std::string filename; // empty: a plain form field
    std::string content_type;
    std::variant<std::string, std::filesystem::path> data;
};

struct Multipart {
    std::vector<MultipartPart> parts;
};

// No body, bytes, multipart with file parts, or a whole file streamed from disk.
using Body = std::variant<std::monostate, std::string, Multipart, std::filesystem::path>;

struct Request {
    std::string method = "GET", url;
    Headers headers;
    Body body; // bytes, multipart with file parts, or file
    std::chrono::milliseconds connect_timeout{10'000}, idle_timeout{120'000};
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool follow_redirects = false;             // callers re-validate each hop
    std::optional<std::string> pinned_address; // CURLOPT_RESOLVE: SSRF guard keeps SNI
    bool bypass_proxy = false;
    std::optional<std::uint64_t> range_from;
    std::size_t max_body_bytes = 64u << 20;
    // false keeps the INFO "HTTP Request:" line (logger "httpx") out of the log for this request,
    // as OpenWhisper's _QuietRequests does for hook deliveries.
    bool log_request = true;
};

struct Response {
    int status = 0;
    Headers headers;  // of the final response, in arrival order, names as sent
    std::string body; // empty when the body went to a BodySink
    std::string effective_url;
    std::string reason; // the status line's reason phrase ("Not Found"), possibly empty
};

// Receives a response body as it arrives instead of Response::body, whatever the status.
// Returning false from either method ends the transfer at once; send() then returns the Response
// head (status, reason, headers, effective_url), not an Error. The sink knows why it stopped.
class BodySink {
public:
    virtual ~BodySink() = default;
    // Once, with the final response's head (after any redirects followed), before the first
    // write, and also when the body is empty.
    virtual bool on_head(const Response& /*head*/) { return true; }
    virtual bool write(std::span<const char>) = 0; // false aborts (SSE, downloads)
};

enum class ErrorKind { Timeout, Dns, Connect, Tls, Cancelled, TooLarge, Protocol };

struct Error {
    ErrorKind kind;
    std::string message;      // the reason as Python's socket and ssl errors word it, when known
    bool certificate = false; // Tls: the server's certificate failed verification
    bool after_head = false;  // the failure came while the body was being read
    bool truncated = false;   // the connection closed before the declared body length arrived
    // The request had been sent (to the last hop of any redirects followed); the failure came
    // while waiting for or reading the answer. urllib wraps only failures before that point in a
    // URLError (connect, TLS handshake, sending); later ones reach the caller as the bare
    // OSError, such as a server that never answers ("timed out").
    bool after_request = false;
    // How many times the request went out on a connection (curl's prerequest callback): curl
    // sends it again by itself, on a fresh connection, when a reused one died before anything
    // was answered, so 2 says that resend already happened. 0 when unknown (a fake client).
    int sends = 0;
};

// The first header of that name, compared without regard to case, as email.message.get finds it.
std::optional<std::string> find_header(const Headers&, std::string_view name);

// Blocking; never on the GUI thread. Cancel aborts through curl's transfer callback.
// Affinity: thread-safe.
class Client {
public:
    virtual ~Client() = default;
    virtual std::expected<Response, Error> send(const Request&, const CancelToken&,
                                                BodySink* stream = nullptr) = 0;
};

// Where a request goes: the proxy URL for it, or nullopt to connect directly.
using ProxyResolver = std::function<std::optional<std::string>(std::string_view url)>;

struct CurlClientOptions {
    // Empty: the system settings as urllib reads them (http/proxy.h), read again per request.
    ProxyResolver proxy;
};

// Request::max_body_bytes limits Response::body only; a BodySink decides for itself. A multipart
// body streams its file parts from disk (http/multipart.h); a part whose file cannot be opened
// fails with ErrorKind::Protocol before connecting.
std::shared_ptr<Client> make_curl_client(); // one share handle, Schannel, OS trust store
std::shared_ptr<Client> make_curl_client(CurlClientOptions);

} // namespace fv::http
