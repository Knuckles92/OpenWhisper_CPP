#include "llm/detail/keep_alive.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <tuple>

#include "http/user_agent.h"
#include "llm/detail/url.h"
#include "pyport/json.h"

namespace fv::llm::detail {

namespace {

using Seconds = std::chrono::duration<double>;

std::chrono::milliseconds to_ms(double seconds) {
    return std::chrono::milliseconds(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(seconds * 1000.0 + 0.5)));
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), ascii_lower);
    return out;
}

// {**extra, name: value}: a later key of the same spelling replaces the earlier value in place,
// as a Python dict does.
void put_header(http::Headers& headers, std::string_view name, std::string value) {
    for (auto& [key, existing] : headers) {
        if (key == name) {
            existing = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::string(name), std::move(value));
}

// urlsplit().port with the scheme's default; a port that does not parse keeps the default.
int port_of(const UrlParts& parts) {
    const int fallback = parts.scheme == "https" ? 443 : 80;
    std::string_view netloc = parts.netloc;
    if (const auto at = netloc.rfind('@'); at != std::string_view::npos) {
        netloc.remove_prefix(at + 1);
    }
    std::size_t colon = std::string_view::npos;
    if (netloc.starts_with('[')) {
        const auto close = netloc.find(']');
        if (close != std::string_view::npos && close + 1 < netloc.size() &&
            netloc[close + 1] == ':') {
            colon = close + 1;
        }
    } else {
        colon = netloc.rfind(':');
    }
    if (colon == std::string_view::npos || colon + 1 >= netloc.size()) {
        return fallback;
    }
    const auto digits = netloc.substr(colon + 1);
    int port = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (error != std::errc() || end != digits.data() + digits.size() || port <= 0 || port > 65535) {
        return fallback;
    }
    return port;
}

// response.will_close: the server said it closes the connection after this answer.
bool will_close(const http::Response& response) {
    const auto connection = http::find_header(response.headers, "Connection");
    return connection && lowered(*connection).find("close") != std::string::npos;
}

// ConnectionError, ssl.SSLEOFError and TimeoutError: the failures that leave nothing answered,
// so a judgment (which has no side effects) may go out again.
// ssl.SSLEOFError is a TLS failure whose text says the peer ended the stream ("EOF occurred in
// violation of protocol", OpenSSL's "unexpected eof while reading").
bool resendable(const http::Error& error) {
    if (error.kind == http::ErrorKind::Tls) {
        return !error.certificate && lowered(error.message).find("eof") != std::string::npos;
    }
    return error.kind == http::ErrorKind::Timeout || error.kind == http::ErrorKind::Connect;
}

http::Request post_request(std::string_view endpoint, const http::Headers& headers,
                           const std::string& body, double wait_s) {
    http::Request request;
    request.method = "POST";
    request.url = std::string(endpoint);
    request.headers = headers;
    request.body = body;
    request.connect_timeout = to_ms(wait_s);
    request.idle_timeout = to_ms(wait_s);
    request.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(Seconds(wait_s));
    return request;
}

PostReply reply_of(http::Response& response) {
    if (response.status < 200 || response.status >= 300) {
        // The body may echo the request; keep only the status.
        return {response.status, {}};
    }
    return {response.status, std::move(response.body)};
}

// _urlopen_post: one request that applies the system proxy itself and closes its connection.
std::expected<PostReply, http::Error> urlopen_post(http::Client& client, std::string_view endpoint,
                                                   http::Headers headers, const std::string& body,
                                                   double timeout_s, const CancelToken& cancel) {
    headers.emplace_back("Connection", "close");
    auto response = client.send(post_request(endpoint, headers, body, timeout_s), cancel);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    return reply_of(*response);
}

} // namespace

double reused_wait_s(double timeout_s) {
    return std::min(timeout_s, std::max(1.0, timeout_s / 2));
}

KeepAlivePool::KeepAlivePool(ConnectionFactory factory, std::size_t max_idle, double idle_s,
                             const Clock* clock)
    : factory_(std::move(factory)), max_idle_(max_idle), idle_s_(idle_s),
      clock_(clock != nullptr ? *clock : system_clock()) {}

double KeepAlivePool::now() const {
    return Seconds(clock_.steady().time_since_epoch()).count();
}

KeepAlivePool::Lease KeepAlivePool::acquire(const Origin& origin) {
    while (true) {
        std::shared_ptr<http::Client> connection;
        double released = 0.0;
        {
            std::lock_guard lock(mutex_);
            const auto found = idle_.find(origin);
            if (found == idle_.end() || found->second.empty()) {
                break;
            }
            std::tie(connection, released) = std::move(found->second.back());
            found->second.pop_back();
        }
        if (now() - released < idle_s_) {
            return {std::move(connection), true};
        }
        // Past the window: dropping the last reference closes it.
    }
    auto connection = factory_();
    {
        std::lock_guard lock(mutex_);
        ++opened_;
    }
    return {std::move(connection), false};
}

