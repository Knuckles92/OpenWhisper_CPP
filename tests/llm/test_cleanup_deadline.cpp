// tests/test_cleanup_deadline.py: cleanup must not hold the paste or the job slot past its
// budget. The HTTP timeout bounds each wait for bytes, not the request: a provider that answers
// 200 at once and then trickles its body resets it on every read. These run against a loopback
// server with a fake wall clock, so the budget's seconds pass without being waited.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "../http/loopback_server.h"
#include "base/testing/fake_clock.h"
#include "generation_test_support.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "llm/openai_client.h"
#include "llm/transcript_cleanup.h"
#include "llm_test_support.h"
#include "pyport/json.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::LoopbackServer;
using namespace std::chrono_literals;

namespace {

const std::string kCompletion = py::dumps(
    Json{{"id", "x"},
         {"object", "chat.completion"},
         {"created", 0},
         {"model", "m"},
         {"choices",
          Json::array({Json{{"index", 0},
                            {"finish_reason", "stop"},
                            {"message", {{"role", "assistant"}, {"content", "Cleaned."}}}}})}});

std::shared_ptr<http::Client> direct_client() {
    return http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
}

// A chat endpoint that answers 200 at once, then one space per 200 ms; the body completes
// after 5 s, so a regression fails instead of hanging.
LoopbackServer::Handler trickling() {
    return [](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        const auto finish_at = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < finish_at && reply.wait(200ms)) {
            reply.write("1\r\n \r\n");
        }
        std::ostringstream chunk;
        chunk << std::hex << kCompletion.size() << "\r\n" << kCompletion << "\r\n0\r\n\r\n";
        reply.write(chunk.str());
    };
}

// A server that takes the request and never answers (until it stops).
LoopbackServer::Handler silent() {
    return [](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        while (reply.wait(200ms)) {
        }
    };
}

// tests/test_cleanup_deadline.py _cleaner: OpenAI with a client the test points at `url`.
std::unique_ptr<TranscriptCleanup> cleaner_at(std::shared_ptr<http::Client> http,
                                              const std::string& url, const Clock* clock,
                                              TestKeys& keys) {
    CleanupOptions options;
    options.api_key = "k";
    options.clock = clock;
    options.client_factory = [http, url](const TextLLMProfile&, double timeout,
                                         const std::string& key) {
        return OpenAiCompatClient(http, url, key, {}, timeout, kTranscriptCleanupMaxRetries);
    };
    return std::make_unique<TranscriptCleanup>(
        http, keys.sources, [] { return Json::object(); }, "openai", "gpt-test", "", false,
        std::move(options));
}

bool wait_for_requests(const LoopbackServer& server, std::size_t count) {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
        if (server.requests().size() >= count) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// An http::Client whose requests run until their cancel token or release().
class StallingClient final : public http::Client {
public:
    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken& cancel,
                                                    http::BodySink*) override {
        {
            std::lock_guard lock(mutex_);
            started_ = true;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 10s, [&] { return released_ || cancel.cancelled(); });
        return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
    }
    bool wait_started() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return started_; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool started_ = false;
    bool released_ = false;
};

http::FakeHttpClient::Handler answering(const std::string& text) {
    return [text](const http::Request& request) {
        if (url_path(request.url).ends_with("/responses")) {
            return json_reply(
                200, Json{{"status", "completed"},
                          {"output",
                           Json::array({Json{{"type", "message"},
                                             {"content", Json::array({Json{{"type", "output_text"},
                                                                           {"text", text}}})}}})}});
        }
        return json_reply(
            200, Json{{"choices",
                       Json::array({Json{{"message", {{"role", "assistant"}, {"content", text}}},
                                         {"finish_reason", "stop"}}})}});
    };
}

} // namespace

TEST_CASE("trickling response ends at the attempts budget", "[test_cleanup_deadline]") {
    // from tests/test_cleanup_deadline.py::test_trickling_response_ends_at_the_attempts_budget
    LoopbackServer server(trickling());
    FakeClock clock;
    TestKeys keys;
    auto cleaner = cleaner_at(direct_client(), server.url("/v1"), &clock, keys);
    auto result =
        std::async(std::launch::async, [&] { return cleaner->cleanup("um raw text", "Tidy."); });
    REQUIRE(wait_for_requests(server, 1));
    // Two 8 s attempts plus a second of backoff: every read arrives well inside 8 s, so
    // without the deadline this waits for the server's final body.
    clock.advance(16900ms);
    CHECK(result.wait_for(300ms) == std::future_status::timeout);
    clock.advance(100ms);
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    CHECK(result.get() == "um raw text");
    CHECK(cleaner->last_error() == "timed out after 17 s");
    CHECK(server.requests().size() == 1);
}

