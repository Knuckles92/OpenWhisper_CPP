#include "llm/streaming.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "base/clock.h"
#include "base/subscription.h"
#include "llm/detail/stream_protocol.h"
#include "llm/detail/truthy.h"
#include "llm/model_catalog.h"
#include "llm/sse.h"
#include "pyport/json.h"

namespace fv::llm {

namespace detail {

StreamEvent stream_event(StreamDelta kind, std::string text, std::string tool_call_id,
                         std::string tool_name) {
    return StreamEvent{kind, std::move(text), std::move(tool_call_id), std::move(tool_name)};
}

Json event_json(const SseEvent& event) {
    try {
        return py::loads(event.data);
    } catch (const std::exception&) {
        throw StreamError(std::string(kStreamMalformedError));
    }
}

std::optional<int> error_type_status(std::string_view type) {
    static constexpr std::pair<std::string_view, int> kTypes[] = {
        {"invalid_request_error", 400}, {"authentication_error", 401},
        {"permission_error", 403},      {"not_found_error", 404},
        {"request_too_large", 413},     {"rate_limit_error", 429},
        {"rate_limit_exceeded", 429},   {"api_error", 500},
        {"server_error", 500},          {"internal_error", 500},
        {"internal_server_error", 500}, {"service_unavailable", 503},
        {"overloaded_error", 529},
    };
    for (const auto& [name, status] : kTypes) {
        if (name == type) {
            return status;
        }
    }
    return std::nullopt;
}

void throw_stream_failure(std::optional<int> status, const Json& payload) {
    StreamError error{std::string(kStreamFailedError)};
    error.status = status;
    error.transient = status && (*status == 429 || *status >= 500);
    error.provider_message = provider_error_message_json(payload);
    throw error;
}

std::string string_at(const Json& object, std::string_view key) {
    const Json& value = get(object, key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

} // namespace detail

namespace {

bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// What the transfer thread hands to the thread that called stream_generate: the head, the body
// pieces in order, then the transfer's outcome.
struct Channel {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::variant<http::Response, std::string>> items;
    std::optional<std::expected<http::Response, http::Error>> outcome;

    bool finished() {
        std::lock_guard lock(mutex);
        return outcome.has_value();
    }
};

class ChannelSink final : public http::BodySink {
public:
    explicit ChannelSink(std::shared_ptr<Channel> channel) : channel_(std::move(channel)) {}

    bool on_head(const http::Response& head) override {
        push(head);
        // An error status's body is read (up to kErrorBodyLimit) only to be classified and to
        // take its cleaned message (status_error); it is never quoted.
        return true;
    }
    bool write(std::span<const char> bytes) override {
        push(std::string(bytes.data(), bytes.size()));
        return true;
    }

private:
    template <class Item> void push(Item item) {
        {
            std::lock_guard lock(channel_->mutex);
            channel_->items.emplace_back(std::move(item));
        }
        channel_->wake.notify_all();
    }

    std::shared_ptr<Channel> channel_;
};

// Transfers left behind by a cancel: curl notices the cancel at its next progress callback,
// which can take up to a second on a silent connection, and stream_generate does not wait for
// that. A reaper thread joins each one as soon as it has finished, and the rest at exit.
class Lingering {
public:
    ~Lingering() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (reaper_.joinable()) {
            reaper_.join();
        }
        for (auto& entry : threads_) {
            entry.first.join();
        }
    }

    void adopt(std::thread thread, std::shared_ptr<Channel> channel) {
        {
            std::lock_guard lock(mutex_);
            threads_.emplace_back(std::move(thread), std::move(channel));
            if (!reaper_.joinable()) {
                reaper_ = std::thread([this] { reap(); });
            }
        }
        wake_.notify_all();
    }

    // A transfer thread set its outcome (and holds no channel lock).
    void finished() {
        { std::lock_guard lock(mutex_); }
        wake_.notify_all();
    }

    std::size_t count() {
        std::lock_guard lock(mutex_);
        return threads_.size();
    }

private:
    void reap() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            std::vector<std::thread> done;
            for (auto entry = threads_.begin(); entry != threads_.end();) {
                if (entry->second->finished()) {
                    done.push_back(std::move(entry->first));
                    entry = threads_.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (!done.empty()) {
                lock.unlock();
                for (auto& thread : done) {
                    thread.join();
                }
                lock.lock();
                continue;
            }
            wake_.wait(lock, [this] {
                return stopping_ ||
                       std::ranges::any_of(threads_, [](auto& e) { return e.second->finished(); });
            });
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::vector<std::pair<std::thread, std::shared_ptr<Channel>>> threads_;
    std::thread reaper_;
};

Lingering& lingering() {
    static Lingering instance;
    return instance;
}

// The transfer thread of one request.
class Transfer {
public:
    Transfer(std::shared_ptr<http::Client> http, http::Request request, const CancelToken& cancel)
        : source_(cancel), channel_(std::make_shared<Channel>()) {
        thread_ = std::thread([http = std::move(http), request = std::move(request),
                               token = source_.token(), channel = channel_] {
            ChannelSink sink(channel);
            std::expected<http::Response, http::Error> outcome =
                std::unexpected(http::Error{http::ErrorKind::Protocol, "the transfer failed"});
            try {
                outcome = http->send(request, token, &sink);
            } catch (...) {
                // Nothing may leave a thread; the stream then reads as a failed connection.
            }
            {
                std::lock_guard lock(channel->mutex);
                channel->outcome = std::move(outcome);
            }
            channel->wake.notify_all();
            lingering().finished();
        });
    }

    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;

    ~Transfer() {
        if (!thread_.joinable()) {
            return;
        }
        source_.cancel();
        if (channel_->finished()) {
            thread_.join();
        } else {
            lingering().adopt(std::move(thread_), channel_);
        }
    }

    Channel& channel() { return *channel_; }
    void join() { thread_.join(); }

private:
    CancelSource source_;
    std::shared_ptr<Channel> channel_;
    std::thread thread_;
};

// What the agent loop's polish retry reads (sidecar.py's "corrupted thought signature" check):
// Gemini rejects a replayed reasoning signature with a 400 that says so. Only this fixed sentence
// leaves the body.
constexpr std::string_view kSignatureRejected = "Corrupted thought signature.";
constexpr std::size_t kErrorBodyLimit = 64 * 1024;

bool names_corrupted_signature(std::string body) {
    std::ranges::transform(body, body.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return body.find("corrupted thought signature") != std::string::npos;
}

StreamError status_error(const http::Response& head, const Clock& clock,
                         const std::string& body = {}) {
    const bool signature = head.status == 400 && names_corrupted_signature(body);
    StreamError error{signature ? std::string(kSignatureRejected)
                                : stream_status_message(head.status)};
    // The signature sentence stands in for the provider's message too, so the polish retry
    // finds it wherever the body said it.
    error.provider_message =
        signature ? std::string(kSignatureRejected) : provider_error_message(body);
    error.status = head.status;
    error.transient = head.status == 429 || head.status >= 500;
    if (const auto wait = parse_retry_after(head.headers, clock);
        wait && std::isfinite(*wait) && *wait >= 0) {
        error.retry_after_s = *wait;
    }
    return error;
}

bool is_json(const http::Headers& headers) {
    std::string type = http::find_header(headers, "content-type").value_or("");
    std::ranges::transform(type, type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return type.find("json") != std::string::npos;
}

std::unique_ptr<detail::StreamAccumulator> accumulator_for(std::string_view protocol) {
    if (protocol == "responses") {
        return detail::make_responses_accumulator();
    }
    if (protocol == "anthropic") {
        return detail::make_anthropic_accumulator();
    }
    if (protocol == "google") {
        return detail::make_google_accumulator();
    }
    return detail::make_chat_accumulator();
}

// The http::Client generate() sends through: it sends the request as an event stream, reports
// the deltas, and answers generate() with the non-streamed reply the stream amounts to.
class StreamTransport final : public http::Client {
public:
    StreamTransport(std::shared_ptr<http::Client> http, std::string protocol,
                    const StreamSink& sink, const Clock& clock)
        : http_(std::move(http)), protocol_(std::move(protocol)), sink_(sink), clock_(clock) {}

    std::expected<http::Response, http::Error>
    send(const http::Request& request, const CancelToken& cancel, http::BodySink*) override {
        reply_.reset();
        bool seen = false;
        try {
            return stream(request, cancel, seen);
        } catch (StreamError& error) {
            error.output_seen = error.output_seen || seen;
            throw;
        }
    }

    // The non-streamed reply the last request amounted to, as generate() received it.
    const std::optional<http::Response>& reply() const { return reply_; }

private:
    http::Request wire_request(const http::Request& request) const {
        http::Request wire = request;
        if (protocol_ == "anthropic") {
            if (auto* body = std::get_if<std::string>(&wire.body); body && body->ends_with('}')) {
                body->pop_back();
                if (body->size() > 1) {
                    body->push_back(',');
                }
                body->append("\"stream\":true}");
            }
        } else if (protocol_ == "google") {
            constexpr std::string_view kCall = ":generateContent";
            if (const auto mark = wire.url.rfind(kCall); mark != std::string::npos) {
                wire.url.replace(mark, kCall.size(), ":streamGenerateContent");
            }
            wire.url += wire.url.find('?') == std::string::npos ? "?alt=sse" : "&alt=sse";
        }
        if (protocol_ == "anthropic" || protocol_ == "google") {
            const std::string_view name = protocol_ == "anthropic" ? "x-api-key" : "x-goog-api-key";
            const auto auth = std::ranges::find_if(wire.headers, [](const auto& entry) {
                return same_name(entry.first, "authorization");
            });
            if (auth != wire.headers.end() && auth->second.starts_with("Bearer ")) {
                std::string key = auth->second.substr(7);
                wire.headers.erase(auth);
                if (!http::find_header(wire.headers, name)) {
                    wire.headers.emplace_back(std::string(name), std::move(key));
                }
            }
        }
        return wire;
    }

    http::Response stream(const http::Request& request, const CancelToken& cancel, bool& seen) {
        Transfer transfer(http_, wire_request(request), cancel);
        Channel& channel = transfer.channel();
        const Subscription woken = cancel.on_cancel([&channel] {
            std::lock_guard lock(channel.mutex);
            channel.wake.notify_all();
        });
        const detail::EmitDelta emit = [this, &seen](const StreamEvent& event) {
            seen = true;
            if (sink_) {
                sink_(event);
            }
        };
        auto accumulator = accumulator_for(protocol_);
        SseParser parser;
        const SseHandler on_event = [&](const SseEvent& event) {
            accumulator->on_event(event, emit);
        };
        std::optional<http::Response> head;
        bool plain = false;
        bool rejected = false;
        std::string plain_body;
        std::string error_body;
        while (true) {
            std::optional<std::variant<http::Response, std::string>> item;
            std::optional<std::expected<http::Response, http::Error>> outcome;
            bool cancelled = false;
            {
                std::unique_lock lock(channel.mutex);
                channel.wake.wait(lock, [&] {
                    return cancel.cancelled() || !channel.items.empty() ||
                           channel.outcome.has_value();
                });
                if (cancel.cancelled()) {
                    cancelled = true;
                } else if (!channel.items.empty()) {
                    item = std::move(channel.items.front());
                    channel.items.pop_front();
                } else {
                    outcome = std::move(channel.outcome);
                }
            }
            if (cancelled) {
                throw TextGenerationError("canceled");
            }
            if (item) {
                if (auto* received = std::get_if<http::Response>(&*item)) {
                    head = std::move(*received);
                    rejected = head->status < 200 || head->status >= 300;
                    plain = is_json(head->headers);
                } else if (rejected) {
                    const auto& piece = std::get<std::string>(*item);
                    const std::size_t room =
                        kErrorBodyLimit - std::min(kErrorBodyLimit, error_body.size());
                    error_body.append(piece, 0, std::min(piece.size(), room));
                } else if (plain) {
                    plain_body += std::get<std::string>(*item);
                } else {
                    parser.feed(std::get<std::string>(*item), on_event);
                }
                continue;
            }
            transfer.join();
            if (!outcome->has_value()) {
                const http::Error& error = outcome->error();
                if (error.kind == http::ErrorKind::Cancelled || cancel.cancelled()) {
                    throw TextGenerationError("canceled");
                }
                if (rejected) {
                    throw status_error(*head, clock_, error_body);
                }
                StreamError failure{std::string(error.kind == http::ErrorKind::Timeout
                                                    ? kStreamTimeoutError
                                                : head ? kStreamDroppedError
                                                       : kStreamConnectionError)};
                failure.transient = true;
                throw failure;
            }
            http::Response finished = std::move(**outcome);
            if (rejected) {
                throw status_error(*head, clock_, error_body);
            }
            if (!head) {
                head = finished;
                if (head->status < 200 || head->status >= 300) {
                    throw status_error(*head, clock_, finished.body);
                }
                plain = is_json(head->headers);
            }
            if (plain) {
                http::Response reply = *head;
                reply.body = std::move(plain_body);
                reply_ = reply;
                return reply;
            }
            parser.finish(on_event);
            if (!accumulator->complete()) {
                StreamError failure{std::string(kStreamDroppedError)};
                failure.transient = true;
                throw failure;
            }
            const Json body = accumulator->reply(emit);
            http::Response reply;
            reply.status = 200;
            reply.reason = head->reason;
            reply.headers = {{"Content-Type", "application/json"}};
            reply.effective_url = head->effective_url;
            reply.body = py::dumps(body);
            reply_ = reply;
            return reply;
        }
    }

    std::shared_ptr<http::Client> http_;
    std::string protocol_;
    const StreamSink& sink_;
    // The client's clock: an HTTP-date retry-after is measured from it.
    const Clock& clock_;
    std::optional<http::Response> reply_;
};

// Answers every request with one reply: how a settled length stop goes back through generate()'s
// own reader without a second request.
class ReplayClient final : public http::Client {
public:
    explicit ReplayClient(http::Response reply) : reply_(std::move(reply)) {}

    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken&,
                                                    http::BodySink*) override {
        return reply_;
    }

private:
    http::Response reply_;
};

// The reply a length stop amounts to with its stop marked as a normal one, or nullopt when the
// reply did not stop at the output limit. Pi's providers map these to stopReason "length" (a
// Responses reply incomplete for another reason, such as a content filter, stays a failure).
std::optional<Json> settled_length_reply(std::string_view protocol, Json reply) {
    if (!reply.is_object()) {
        return std::nullopt;
    }
    const auto first = [](Json& object, std::string_view key) -> Json* {
        const auto list = object.find(key);
        if (list == object.end() || !list->is_array() || list->empty() ||
            !list->front().is_object()) {
            return nullptr;
        }
        return &list->front();
    };
    if (protocol == "responses") {
        const auto error = reply.find("error");
        if (reply.value("status", Json()) != "incomplete" ||
            (error != reply.end() && !error->is_null())) {
            return std::nullopt;
        }
        const auto details = reply.find("incomplete_details");
        const std::string reason = details != reply.end() && details->is_object()
                                       ? detail::string_at(*details, "reason")
                                       : std::string();
        if (!reason.empty() && reason != "max_output_tokens") {
            return std::nullopt;
        }
        reply["status"] = "completed";
        return reply;
    }
    if (protocol == "anthropic") {
        if (reply.value("stop_reason", Json()) != "max_tokens") {
            return std::nullopt;
        }
        reply["stop_reason"] = "end_turn";
        return reply;
    }
    if (protocol == "google") {
        Json* candidate = first(reply, "candidates");
        if (candidate == nullptr || candidate->value("finishReason", Json()) != "MAX_TOKENS") {
            return std::nullopt;
        }
        (*candidate)["finishReason"] = "STOP";
        return reply;
    }
    Json* choice = first(reply, "choices");
    if (choice == nullptr || choice->value("finish_reason", Json()) != "length") {
        return std::nullopt;
    }
    (*choice)["finish_reason"] = "stop";
    return reply;
}

// A key, token or signature: 20 or more base64 or base64url characters with a digit among them
// (a long plain word is not a key).
bool key_shaped(std::string_view run) {
    return run.size() >= 20 &&
           std::ranges::any_of(run, [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool base64_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '+' || c == '/' || c == '_' || c == '-' || c == '=';
}

std::string clean_provider_message(std::string text) {
    if (const auto end = text.find_first_of("\r\n"); end != std::string::npos) {
        text.resize(end);
    }
    // Whole words first: "Bearer <token>", "sk-..." keys, and the query of any URL.
    std::vector<std::string> words;
    for (std::size_t at = 0; at < text.size();) {
        const auto start = text.find_first_not_of(" \t", at);
        if (start == std::string::npos) {
            break;
        }
        const auto stop = std::min(text.find_first_of(" \t", start), text.size());
        words.push_back(text.substr(start, stop - start));
        at = stop;
    }
    std::string kept;
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::string word = words[i];
        if (same_name(word, "bearer")) {
            ++i; // the token after it
            continue;
        }
        if (word.starts_with("sk-") || word.starts_with("\"sk-") || word.starts_with("'sk-")) {
            continue;
        }
        if (word.find("://") != std::string::npos) {
            if (const auto query = word.find('?'); query != std::string::npos) {
                word.resize(query);
            }
        }
        // Then any key-shaped run inside what is left of the word.
        std::string cleaned;
        for (std::size_t at = 0; at < word.size();) {
            if (!base64_char(static_cast<unsigned char>(word[at]))) {
                cleaned += word[at++];
                continue;
            }
            auto stop = at;
            while (stop < word.size() && base64_char(static_cast<unsigned char>(word[stop]))) {
                ++stop;
            }
            const std::string_view run(word.data() + at, stop - at);
            if (!key_shaped(run)) {
                cleaned += run;
            }
            at = stop;
        }
        if (!cleaned.empty()) {
            if (!kept.empty()) {
                kept += ' ';
            }
            kept += cleaned;
        }
    }
    // At most 200 code points (UTF-8 continuation bytes do not start one).
    std::size_t points = 0;
    for (std::size_t at = 0; at < kept.size(); ++at) {
        if ((static_cast<unsigned char>(kept[at]) & 0xC0) != 0x80 && ++points > 200) {
            kept.resize(at);
            break;
        }
    }
    while (!kept.empty() && kept.back() == ' ') {
        kept.pop_back();
    }
    return kept;
}

} // namespace

std::string provider_error_message_json(const Json& body) {
    // Google's streamed endpoint answers an error as a one-element list.
    const Json& source = body.is_array() && !body.empty() ? body.front() : body;
    if (!source.is_object()) {
        return {};
    }
    const auto text_at = [](const Json& object, std::string_view key) -> std::string {
        const auto found = object.find(key);
        return found != object.end() && found->is_string() ? found->get<std::string>()
                                                           : std::string();
    };
    std::string message;
    if (const auto error = source.find("error"); error != source.end() && error->is_object()) {
        message = text_at(*error, "message");
    }
    if (message.empty()) {
        message = text_at(source, "message");
    }
    return clean_provider_message(std::move(message));
}

std::string provider_error_message(std::string_view body) {
    const Json parsed = Json::parse(body, nullptr, false);
    return parsed.is_discarded() ? std::string() : provider_error_message_json(parsed);
}

std::string_view stream_delta_name(StreamDelta kind) {
    switch (kind) {
    case StreamDelta::ThinkingStart:
        return "thinking_start";
    case StreamDelta::ThinkingDelta:
        return "thinking_delta";
    case StreamDelta::ThinkingEnd:
        return "thinking_end";
    case StreamDelta::TextDelta:
        return "text_delta";
    case StreamDelta::ToolCallStart:
        return "toolcall_start";
    case StreamDelta::ToolCallDelta:
        return "toolcall_delta";
    case StreamDelta::ToolCallEnd:
        return "toolcall_end";
    }
    return {};
}

std::string stream_status_message(int status) {
    return "Error code: " + std::to_string(status);
}

TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile& profile,
                           GenerateRequest request, const StreamSink& sink) {
    return stream_generate(client, &profile, std::move(request), sink);
}

TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile& profile,
                           GenerateRequest request, const StreamSink& sink, StreamOptions options) {
    return stream_generate(client, &profile, std::move(request), sink, options);
}

TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile* profile,
                           GenerateRequest request, const StreamSink& sink) {
    return stream_generate(client, profile, std::move(request), sink, StreamOptions{});
}

TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile* profile,
                           GenerateRequest request, const StreamSink& sink, StreamOptions options) {
    if (request.cancel.cancelled()) {
        throw TextGenerationError("canceled");
    }
    const TextModelSpec spec =
        profile != nullptr ? model_spec(*profile, request.model) : TextModelSpec{};
    if (!request.options.is_object()) {
        request.options = Json::object();
    }
    // Chat and Responses take these as SDK parameters, so they sit among the other options in
    // the SDK's order; the native protocols' bodies are built without the options.
    if (spec.protocol == "chat") {
        request.options["stream"] = true;
        request.options["stream_options"] = Json{{"include_usage", true}};
    } else if (spec.protocol == "responses") {
        request.options["stream"] = true;
    }
    auto transport = std::make_shared<StreamTransport>(client.http_client(), spec.protocol, sink,
                                                       client.clock());
    OpenAiCompatClient streaming = client.with_http_client(transport).with_options(std::nullopt, 0);
    if (!options.settle_length_stop) {
        return generate(streaming, profile, std::move(request));
    }
    GenerateRequest again = request;
    try {
        return generate(streaming, profile, std::move(request));
    } catch (const StreamError&) {
        throw;
    } catch (const TextGenerationError&) {
        const auto& reply = transport->reply();
        if (!reply) {
            throw;
        }
        auto settled =
            settled_length_reply(spec.protocol, Json::parse(reply->body, nullptr, false));
        if (!settled) {
            throw;
        }
        http::Response replayed = *reply;
        replayed.body = py::dumps(*settled);
        OpenAiCompatClient reader =
            client.with_http_client(std::make_shared<ReplayClient>(std::move(replayed)))
                .with_options(std::nullopt, 0);
        TextResult result = generate(reader, profile, std::move(again));
        result.finish_reason = "length";
        return result;
    }
}

namespace detail {

std::size_t lingering_stream_transfers() {
    return lingering().count();
}

} // namespace detail

} // namespace fv::llm
