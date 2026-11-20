#include "agentapi/mcp_events_delivery.h"

#include <algorithm>
#include <span>
#include <utility>

#include "automation/event_bus.h"
#include "automation/signing.h"
#include "base/hmac.h"
#include "base/ids.h"
#include "base/log.h"
#include "http/user_agent.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"

namespace fv::agentapi::events_ext {

namespace {

constexpr log::Logger kLog{"services.agent_mcp.events_ext"};

// Waits for a due job at most this long at a time, so a clock that moves without waking anyone
// (a test's) is still followed.
constexpr std::chrono::milliseconds kLongestWait{100};

// Keeps the first MAX_REPLY_BYTES of an answer, then ends the transfer.
class ReplySink final : public http::BodySink {
public:
    bool write(std::span<const char> chunk) override {
        received.append(chunk.data(), chunk.size());
        return received.size() < MAX_REPLY_BYTES;
    }
    std::string received;
};

std::chrono::steady_clock::duration seconds(double value) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(value));
}

py::DumpOptions compact(bool ensure_ascii) {
    py::DumpOptions options;
    options.ensure_ascii = ensure_ascii;
    options.item_sep = ",";
    options.key_sep = ":";
    return options;
}

} // namespace

std::string status_category(int status) {
    if (status >= 200 && status < 300) {
        return "";
    }
    if (status == 413) {
        return "payload_too_large";
    }
    if (status < 400) {
        return "redirect";
    }
    return status < 500 ? "http_4xx" : "http_5xx";
}

std::optional<double> retry_after(std::optional<std::string_view> value) {
    if (!value || value->empty()) {
        return std::nullopt;
    }
    const auto seconds = py::py_float(*value);
    if (!seconds || !(*seconds >= 0)) {
        return std::nullopt;
    }
    return seconds;
}

Reply post(http::Client& client, std::string_view url, const std::string& body,
           const http::Headers& headers, double timeout_s,
           const std::optional<std::string>& connect_to, const CancelToken& cancel) {
    http::Request request;
    request.method = "POST";
    request.url = std::string(url);
    request.headers = headers;
    request.headers.emplace_back("user-agent", http::user_agent());
    request.body = body;
    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(timeout_s));
    request.connect_timeout = timeout;
    request.idle_timeout = timeout;
    request.follow_redirects = false;
    request.bypass_proxy = true;
    // The URL and its query can carry a secret: the request's own log line is dropped.
    request.log_request = false;
    request.pinned_address = connect_to;
    ReplySink sink;
    const auto result = client.send(request, cancel, &sink);
    Reply reply;
    if (!result) {
        switch (result.error().kind) {
        case http::ErrorKind::Timeout:
            reply.category = "timeout";
            break;
        case http::ErrorKind::Tls:
            reply.category = "tls_error";
            break;
        default:
            reply.category = "connection_refused";
            break;
        }
        return reply;
    }
    reply.status = result->status;
    reply.body = sink.received.substr(0, MAX_REPLY_BYTES);
    reply.category = status_category(result->status);
    // httpx's Headers.get joins repeated values with ", ".
    std::optional<std::string> retry;
    for (const auto& [name, value] : result->headers) {
        std::string lower = name;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        if (lower == "retry-after") {
            retry = retry ? *retry + ", " + value : value;
        }
    }
    reply.retry_after = retry_after(retry);
    return reply;
}

http::Headers signed_headers(const std::vector<std::string>& keys, std::string_view msg_id,
                             std::string_view body, std::string_view subscription_id,
                             std::int64_t timestamp) {
    http::Headers headers = automation::signing::headers(keys.front(), msg_id, body, timestamp);
    std::string extra;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        extra += " " + automation::signing::signature(keys[i], msg_id, timestamp, body);
    }
    if (!extra.empty()) {
        for (auto& [name, value] : headers) {
            if (name == "webhook-signature") {
                value += extra;
            }
        }
    }
    headers.emplace_back("content-type", "application/json");
    headers.emplace_back("x-mcp-subscription-id", std::string(subscription_id));
    return headers;
}

std::string event_body(const automation::Event& event) {
    Json body = Json::object();
    body["eventId"] = event.id;
    body["name"] = event.type;
    body["timestamp"] = event.time;
    body["data"] = event.data.is_object() ? event.data : Json::object();
    body["cursor"] = nullptr;
    return py::dumps(body, compact(true));
}

CallbackVerifier::CallbackVerifier(http::Client& client, const Clock& clock)
    : client_(client), clock_(clock) {}

