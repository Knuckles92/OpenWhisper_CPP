#include "automation/hook_http.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <utility>

#include "automation/detail/http_date.h"
#include "automation/detail/pyvalue.h"
#include "automation/hooks_model.h"
#include "base/log.h"
#include "http/user_agent.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::automation::hook_http {

namespace {

constexpr log::Logger kLog{"services.automation.webhooks"};

constexpr std::string_view kClosedWithoutAnswer = "Remote end closed connection without response";

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_folded(std::string_view text, std::string_view needle) {
    if (needle.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool same = true;
        for (std::size_t j = 0; j < needle.size() && same; ++j) {
            same = ascii_lower(text[i + j]) == ascii_lower(needle[j]);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

// The number in "[WinError N] ..." or "[Errno N] ...", the form fv_http gives socket errors.
std::optional<long> socket_code(std::string_view message) {
    if (!message.starts_with('[')) {
        return std::nullopt;
    }
    const auto space = message.find(' ');
    const auto close = message.find(']');
    if (space == std::string_view::npos || close == std::string_view::npos || close < space) {
        return std::nullopt;
    }
    const auto number = py::py_int(message.substr(space + 1, close - space - 1));
    if (!number) {
        return std::nullopt;
    }
    return static_cast<long>(*number);
}

// A connection the other end reset or aborted, or a pipe it closed: httpx reports these from a
// read or a write (NetworkError), never from connecting.
bool broken_connection(std::string_view message) {
    static constexpr std::array<long, 5> kBroken = {10053, 10054, 32, 103, 104};
    const auto code = socket_code(message);
    return code && std::ranges::find(kBroken, *code) != kBroken.end();
}

// Takes the head, and the body of a 2xx reply up to `limit` bytes; ends the transfer as soon as
// the rest is not wanted, as leaving httpx's stream unread does.
class CappedReply final : public http::BodySink {
public:
    explicit CappedReply(std::size_t limit) : limit_(limit) {}

    bool on_head(const http::Response& head) override {
        status = head.status;
        retry_header = http::find_header(head.headers, "retry-after");
        return limit_ > 0 && head.status >= 200 && head.status < 300;
    }
    bool write(std::span<const char> bytes) override {
        if (body.size() + bytes.size() > limit_) {
            too_large = true;
            return false;
        }
        body.append(bytes.data(), bytes.size());
        return true;
    }

    std::optional<int> status;
    std::optional<std::string> retry_header;
    std::string body;
    bool too_large = false;

private:
    std::size_t limit_;
};

} // namespace

std::string iso_now(const Clock& clock) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(clock.utc());
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto millis = (now - seconds).count();
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03d}Z", seconds, millis);
}

std::optional<double> retry_after(std::optional<std::string_view> value,
                                  std::chrono::system_clock::time_point now) {
    if (!value || value->empty()) {
        return std::nullopt;
    }
    const std::string text = py::strip(*value);
    if (const auto seconds = py::py_float(text)) {
        if (!std::isfinite(*seconds)) {
            return std::nullopt;
        }
        return std::max(0.0, *seconds);
    }
    const auto when = detail::parse_http_date(text);
    if (!when) {
        return std::nullopt;
    }
    const double now_s =
        std::chrono::duration<double>(now.time_since_epoch()).count(); // Unix seconds
    return std::max(0.0, static_cast<double>(*when) - now_s);
}

std::string no_answer_copy(double timeout_s) {
    double rounded = timeout_s;
    try {
        rounded = py::round_ndigits(timeout_s, 3);
    } catch (const py::OverflowError&) {
        // round() of a huge float raises in Python too; the value itself is close enough.
    }
    return std::format("No answer within {:g} s", rounded);
}

std::string transport_copy(const http::Error& error) {
    switch (error.kind) {
    case http::ErrorKind::Dns:
    case http::ErrorKind::Tls:
        return "Couldn't connect";
    case http::ErrorKind::Connect:
        if (error.message == kClosedWithoutAnswer) {
            return "The server closed the connection";
        }
        if (error.after_request || broken_connection(error.message)) {
            return "The connection broke";
        }
        return "Couldn't connect";
    case http::ErrorKind::Protocol:
        if (contains_folded(error.message, "proxy") ||
            contains_folded(error.message, "CONNECT tunnel")) {
            return "The proxy refused the request";
        }
        return "The server closed the connection";
    case http::ErrorKind::Timeout:
        return "The request failed (TimeoutException)";
    case http::ErrorKind::Cancelled:
        return "The request failed (Cancelled)";
    case http::ErrorKind::TooLarge:
        return "The reply was too large";
    }
    return "The request failed (TransportError)";
}

bool is_private(std::string_view url) {
    return hooks::url_is_private(url);
}

Result exchange(http::Client& client, std::string_view url, std::string body,
                const http::Headers& headers, double timeout_s, std::size_t read_limit,
                std::optional<std::chrono::steady_clock::time_point> deadline,
                const CancelToken& cancel) {
    const auto started = std::chrono::steady_clock::now();
    Result result;
    const auto finish = [&]() -> Result {
        result.elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count());
        return std::move(result);
    };

    http::Request request;
    request.method = "POST";
    request.url = std::string(url);
    // The client's default User-Agent, which a caller's own header replaces (httpx merges request
    // headers over the client's).
    if (!http::find_header(headers, "user-agent")) {
        request.headers.emplace_back("User-Agent", http::user_agent());
    }
    for (const auto& header : headers) {
        request.headers.push_back(header);
    }
    request.body = std::move(body);
    const auto limit = std::chrono::milliseconds(
        static_cast<std::int64_t>(std::max(0.0, timeout_s) * 1000.0 + 0.5));
    request.connect_timeout = limit;
    request.idle_timeout = limit;
    request.deadline = deadline;
    request.follow_redirects = false;
    request.bypass_proxy = is_private(url);
    request.log_request = false; // webhooks.py _QuietRequests: hook URLs stay out of the log

    CappedReply reply(read_limit);
    std::expected<http::Response, http::Error> answer;
    try {
        answer = client.send(request, cancel, &reply);
    } catch (const std::exception& error) {
        // Type only: a message could carry the URL or the payload.
        kLog.warning("A hook request failed unexpectedly: {}", detail::error_type(error));
        result.status = reply.status;
        result.error = "The request failed (Exception)";
        return finish();
    }
    result.status = reply.status;
    if (!answer) {
        const http::Error& error = answer.error();
        if (error.kind == http::ErrorKind::Timeout) {
            result.error = no_answer_copy(timeout_s);
            result.retryable = true;
        } else if (error.kind == http::ErrorKind::TooLarge) {
            result.error = "The reply was too large";
        } else {
            result.error = transport_copy(error);
            result.retryable = error.kind != http::ErrorKind::Cancelled;
        }
        return finish();
    }
    if (reply.too_large) {
        result.error = "The reply was too large";
        return finish();
    }
    const int status = answer->status;
    result.status = status;
    if (status >= 200 && status < 300) {
        result.ok = true;
        result.body = std::move(reply.body);
        return finish();
    }
    if (status >= 300 && status < 400) {
        result.error = std::format("HTTP {}: redirects aren't followed", status);
        return finish();
    }
    result.error = std::format("HTTP {}", status);
    result.retryable = status == 408 || status == 429 || status >= 500;
    if (result.retryable) {
        result.retry_after = retry_after(reply.retry_header, std::chrono::system_clock::now());
    }
    return finish();
}

} // namespace fv::automation::hook_http
