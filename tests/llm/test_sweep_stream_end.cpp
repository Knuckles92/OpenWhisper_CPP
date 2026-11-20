// Sweep B22: a Chat stream that ends with [DONE] but no finish_reason, a Responses
// response.failed naming a server error, a retry-after date read on the client's own clock, and
// a cancelled transfer joined as soon as it ends rather than at the next cancel. In-memory
// providers only.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/cancel.h"
#include "base/testing/fake_clock.h"
#include "generation_test_support.h"
#include "http/testing/fake_http_client.h"
#include "llm/detail/stream_protocol.h"
#include "llm/streaming.h"
#include "llm_test_support.h"
#include "pyport/json.h"
#include "streaming_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using namespace std::chrono_literals;
using fv::testing::SseReply;

namespace {

constexpr const char* kBase = "http://provider.test/v1";

http::ScriptedResponse sse(const std::vector<Json>& frames) {
    http::ScriptedResponse reply;
    reply.headers = {{"Content-Type", "text/event-stream"}};
    for (const auto& frame : frames) {
        reply.body += frame.is_string() ? frame.get<std::string>() : SseReply::frame(frame);
    }
    return reply;
}

Json chunk(const Json& delta, const Json& reason = nullptr) {
    return Json{{"choices",
                 Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}})}};
}

GenerateRequest request_of(CancelToken cancel = {}) {
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    request.cancel = std::move(cancel);
    return request;
}

std::optional<StreamError> stream_error_of(const std::string& protocol,
                                           http::ScriptedResponse reply,
                                           const Clock* clock = nullptr) {
    const TextLLMProfile profile = synthetic_profile(protocol, kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(std::move(reply));
    auto client = quick_client(fake, profile);
    if (clock != nullptr) {
        client.set_hooks({[](std::chrono::milliseconds, const CancelToken&) {}, nullptr, clock});
    }
    try {
        stream_generate(client, profile, request_of(), {});
    } catch (const StreamError& error) {
        return error;
    }
    return std::nullopt;
}

// Streams one delta, then holds the transfer until it is cancelled and returns only `linger`
// later, as curl does when it notices a cancel at its next progress callback.
class SlowToCancel final : public http::Client {
public:
    explicit SlowToCancel(std::chrono::milliseconds linger) : linger_(linger) {}

    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken& cancel,
                                                    http::BodySink* sink) override {
        http::Response head;
        head.status = 200;
        head.headers = {{"Content-Type", "text/event-stream"}};
        sink->on_head(head);
        const std::string frame = SseReply::frame(chunk(Json{{"content", "Hi"}}));
        sink->write(frame);
        while (!cancel.cancelled()) {
            std::this_thread::sleep_for(5ms);
        }
        std::this_thread::sleep_for(linger_);
        return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
    }

private:
    std::chrono::milliseconds linger_;
};

} // namespace

TEST_CASE("a chat stream ending with [DONE] but no finish_reason is a dropped stream",
          "[sweep][streaming]") {
    const auto error = stream_error_of(
        "chat", sse({chunk(Json{{"content", "Half a"}}), Json("data: [DONE]\n\n")}));
    REQUIRE(error);
    CHECK(std::string(error->what()) == kStreamDroppedError);
    CHECK(error->transient);
    CHECK(error->output_seen);
}

TEST_CASE("a chat stream with a finish_reason and no [DONE] is complete", "[sweep][streaming]") {
    const TextLLMProfile profile = synthetic_profile("chat", kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(sse({chunk(Json{{"content", "Done."}}, "stop")}));
    auto client = quick_client(fake, profile);
    CHECK(stream_generate(client, profile, request_of(), {}).text == "Done.");
}

TEST_CASE("response.failed for a server error is a transient stream failure",
          "[sweep][streaming]") {
    const Json failed{
        {"type", "response.failed"},
        {"response",
         {{"id", "resp_1"},
          {"status", "failed"},
          {"output", Json::array()},
          {"error", {{"code", "server_error"}, {"message", "The server had an error"}}}}}};
    const auto error = stream_error_of("responses", sse({failed}));
    REQUIRE(error);
    CHECK(std::string(error->what()) == kStreamFailedError);
    CHECK(error->status == 500);
    CHECK(error->transient);
    CHECK(error->provider_message == "The server had an error");

    Json limited = failed;
    limited["response"]["error"] = {{"code", "rate_limit_exceeded"}, {"message", "Slow down"}};
    const auto busy = stream_error_of("responses", sse({limited}));
    REQUIRE(busy);
    CHECK(busy->status == 429);
    CHECK(busy->transient);
}

TEST_CASE("response.failed without a known error code stays generate()'s failure",
          "[sweep][streaming]") {
    const Json failed{{"type", "response.failed"},
                      {"response",
                       {{"id", "resp_1"},
                        {"status", "failed"},
                        {"output", Json::array()},
                        {"error", {{"code", "invalid_prompt"}, {"message", "No."}}}}}};
    const TextLLMProfile profile = synthetic_profile("responses", kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(sse({failed}));
    auto client = quick_client(fake, profile);
    try {
        stream_generate(client, profile, request_of(), {});
        FAIL("a failed response raises");
    } catch (const StreamError&) {
        FAIL("not a stream failure");
    } catch (const TextGenerationError& error) {
        CHECK(std::string(error.what()) == "Model output was incomplete or failed.");
    }
}

TEST_CASE("a retry-after HTTP date is measured on the client's clock", "[sweep][streaming]") {
    // 2026-01-01T00:00:00Z on the fake clock; the date is 30 s later. Read on the system clock
    // it would lie in the past and ask for no wait.
    FakeClock clock;
    http::ScriptedResponse busy;
    busy.status = 429;
    busy.headers = {{"Content-Type", "application/json"},
                    {"Retry-After", "Thu, 01 Jan 2026 00:00:30 GMT"}};
    busy.body = R"({"error": {"message": "Too many requests"}})";
    const auto error = stream_error_of("chat", busy, &clock);
    REQUIRE(error);
    CHECK(error->status == 429);
    REQUIRE(error->retry_after_s);
    CHECK(*error->retry_after_s == 30.0);
}

TEST_CASE("a cancelled transfer is joined once it ends, without waiting for another cancel",
          "[sweep][streaming]") {
    const TextLLMProfile profile = synthetic_profile("chat", kBase);
    auto client = quick_client(std::make_shared<SlowToCancel>(300ms), profile);
    CancelSource source;
    const auto before = llm::detail::lingering_stream_transfers();
    try {
        stream_generate(client, profile, request_of(source.token()),
                        [&](const StreamEvent&) { source.cancel(); });
        FAIL("a cancel ends the stream");
    } catch (const TextGenerationError& error) {
        CHECK(std::string(error.what()) == "canceled");
    }
    // The transfer is still running (it returns 300 ms after the cancel) and was left behind.
    CHECK(llm::detail::lingering_stream_transfers() == before + 1);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (llm::detail::lingering_stream_transfers() > before &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(llm::detail::lingering_stream_transfers() == before);
}