TEST_CASE("a hosted cleanup whose server never answers gives up at seventeen seconds",
          "[cleanup_deadline]") {
    LoopbackServer server(silent());
    FakeClock clock;
    TestKeys keys;
    auto cleaner = cleaner_at(direct_client(), server.url("/v1"), &clock, keys);
    REQUIRE(cleaner->attempt_timeout_s() == 8.0);
    auto result =
        std::async(std::launch::async, [&] { return cleaner->cleanup("raw text", "Tidy."); });
    REQUIRE(wait_for_requests(server, 1));
    clock.advance(16999ms);
    CHECK(result.wait_for(300ms) == std::future_status::timeout);
    clock.advance(1ms);
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    CHECK(result.get() == "raw text");
    CHECK(cleaner->last_error() == "timed out after 17 s");
}

TEST_CASE("a deadline ends the call without a second slow attempt", "[test_cleanup_deadline]") {
    // from
    // tests/test_cleanup_deadline.py::test_a_deadline_ends_the_call_without_a_second_slow_attempt
    // Rewrites pass deadline_s == timeout_s: a slow model is asked once.
    LoopbackServer server(trickling());
    FakeClock clock;
    TestKeys keys;
    auto cleaner = cleaner_at(direct_client(), server.url("/v1"), &clock, keys);
    auto result = std::async(std::launch::async,
                             [&] { return cleaner->cleanup("raw text", "Tidy.", 0.5, 0.5); });
    REQUIRE(wait_for_requests(server, 1));
    clock.advance(400ms);
    CHECK(result.wait_for(300ms) == std::future_status::timeout);
    clock.advance(100ms);
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    CHECK(result.get() == "raw text");
    CHECK(cleaner->last_error()->starts_with("timed out after"));
    // Without the deadline, timeout_s alone allows three attempts.
    CHECK(server.requests().size() == 1);
}

TEST_CASE("cancel ends the wait at once", "[test_cleanup_deadline]") {
    // from tests/test_cleanup_deadline.py::test_cancel_ends_the_wait_at_once
    auto stalling = std::make_shared<StallingClient>();
    TestKeys keys;
    auto cleaner = cleaner_at(stalling, "http://127.0.0.1:9/v1", nullptr, keys);
    CancelSource cancel;
    auto result = std::async(std::launch::async, [&] {
        return cleaner->cleanup("raw text", "Tidy.", std::nullopt, std::nullopt, cancel.token());
    });
    REQUIRE(stalling->wait_started());
    const auto pressed = std::chrono::steady_clock::now();
    cancel.cancel();
    REQUIRE(result.wait_for(1s) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - pressed < 500ms);
    stalling->release();
    CHECK(result.get() == "raw text");
    CHECK(cleaner->last_error() == std::string(kCanceledReason));
}

TEST_CASE("a cancel set before the call sends nothing", "[test_cleanup_deadline]") {
    // from tests/test_cleanup_deadline.py::test_a_cancel_set_before_the_call_sends_nothing
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->set_handler(answering("Too late."));
    TestKeys keys;
    auto cleaner = cleaner_at(fake, "http://127.0.0.1:9/v1", nullptr, keys);
    CancelSource cancel;
    cancel.cancel();
    CHECK(cleaner->cleanup("raw text", "Tidy.", std::nullopt, std::nullopt, cancel.token()) ==
          "raw text");
    CHECK(fake->requests().empty());
    CHECK(cleaner->last_error() == std::string(kCanceledReason));
}

TEST_CASE("response inside the budget is used", "[test_cleanup_deadline]") {
    // from tests/test_cleanup_deadline.py::test_response_inside_the_budget_is_used
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->set_handler(answering(" Cleaned. "));
    TestKeys keys;
    auto cleaner = cleaner_at(fake, "http://127.0.0.1:9/v1", nullptr, keys);
    CancelSource cancel;
    CHECK(cleaner->cleanup("um cleaned", "Tidy.", std::nullopt, std::nullopt, cancel.token()) ==
          "Cleaned.");
    CHECK_FALSE(cleaner->last_error());
}

TEST_CASE("request errors still reach last error", "[test_cleanup_deadline]") {
    // from tests/test_cleanup_deadline.py::test_request_errors_still_reach_last_error
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->set_handler([](const http::Request&) { return text_reply(400, "provider said no"); });
    TestKeys keys;
    auto cleaner = cleaner_at(fake, "http://127.0.0.1:9/v1", nullptr, keys);
    CHECK(cleaner->cleanup("raw text", "Tidy.") == "raw text");
    CHECK(cleaner->last_error() == "provider said no");
}
