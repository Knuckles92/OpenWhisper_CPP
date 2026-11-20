#pragma once

// Helpers for the server tests: a log capture, the curl client of fv_http pointed at loopback
// without any proxy, and a raw socket client for what curl hides (keep-alive, exact bytes).
// Everything here talks to 127.0.0.1 only.

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/log.h"
#include "http/client.h"
#include "net/http_server.h"

namespace fv::net::testing {

class LogCapture final : public log::Sink {
public:
    struct Line {
        log::Level level;
        std::string logger;
        std::string message;
        std::string exception_type;
    };

    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        lines_.push_back({record.level, std::string(record.logger), std::string(record.message),
                          std::string(record.exception_type)});
    }
    std::vector<Line> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }
    bool any(log::Level at_least) const {
        for (const auto& line : lines()) {
            if (line.level >= at_least) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
};

// Captures every record, DEBUG included, for the scope; the root level comes back after.
struct ScopedCapture {
    ScopedCapture() : capture(std::make_shared<LogCapture>()), previous(log::level()) {
        log::set_level(log::Level::Debug);
        log::add_sink(capture);
    }
    ~ScopedCapture() {
        log::remove_sink(capture);
        log::set_level(previous);
    }
    ScopedCapture(const ScopedCapture&) = delete;
    ScopedCapture& operator=(const ScopedCapture&) = delete;
    LogCapture* operator->() const { return capture.get(); }

    std::shared_ptr<LogCapture> capture;
    log::Level previous;
};

inline std::shared_ptr<http::Client> loopback_client() {
    http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return http::make_curl_client(options);
}

inline std::string url(std::uint16_t port, std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
}

inline http::Request request(std::string method, std::string target_url, std::string body = {}) {
    http::Request request;
    request.method = std::move(method);
    request.url = std::move(target_url);
    request.bypass_proxy = true;
    request.idle_timeout = std::chrono::milliseconds(10'000);
    if (!body.empty()) {
        request.body = std::move(body);
    }
    return request;
}

inline Endpoint listen_loopback(HttpServer& server) {
    auto endpoint = server.listen(Listen{.address = "127.0.0.1", .port = 0});
    if (!endpoint) {
        throw std::runtime_error(endpoint.error());
    }
    return *endpoint;
}

// One blocking TCP connection to 127.0.0.1, for exact bytes and keep-alive.
class RawClient {
public:
    explicit RawClient(std::uint16_t port) : socket_(io_) {
        socket_.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    }

    void send(std::string_view bytes) { boost::asio::write(socket_, boost::asio::buffer(bytes)); }

    // One response: the head, then a Content-Length body (none for a HEAD request). Empty when
    // the server closed the connection first.
    std::string read_response(bool head_request = false) {
        std::string head = read_until("\r\n\r\n");
        if (head.empty()) {
            return {};
        }
        std::size_t length = 0;
        const auto lowered = lower(head);
        if (const auto at = lowered.find("\r\ncontent-length: "); at != std::string::npos) {
            length = std::stoul(head.substr(at + 18));
        }
        if (head_request) {
            return head;
        }
        return head + read_exactly(length);
    }

    // Everything until the server closes the connection.
    std::string read_to_end() {
        std::string out = std::move(pending_);
        pending_.clear();
        boost::system::error_code error;
        char chunk[4096];
        while (true) {
            const auto n = socket_.read_some(boost::asio::buffer(chunk), error);
            if (error) {
                return out;
            }
            out.append(chunk, n);
        }
    }

    // True when the server has closed the connection (a read ends at once with EOF).
    bool closed_by_server() {
        boost::system::error_code error;
        char byte;
        socket_.read_some(boost::asio::buffer(&byte, 1), error);
        return static_cast<bool>(error);
    }

    boost::asio::ip::tcp::socket& socket() { return socket_; }

    // What the last read_until read past its marker: the start of a body that arrived in the
    // same segment as the head (more likely the busier the machine). A test that goes on
    // reading socket() itself counts these bytes first.
    std::string take_pending() { return std::exchange(pending_, {}); }

    static std::string lower(std::string text) {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return text;
    }

    // Up to and including marker; empty when the connection ends first.
    std::string read_until(std::string_view marker) {
        boost::system::error_code error;
        char chunk[4096];
        while (pending_.find(marker) == std::string::npos) {
            const auto n = socket_.read_some(boost::asio::buffer(chunk), error);
            if (error) {
                return {};
            }
            pending_.append(chunk, n);
        }
        const auto end = pending_.find(marker) + marker.size();
        std::string out = pending_.substr(0, end);
        pending_.erase(0, end);
        return out;
    }

    std::string read_exactly(std::size_t length) {
        boost::system::error_code error;
        char chunk[4096];
        while (pending_.size() < length) {
            const auto n = socket_.read_some(boost::asio::buffer(chunk), error);
            if (error) {
                break;
            }
            pending_.append(chunk, n);
        }
        std::string out = pending_.substr(0, length);
        pending_.erase(0, std::min(length, pending_.size()));
        return out;
    }

private:
    boost::asio::io_context io_;
    boost::asio::ip::tcp::socket socket_;
    std::string pending_;
};

} // namespace fv::net::testing