void KeepAlivePool::release(const Origin& origin, std::shared_ptr<http::Client> connection) {
    const double released = now();
    std::lock_guard lock(mutex_);
    auto& idle = idle_[origin];
    if (idle.size() < max_idle_) {
        idle.emplace_back(std::move(connection), released);
    }
}

void KeepAlivePool::discard(const Origin& origin) {
    std::vector<std::pair<std::shared_ptr<http::Client>, double>> closing;
    {
        std::lock_guard lock(mutex_);
        ++discards_;
        if (const auto found = idle_.find(origin); found != idle_.end()) {
            closing = std::move(found->second);
            idle_.erase(found);
        }
    }
}

std::size_t KeepAlivePool::idle() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [origin, connections] : idle_) {
        count += connections.size();
    }
    return count;
}

std::size_t KeepAlivePool::opened() const {
    std::lock_guard lock(mutex_);
    return opened_;
}

std::size_t KeepAlivePool::discards() const {
    std::lock_guard lock(mutex_);
    return discards_;
}

bool proxied(std::string_view url, const ProxyLookup& lookup) {
    try {
        return lookup(url).has_value();
    } catch (const std::exception&) {
        return true;
    }
}

std::expected<PostReply, http::Error>
http_post(KeepAlivePool& pool, http::Client& one_shot, const ProxyLookup& lookup,
          const Json& payload, double timeout_s, std::string_view api_key,
          std::string_view endpoint, const http::Headers& extra, const CancelToken& cancel) {
    const std::string body = py::dumps(payload);
    http::Headers headers = extra;
    put_header(headers, "Authorization", "Bearer " + std::string(api_key));
    put_header(headers, "Content-Type", "application/json");
    put_header(headers, "Accept", "application/json");
    put_header(headers, "User-Agent", http::user_agent());

    std::optional<UrlParts> parts;
    try {
        parts = urlsplit(endpoint);
    } catch (const std::exception&) {
        parts.reset();
    }
    const auto host = parts ? parts->hostname() : std::nullopt;
    if (!parts || (parts->scheme != "http" && parts->scheme != "https") || !host ||
        proxied(endpoint, lookup)) {
        return urlopen_post(one_shot, endpoint, std::move(headers), body, timeout_s, cancel);
    }
    const Origin origin{parts->scheme, *host, port_of(*parts)};
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(Seconds(timeout_s));
    double budget = timeout_s;
    bool retried = false;
    while (true) {
        auto lease = pool.acquire(origin);
        const double wait = lease.reused ? reused_wait_s(budget) : budget;
        http::Request request = post_request(endpoint, headers, body, wait);
        // Only an unproxied URL gets here.
        request.bypass_proxy = true;
        auto response = lease.connection->send(request, cancel);
        if (!response) {
            // The lease is dropped, which closes its connection.
            // sends > 1: curl itself already sent the request again on a fresh connection,
            // which is the one resend _http_post allows; another would make three.
            if (!resendable(response.error()) || !lease.reused || retried ||
                response.error().sends > 1) {
                return std::unexpected(std::move(response.error()));
            }
            // The server dropped an idle connection as this request went out, or a NAT forgot
            // it and it went silent, so nothing was answered. Idle siblings probably died with
            // it.
            pool.discard(origin);
            budget = Seconds(deadline - std::chrono::steady_clock::now()).count();
            if (budget < kFreshAttemptMinS) {
                return std::unexpected(std::move(response.error()));
            }
            retried = true;
            continue;
        }
        if (!will_close(*response)) {
            pool.release(origin, std::move(lease.connection));
        }
        return reply_of(*response);
    }
}

std::string python_exception_name(const http::Error& error) {
    switch (error.kind) {
    case http::ErrorKind::Timeout:
        return "TimeoutError";
    case http::ErrorKind::Dns:
        return "gaierror";
    case http::ErrorKind::Connect:
        if (error.message.find("closed connection without response") != std::string::npos) {
            return "RemoteDisconnected";
        }
        if (error.message.find("10061") != std::string::npos ||
            error.message.find("[Errno 111]") != std::string::npos) {
            return "ConnectionRefusedError";
        }
        if (error.message.find("10054") != std::string::npos ||
            error.message.find("[Errno 104]") != std::string::npos) {
            return "ConnectionResetError";
        }
        if (error.message.find("10053") != std::string::npos ||
            error.message.find("[Errno 103]") != std::string::npos) {
            return "ConnectionAbortedError";
        }
        return "OSError";
    case http::ErrorKind::Tls:
        return error.certificate ? "SSLCertVerificationError" : "SSLError";
    case http::ErrorKind::Cancelled:
        return "CancelledError";
    case http::ErrorKind::TooLarge:
    case http::ErrorKind::Protocol:
        return error.truncated ? "IncompleteRead" : "HTTPException";
    }
    return "OSError";
}

} // namespace fv::llm::detail