void CallbackVerifier::verify(std::string_view subscription_id, const std::string& url,
                              const std::string& secret,
                              const std::optional<std::string>& peer_address,
                              const CancelToken& cancel) {
    {
        std::lock_guard lock(mutex_);
        const auto found = verified_.find(url);
        if (found != verified_.end() && found->second > clock_.steady()) {
            return;
        }
    }
    const std::string challenge = token_urlsafe(32);
    Json payload = Json::object();
    payload["type"] = "verification";
    payload["challenge"] = challenge;
    const std::string body = py::dumps(payload, compact(true));
    const std::string msg_id = "msg_verification_" + random_hex(24);
    const auto timestamp = static_cast<std::int64_t>(
        std::chrono::duration<double>(clock_.utc().time_since_epoch()).count());
    const Reply reply =
        post(client_, url, body, signed_headers({secret}, msg_id, body, subscription_id, timestamp),
             REQUEST_TIMEOUT_S, peer_address, cancel);
    std::string reason = "challenge_failed";
    if (reply.ok()) {
        std::optional<Json> answer;
        if (decodes_as_utf8(reply.body)) {
            try {
                answer = py::loads(reply.body);
            } catch (const py::PyError&) {
                answer.reset();
            }
        }
        const Json* echoed = nullptr;
        if (answer && answer->is_object()) {
            if (const auto found = answer->find("challenge"); found != answer->end()) {
                echoed = &*found;
            }
        }
        if (echoed != nullptr && echoed->is_string() &&
            constant_time_equal(echoed->get_ref<const std::string&>(), challenge)) {
            std::lock_guard lock(mutex_);
            verified_[url] = clock_.steady() + seconds(VERIFY_CACHE_S);
            return;
        }
    } else if (reply.category == "payload_too_large") {
        reason = "http_4xx";
    } else if (std::ranges::find(CALLBACK_REASONS, reply.category) != CALLBACK_REASONS.end()) {
        reason = reply.category;
    }
    kLog.info("MCP Events callback verification failed for {}: {}", subscription_id, reason);
    throw callback_failed(peer_address ? PEER_VERIFICATION_FAILED : std::string_view(reason));
}

Deliverer::Deliverer(Deliver deliver, const Clock& clock, std::size_t max_queued,
                     std::size_t workers)
    : deliver_(std::move(deliver)), clock_(clock), max_queued_(max_queued), workers_(workers) {}

Deliverer::~Deliverer() {
    close();
}

bool Deliverer::push(std::string subscription_id, std::string event_id, std::string event_type,
                     std::shared_ptr<const std::string> body, int attempt, double delay_s) {
    std::lock_guard lock(mutex_);
    if (closed_ || heap_.size() >= max_queued_) {
        return false;
    }
    Job job;
    job.due = clock_.steady() + seconds(delay_s);
    job.order = order_++;
    job.subscription_id = std::move(subscription_id);
    job.event_id = std::move(event_id);
    job.event_type = std::move(event_type);
    job.body = std::move(body);
    job.attempt = attempt;
    heap_.push(std::move(job));
    if (threads_.size() < workers_) {
        threads_.emplace_back([this] { run(); });
    }
    changed_.notify_one();
    return true;
}

std::size_t Deliverer::pending() const {
    std::lock_guard lock(mutex_);
    return heap_.size();
}

void Deliverer::close() {
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        heap_ = {};
        threads.swap(threads_);
    }
    cancel_.cancel();
    changed_.notify_all();
    for (auto& thread : threads) {
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }
}

std::optional<Job> Deliverer::next() {
    std::unique_lock lock(mutex_);
    while (!closed_) {
        if (heap_.empty()) {
            changed_.wait(lock);
            continue;
        }
        const auto wait = heap_.top().due - clock_.steady();
        if (wait <= std::chrono::steady_clock::duration::zero()) {
            Job job = heap_.top();
            heap_.pop();
            return job;
        }
        changed_.wait_for(lock, std::min<std::chrono::steady_clock::duration>(wait, kLongestWait));
    }
    return std::nullopt;
}

void Deliverer::run() {
    while (auto job = next()) {
        std::optional<double> delay;
        try {
            delay = deliver_(*job, cancel_.token());
        } catch (const std::exception& error) {
            kLog.warning("MCP Events delivery failed: {}", error_name(error));
            continue;
        }
        if (delay) {
            push(job->subscription_id, job->event_id, job->event_type, job->body, job->attempt + 1,
                 *delay);
        }
    }
}

} // namespace fv::agentapi::events_ext
