#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "http/client.h"

namespace fv::http {

// One scripted answer of a FakeHttpClient.
struct ScriptedResponse {
    int status = 200;
    std::string reason = "OK";
    Headers headers;
    std::string body;
    // Sizes in which the body reaches a BodySink (the rest in one piece); empty: one piece.
    std::vector<std::size_t> chunks;
    // Returned instead of any response, as a failed connect or TLS handshake.
    std::optional<Error> error;
    // Returned after the body was delivered, as a connection lost mid-body (after_head and
    // after_request are set). An `error` with after_request set stands for a server that took
    // the request and never answered it.
    std::optional<Error> cut;
    // Runs before each body piece reaches the sink, as a test's hook (setting a cancel, say).
    std::function<void()> on_chunk;
};

// A Client that answers from a script, for tests of code that downloads: queued responses, or a
// handler that computes one per request. It records every request and checks the cancel token
// before the head and between body pieces, as the curl client's transfer callback does.
// Affinity: thread-safe.
class FakeHttpClient final : public Client {
public:
    using Handler = std::function<ScriptedResponse(const Request&)>;

    void push(ScriptedResponse response) {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(response));
    }
    // Answers every request once the queue is empty.
    void set_handler(Handler handler) {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }
    std::vector<Request> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    // The parts of each multipart request, in requests() order (empty for any other body), with
    // every file part's bytes as read when the request was sent: what was uploaded, after the
    // file is gone.
    std::vector<std::vector<MultipartPart>> multipart_parts() const {
        std::lock_guard lock(mutex_);
        return multipart_parts_;
    }
    std::size_t pending() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    // A server holding `content` that honours Range as a static file server does: 206 with an
    // exact Content-Range and Content-Length from range_from, 416 past the end, else 200.
    static ScriptedResponse serve(const Request& request, std::string_view content) {
        ScriptedResponse response;
        const auto size = content.size();
        if (request.range_from) {
            const auto from = *request.range_from;
            if (from >= size) {
                response.status = 416;
                response.reason = "Range Not Satisfiable";
                response.headers = {{"Content-Range", "bytes */" + std::to_string(size)}};
                return response;
            }
            response.status = 206;
            response.reason = "Partial Content";
            response.headers = {{"Content-Range", "bytes " + std::to_string(from) + "-" +
                                                      std::to_string(size - 1) + "/" +
                                                      std::to_string(size)},
                                {"Content-Length", std::to_string(size - from)}};
            response.body = std::string(content.substr(static_cast<std::size_t>(from)));
            return response;
        }
        response.headers = {{"Content-Length", std::to_string(size)}};
        response.body = std::string(content);
        return response;
    }

    // What the curl client returns when the server's certificate does not verify.
    static Error certificate_failure() {
        Error error{ErrorKind::Tls, "[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: "
                                    "self-signed certificate"};
        error.certificate = true;
        return error;
    }

    std::expected<Response, Error> send(const Request& request, const CancelToken& cancel,
                                        BodySink* stream) override {
        ScriptedResponse scripted;
        std::vector<MultipartPart> parts;
        if (const auto* multipart = std::get_if<Multipart>(&request.body)) {
            for (MultipartPart part : multipart->parts) {
                if (const auto* path = std::get_if<std::filesystem::path>(&part.data)) {
                    std::ifstream file(*path, std::ios::binary);
                    if (!file) {
                        // As the curl client: a file part that cannot be opened fails first.
                        return std::unexpected(
                            Error{ErrorKind::Protocol, "the request body file could not be read"});
                    }
                    part.data = std::string(std::istreambuf_iterator<char>(file),
                                            std::istreambuf_iterator<char>());
                }
                parts.push_back(std::move(part));
            }
        }
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
            multipart_parts_.push_back(std::move(parts));
            if (!queue_.empty()) {
                scripted = std::move(queue_.front());
                queue_.pop_front();
            } else if (handler_) {
                scripted = handler_(request);
            } else {
                return std::unexpected(
                    Error{ErrorKind::Connect, "FakeHttpClient: no response scripted"});
            }
        }
        if (cancel.cancelled()) {
            return std::unexpected(Error{ErrorKind::Cancelled, "canceled"});
        }
        if (scripted.error) {
            return std::unexpected(*scripted.error);
        }
        Response head;
        head.status = scripted.status;
        head.reason = scripted.reason;
        head.headers = scripted.headers;
        head.effective_url = request.url;
        if (stream != nullptr && !stream->on_head(head)) {
            return head;
        }
        std::vector<std::string_view> pieces;
        std::string_view rest(scripted.body);
        for (const auto size : scripted.chunks) {
            if (rest.empty()) {
                break;
            }
            pieces.push_back(rest.substr(0, size));
            rest.remove_prefix(std::min(size, rest.size()));
        }
        if (!rest.empty()) {
            pieces.push_back(rest);
        }
        std::string body;
        for (const auto piece : pieces) {
            if (cancel.cancelled()) {
                return std::unexpected(Error{ErrorKind::Cancelled, "canceled"});
            }
            if (scripted.on_chunk) {
                scripted.on_chunk();
            }
            if (stream != nullptr) {
                if (!stream->write(std::span<const char>(piece.data(), piece.size()))) {
                    return head;
                }
            } else {
                if (body.size() + piece.size() > request.max_body_bytes) {
                    return std::unexpected(Error{ErrorKind::TooLarge, "the response is too large"});
                }
                body.append(piece);
            }
        }
        if (scripted.cut) {
            Error error = *scripted.cut;
            error.after_head = true;
            error.after_request = true;
            return std::unexpected(std::move(error));
        }
        head.body = std::move(body);
        return head;
    }

private:
    mutable std::mutex mutex_;
    std::deque<ScriptedResponse> queue_;
    Handler handler_;
    std::vector<Request> requests_;
    std::vector<std::vector<MultipartPart>> multipart_parts_;
};

} // namespace fv::http
