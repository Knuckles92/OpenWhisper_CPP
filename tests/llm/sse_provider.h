#pragma once

// A fake model provider on 127.0.0.1 (ephemeral port) for the tests of streamed generation and
// of everything built on it: it serves a list of replies in order (status, headers, an event
// stream or a plain JSON body, an optional delay between pieces, a point where it stalls or
// drops the connection) and records each request (method, path, headers with the credentials
// masked, body). Nothing leaves the loopback interface. Tests that include it add
// Boost::asio OpenSSL::SSL OpenSSL::Crypto to their fv_test DEPS.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../http/loopback_server.h"
#include "base/json.h"
#include "pyport/json.h"

namespace fv::testing {

struct SseReply {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    // The body, written piece by piece: chunked when `chunked`, else with a Content-Length.
    std::vector<std::string> pieces;
    bool chunked = true;
    std::chrono::milliseconds piece_delay{0};
    // After this many pieces the connection stays open and silent until the provider stops.
    std::optional<std::size_t> stall_after;
    // After this many pieces the connection closes in the middle of the body.
    std::optional<std::size_t> drop_after;

    // "data: <json>\n\n", with "event: <type>\n" first when `named`.
    static std::string frame(const Json& data, bool named = false) {
        std::string out;
        if (named && data.is_object() && data.contains("type")) {
            out += "event: " + data.at("type").get<std::string>() + "\n";
        }
        return out + "data: " + py::dumps(data) + "\n\n";
    }

    // An event stream of `frames`, one piece each.
    static SseReply stream(std::vector<std::string> frames) {
        SseReply reply;
        reply.headers = {{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-cache"}};
        reply.pieces = std::move(frames);
        return reply;
    }
    static SseReply events(const std::vector<Json>& data, bool named = false) {
        std::vector<std::string> frames;
        for (const auto& item : data) {
            frames.push_back(frame(item, named));
        }
        return stream(std::move(frames));
    }

    // A whole JSON answer.
    static SseReply json(int status, const Json& body, std::string reason = {}) {
        SseReply reply;
        reply.status = status;
        reply.reason = reason.empty() ? standard_reason(status) : std::move(reason);
        reply.headers = {{"Content-Type", "application/json"}};
        reply.pieces = {py::dumps(body)};
        reply.chunked = false;
        return reply;
    }

    static std::string standard_reason(int status) {
        switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Status";
        }
    }
};

struct SseRequest {
    std::string method;
    std::string path; // as on the request line
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::optional<std::string> header(std::string_view name) const {
        for (const auto& [key, value] : headers) {
            if (key.size() == name.size() &&
                std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) {
                return value;
            }
        }
        return std::nullopt;
    }
    Json json() const { return body.empty() ? Json() : py::loads(body); }
};

class SseProvider {
public:
    explicit SseProvider(std::vector<SseReply> replies = {})
        : replies_(std::move(replies)),
          server_([this](const LoopbackServer::Request& request, LoopbackServer::Reply& reply) {
              serve(request, reply);
          }) {}

    SseProvider(const SseProvider&) = delete;
    SseProvider& operator=(const SseProvider&) = delete;

    void push(SseReply reply) {
        std::lock_guard lock(mutex_);
        replies_.push_back(std::move(reply));
    }

    // "http://127.0.0.1:<port><path>".
    std::string url(std::string_view path = "") const { return server_.url(path); }

    std::vector<SseRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    // How a credential header is recorded: a fingerprint, never the value. A test compares a
    // recorded value with mask(expected).
    static std::string mask(std::string_view value) {
        std::uint64_t hash = 1469598103934665603ULL; // FNV-1a
        for (const char c : value) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
        char text[32];
        std::snprintf(text, sizeof text, "masked:%016llx", static_cast<unsigned long long>(hash));
        return text;
    }

private:
    static bool credential(std::string_view name) {
        std::string lower(name);
        for (auto& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lower == "authorization" || lower == "x-api-key" || lower == "x-goog-api-key";
    }

    void serve(const LoopbackServer::Request& request, LoopbackServer::Reply& out) {
        SseRequest recorded{request.method, request.target, {}, request.body};
        for (const auto& [name, value] : request.headers) {
            recorded.headers.emplace_back(name, credential(name) ? mask(value) : value);
        }
        std::optional<SseReply> reply;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(std::move(recorded));
            if (next_ < replies_.size()) {
                reply = replies_[next_++];
            }
        }
        if (!reply) {
            reply = SseReply::json(404, Json{{"error", "no reply scripted"}});
        }
        std::string head =
            "HTTP/1.1 " + std::to_string(reply->status) + " " + reply->reason + "\r\n";
        for (const auto& [name, value] : reply->headers) {
            head += name + ": " + value + "\r\n";
        }
        if (reply->chunked) {
            head += "Transfer-Encoding: chunked\r\n";
        } else {
            std::size_t length = 0;
            for (const auto& piece : reply->pieces) {
                length += piece.size();
            }
            head += "Content-Length: " + std::to_string(length) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";
        out.write(head);
        for (std::size_t i = 0; i < reply->pieces.size(); ++i) {
            if (reply->stall_after && i == *reply->stall_after) {
                out.wait(std::chrono::minutes(10));
                return;
            }
            if (reply->drop_after && i == *reply->drop_after) {
                return;
            }
            if (i > 0 && reply->piece_delay.count() > 0 && !out.wait(reply->piece_delay)) {
                return;
            }
            const std::string& piece = reply->pieces[i];
            if (reply->chunked) {
                if (piece.empty()) {
                    continue; // a zero-size chunk would end the body
                }
                char size[24];
                std::snprintf(size, sizeof size, "%zx\r\n", piece.size());
                out.write(std::string(size) + piece + "\r\n");
            } else {
                out.write(piece);
            }
        }
        if (reply->stall_after && *reply->stall_after >= reply->pieces.size()) {
            out.wait(std::chrono::minutes(10));
            return;
        }
        if (reply->drop_after) {
            return;
        }
        if (reply->chunked) {
            out.write("0\r\n\r\n");
        }
    }

    mutable std::mutex mutex_;
    std::vector<SseReply> replies_;
    std::size_t next_ = 0;
    std::vector<SseRequest> requests_;
    LoopbackServer server_; // last: stopped first, while the replies are still there
};

} // namespace fv::testing
