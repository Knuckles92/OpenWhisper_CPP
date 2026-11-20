// The OpenAI SDK's request loop as OpenAiCompatClient keeps it (retries, waits, error classes),
// and the retry caps of the cleanup path from tests/test_network_reuse.py. Every request goes to
// an in-process fake.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "generation_test_support.h"
#include "http/testing/fake_http_client.h"
#include "llm/cleanup_settings.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "llm/transcript_cleanup.h"
#include "llm_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using namespace std::chrono_literals;

namespace {

http::ScriptedResponse status_reply(int status, http::Headers headers = {}) {
    auto reply = json_reply(status, Json{{"error", {{"message", "synthetic"}}}});
    for (auto& header : headers) {
        reply.headers.push_back(std::move(header));
    }
    return reply;
}

http::ScriptedResponse ok_reply() {
    return json_reply(200, Json{{"ok", true}});
}

struct Rig {
    explicit Rig(int max_retries = 2)
        : client(fake, "http://127.0.0.1:9/v1", "k", {}, 8.0, max_retries) {
        client.set_hooks({[this](std::chrono::milliseconds delay, const CancelToken&) {
                              waits.push_back(delay);
                              if (on_wait) {
                                  on_wait();
                              }
                          },
                          [] { return 0.0; }, &clock});
    }
    std::shared_ptr<http::FakeHttpClient> fake = std::make_shared<http::FakeHttpClient>();
    FakeClock clock;
    std::vector<std::chrono::milliseconds> waits;
    std::function<void()> on_wait;
    OpenAiCompatClient client;
};

http::Headers headers_of(std::initializer_list<std::pair<const char*, const char*>> items) {
    http::Headers out;
    for (const auto& [name, value] : items) {
        out.emplace_back(name, value);
    }
    return out;
}

} // namespace

TEST_CASE("408, 409, 429 and 5xx answers are retried", "[openai_client]") {
    for (const int status : {408, 409, 429, 500, 502, 503, 504}) {
        INFO(status);
        Rig rig;
        rig.fake->push(status_reply(status));
        rig.fake->push(ok_reply());
        CHECK(rig.client.post_json("/chat/completions", Json::object()) == Json{{"ok", true}});
        CHECK(rig.fake->requests().size() == 2);
    }
}

TEST_CASE("400, 401, 403, 404 and 422 answers are not retried", "[openai_client]") {
    const std::vector<std::pair<int, std::string>> table{
        {400, "BadRequestError"}, {401, "AuthenticationError"},      {403, "PermissionDeniedError"},
        {404, "NotFoundError"},   {422, "UnprocessableEntityError"}, {418, "APIStatusError"}};
    for (const auto& [status, kind] : table) {
        INFO(status);
        Rig rig;
        rig.fake->push(status_reply(status));
        rig.fake->push(ok_reply());
        try {
            (void)rig.client.post_json("/chat/completions", Json::object());
            FAIL("no error");
        } catch (const ApiError& error) {
            CHECK(error.kind == kind);
            CHECK(error.status == status);
            CHECK(std::string(error.what()) == "Error code: " + std::to_string(status) +
                                                   " - {'error': {'message': 'synthetic'}}");
        }
        CHECK(rig.fake->requests().size() == 1);
    }
}

TEST_CASE("the last failed answer is reported once the retries are spent", "[openai_client]") {
    Rig rig(1);
    rig.fake->push(status_reply(500));
    rig.fake->push(status_reply(502));
    try {
        (void)rig.client.post_json("/chat/completions", Json::object());
        FAIL("no error");
    } catch (const ApiError& error) {
        CHECK(error.kind == "InternalServerError");
        CHECK(error.status == 502);
    }
    CHECK(rig.fake->requests().size() == 2);
}

TEST_CASE("x-should-retry decides when the server sends it", "[openai_client]") {
    Rig yes;
    yes.fake->push(status_reply(400, headers_of({{"x-should-retry", "true"}})));
    yes.fake->push(ok_reply());
    CHECK(yes.client.post_json("/x", Json::object()) == Json{{"ok", true}});
    CHECK(yes.fake->requests().size() == 2);

    Rig no;
    no.fake->push(status_reply(503, headers_of({{"X-Should-Retry", "false"}})));
    CHECK_THROWS_AS(no.client.post_json("/x", Json::object()), ApiError);
    CHECK(no.fake->requests().size() == 1);

    Rig other;
    other.fake->push(status_reply(400, headers_of({{"x-should-retry", "maybe"}})));
    CHECK_THROWS_AS(other.client.post_json("/x", Json::object()), ApiError);
    CHECK(other.fake->requests().size() == 1);
}

TEST_CASE("a server asking for more than two minutes is not retried", "[openai_client]") {
    Rig rig;
    const auto answer = [](const char* after) {
        http::Response response;
        response.status = 429;
        response.headers = {{"retry-after", after}};
        return response;
    };
    CHECK_FALSE(rig.client.should_retry(answer("121")));
    CHECK(rig.client.should_retry(answer("120")));
    CHECK(rig.client.should_retry(answer("not a number")));
    http::Response ms;
    ms.status = 429;
    ms.headers = {{"retry-after-ms", "120001"}};
    CHECK_FALSE(rig.client.should_retry(ms));
    ms.headers = {{"retry-after-ms", "inf"}};
    CHECK(rig.client.should_retry(ms));

    rig.fake->push(status_reply(429, headers_of({{"retry-after", "300"}})));
    CHECK_THROWS_AS(rig.client.post_json("/x", Json::object()), ApiError);
    CHECK(rig.fake->requests().size() == 1);
}

TEST_CASE("retry-after-ms, then retry-after seconds or date, sets the wait", "[openai_client]") {
    Rig rig;
    CHECK(rig.client.retry_delay(0, headers_of({{"retry-after-ms", "1500"}})) == 1500ms);
    CHECK(rig.client.retry_delay(0, headers_of({{"Retry-After-Ms", " 250.5 "}})) == 251ms);
    CHECK(rig.client.retry_delay(0, headers_of({{"retry-after", "3"}})) == 3000ms);
    CHECK(rig.client.retry_delay(0, headers_of({{"retry-after", "2.5"}})) == 2500ms);
    // retry-after-ms wins over retry-after; one that does not parse falls through to it.
    CHECK(rig.client.retry_delay(
              0, headers_of({{"retry-after-ms", "100"}, {"retry-after", "9"}})) == 100ms);
    CHECK(rig.client.retry_delay(
              0, headers_of({{"retry-after-ms", "soon"}, {"retry-after", "9"}})) == 9000ms);
    // An HTTP date counts from the clock's now (2026-01-01T00:00:00Z).
    CHECK(rig.client.retry_delay(
              0, headers_of({{"retry-after", "Thu, 01 Jan 2026 00:00:10 GMT"}})) == 10000ms);
    CHECK(rig.client.retry_delay(
              0, headers_of({{"retry-after", "Thursday, 01-Jan-26 00:01:00 GMT"}})) == 60000ms);
    CHECK(rig.client.retry_delay(0, headers_of({{"retry-after", "Thu Jan  1 00:00:05 2026"}})) ==
          5000ms);
    CHECK(rig.client.retry_delay(
              0, headers_of({{"retry-after", "Wed, 31 Dec 2025 19:00:20 -0500"}})) == 20000ms);
    // Outside (0, 120] s the backoff applies.
    for (const char* value : {"0", "-3", "121", "Wed, 31 Dec 2025 23:00:00 GMT", "inf", "nan"}) {
        INFO(value);
        CHECK(rig.client.retry_delay(0, headers_of({{"retry-after", value}})) == 500ms);
    }
    CHECK(rig.client.retry_delay(0, headers_of({{"retry-after", "120"}})) == 120000ms);
}

TEST_CASE("the backoff doubles from half a second to eight with up to a quarter off",
          "[openai_client]") {
    auto fake = std::make_shared<http::FakeHttpClient>();
    OpenAiCompatClient client(fake, "", "k");
    double random = 0.0;
    client.set_hooks({nullptr, [&random] { return random; }, nullptr});
    const std::vector<std::chrono::milliseconds> full{500ms,  1000ms, 2000ms,
                                                      4000ms, 8000ms, 8000ms};
    for (int retries = 0; retries < 6; ++retries) {
        CHECK(client.retry_delay(retries, {}) == full[static_cast<std::size_t>(retries)]);
    }
    random = 0.5;
    const std::vector<std::chrono::milliseconds> jittered{438ms, 875ms, 1750ms, 3500ms, 7000ms};
    for (int retries = 0; retries < 5; ++retries) {
        CHECK(client.retry_delay(retries, {}) == jittered[static_cast<std::size_t>(retries)]);
    }
    random = 0.999999;
    CHECK(client.retry_delay(4, {}) == 6001ms);
}

TEST_CASE("each retry waits the computed time", "[openai_client]") {
    Rig rig;
    rig.fake->push(status_reply(500));
    rig.fake->push(status_reply(429, headers_of({{"retry-after-ms", "7"}})));
    rig.fake->push(ok_reply());
    CHECK(rig.client.post_json("/x", Json::object()) == Json{{"ok", true}});
    CHECK(rig.waits == std::vector<std::chrono::milliseconds>{500ms, 7ms});
}

TEST_CASE("connection failures and timeouts are retried, then reported", "[openai_client]") {
    Rig connect(1);
    http::ScriptedResponse refused;
    refused.error = http::Error{http::ErrorKind::Connect, "refused"};
    connect.fake->push(refused);
    connect.fake->push(refused);
    try {
        (void)connect.client.post_json("/x", Json::object());
        FAIL("no error");
    } catch (const ApiError& error) {
        CHECK(error.kind == "APIConnectionError");
        CHECK(error.status == 0);
        CHECK(std::string(error.what()) == "Connection error.");
    }
    CHECK(connect.fake->requests().size() == 2);
    CHECK(connect.waits == std::vector<std::chrono::milliseconds>{500ms});

    Rig timeout(2);
    http::ScriptedResponse slow;
    slow.error = http::Error{http::ErrorKind::Timeout, "timed out"};
    timeout.fake->push(slow);
    timeout.fake->push(slow);
    timeout.fake->push(slow);
    try {
        (void)timeout.client.post_json("/x", Json::object());
        FAIL("no error");
    } catch (const ApiError& error) {
        CHECK(error.kind == "APITimeoutError");
        CHECK(std::string(error.what()) == "Request timed out.");
    }
    CHECK(timeout.fake->requests().size() == 3);

    Rig recovered(1);
    recovered.fake->push(refused);
    recovered.fake->push(ok_reply());
    CHECK(recovered.client.get_json("/models") == Json{{"ok", true}});
}

TEST_CASE("a cancel between retries ends the call without another attempt", "[openai_client]") {
    Rig rig;
    CancelSource cancel;
    rig.on_wait = [&] {
        cancel.cancel();
    };
    rig.fake->push(status_reply(500));
    rig.fake->push(ok_reply());
    CHECK_THROWS_AS(rig.client.post_json("/x", Json::object(), {}, cancel.token()),
                    RequestCanceled);
    CHECK(rig.fake->requests().size() == 1);

    Rig early;
    CancelSource before;
    before.cancel();
    CHECK_THROWS_AS(early.client.post_json("/x", Json::object(), {}, before.token()),
                    RequestCanceled);
    CHECK(early.fake->requests().empty());
}

TEST_CASE("requests carry the key, JSON headers, provider headers and the timeouts",
          "[openai_client]") {
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(ok_reply());
    auto client =
        create_openai_client(fake, *get_profile("openrouter", Json::object()), 12.5, "sk-1");
    (void)client.post_json("/chat/completions", Json{{"model", "m"}},
                           RequestOptions{std::nullopt, std::nullopt, {{"X-Title", "Other"}}});
    const auto sent = fake->requests().at(0);
    CHECK(sent.method == "POST");
    CHECK(sent.url == "https://openrouter.ai/api/v1/chat/completions");
    CHECK(http::find_header(sent.headers, "authorization") == "Bearer sk-1");
    CHECK(http::find_header(sent.headers, "accept") == "application/json");
    CHECK(http::find_header(sent.headers, "content-type") == "application/json");
    // A per-request header replaces the client's of the same name.
    CHECK(http::find_header(sent.headers, "x-title") == "Other");
    CHECK(std::ranges::count_if(sent.headers,
                                [](const auto& h) { return lower(h.first) == "x-title"; }) == 1);
    CHECK(sent.idle_timeout == 12500ms);
    CHECK(sent.connect_timeout == 12500ms);
    CHECK(sent.follow_redirects);
    CHECK(request_body(sent) == Json{{"model", "m"}});

    fake->push(ok_reply());
    (void)client.post_json("/x", Json::object(), RequestOptions{30.0});
    CHECK(fake->requests().at(1).idle_timeout == 30000ms);
    CHECK(http::find_header(fake->requests().at(1).headers, "x-title") ==
          std::string(identity().product));
}

TEST_CASE("paths join the base URL as the SDK joins them", "[openai_client]") {
    auto fake = std::make_shared<http::FakeHttpClient>();
    CHECK(OpenAiCompatClient(fake, "http://h:1/v1", "k").url_for("/chat/completions") ==
          "http://h:1/v1/chat/completions");
    CHECK(OpenAiCompatClient(fake, "http://h:1/v1/", "k").url_for("models") ==
          "http://h:1/v1/models");
    CHECK(OpenAiCompatClient(fake, "", "k").url_for("/responses") ==
          "https://api.openai.com/v1/responses");
    CHECK(OpenAiCompatClient(fake, "http://h", "k").url_for("/messages") == "http://h/messages");
}

TEST_CASE("an answer that is not JSON by its type is its text unless it parses",
          "[openai_client]") {
    Rig rig;
    rig.fake->push(text_reply(200, "plain words"));
    CHECK(rig.client.get_json("/x") == Json("plain words"));
    rig.fake->push(text_reply(200, "{\"a\": 1}"));
    CHECK(rig.client.get_json("/x") == Json{{"a", 1}});
    auto broken = json_reply(200, Json::object());
    broken.body = "{not json";
    rig.fake->push(broken);
    CHECK_THROWS_AS(rig.client.get_json("/x"), py::JsonError);
}

TEST_CASE("a client without a key cannot be made", "[openai_client]") {
    TestKeys keys;
    const auto openai = *get_profile("openai", Json::object());
    auto fake = std::make_shared<http::FakeHttpClient>();
    try {
        (void)create_openai_client(fake, openai, 15.0, {}, {}, std::nullopt, &keys.sources);
        FAIL("made a client");
    } catch (const py::PyError& error) {
        CHECK(error.type_name() == "RuntimeError");
        CHECK(std::string(error.what()) == "No API key found for OpenAI (set OPENAI_API_KEY)");
    }
    keys.env.set("OPENAI_API_KEY", "sk-env");
    auto client = create_openai_client(fake, openai, 15.0, {}, {}, std::nullopt, &keys.sources);
    fake->push(ok_reply());
    (void)client.get_json("/models");
    CHECK(http::find_header(fake->requests().at(0).headers, "authorization") == "Bearer sk-env");

    // An endpoint that needs no key gets the placeholder.
    auto local = create_openai_client(fake, *get_profile("ollama", Json::object()));
    fake->push(ok_reply());
    (void)local.get_json("/models");
    CHECK(http::find_header(fake->requests().at(1).headers, "authorization") == "Bearer dummy");
}

namespace {

// tests/test_network_reuse.py TestCleanupRetryCap._cleaner: every attempt answers 500 with a
// one-millisecond retry-after.
struct RetryCapRig {
    RetryCapRig() {
        fake->set_handler([](const http::Request&) {
            return status_reply(500, headers_of({{"retry-after-ms", "1"}}));
        });
        cleaner = std::make_unique<TranscriptCleanup>(
            fake, keys.sources, [] { return Json::object(); }, "openai", "gpt-test", "", false,
            CleanupOptions{"test-key"});
    }
    std::shared_ptr<http::FakeHttpClient> fake = std::make_shared<http::FakeHttpClient>();
    TestKeys keys;
    std::unique_ptr<TranscriptCleanup> cleaner;
};

} // namespace

TEST_CASE("worst case dictation wait is two timeouts", "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestCleanupRetryCap::test_worst_case_dictation_wait_is_two_timeouts
    STATIC_CHECK(kTranscriptCleanupMaxRetries == 1);
    constexpr int attempts = kTranscriptCleanupMaxRetries + 1;
    STATIC_CHECK(attempts * kTranscriptCleanupTimeoutS <= 16.0);
    STATIC_CHECK(kTranscriptBatchCleanupMaxRetries == 2);
}

TEST_CASE("dictation cleanup makes at most two attempts", "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestCleanupRetryCap::test_dictation_cleanup_makes_at_most_two_attempts
    RetryCapRig rig;
    CHECK(rig.cleaner->cleanup("raw text", "Tidy.") == "raw text");
    CHECK(rig.cleaner->last_error());
    const auto attempts = rig.fake->requests();
    CHECK(attempts.size() == 2);
    CHECK(attempts.at(0).idle_timeout ==
          std::chrono::milliseconds(static_cast<int>(kTranscriptCleanupTimeoutS * 1000)));
}

TEST_CASE("batch cleanup keeps two retries and its timeout", "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestCleanupRetryCap::test_batch_cleanup_keeps_two_retries_and_its_timeout
    constexpr double kBatchTimeoutS = 120.0; // config.TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S
    RetryCapRig rig;
    CHECK(rig.cleaner->cleanup("raw text", "Tidy.", kBatchTimeoutS) == "raw text");
    auto attempts = rig.fake->requests();
    CHECK(attempts.size() == 3);
    CHECK(attempts.at(0).idle_timeout == 120000ms);
    // The override leaves the dictation client alone.
    CHECK(rig.cleaner->cleanup("raw text", "Tidy.") == "raw text");
    attempts = rig.fake->requests();
    CHECK(attempts.size() == 5);
    CHECK(attempts.back().idle_timeout == 8000ms);
}

TEST_CASE("batch override shares the connection pool", "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestCleanupRetryCap::test_batch_override_shares_the_connection_pool
    // As: the per-request override is a copy over the same http::Client.
    auto fake = std::make_shared<http::FakeHttpClient>();
    const OpenAiCompatClient client(fake, "", "k", {}, kTranscriptCleanupTimeoutS,
                                    kTranscriptCleanupMaxRetries);
    const auto batch = client.with_options(std::nullopt, kTranscriptBatchCleanupMaxRetries);
    CHECK(batch.http_client() == client.http_client());
    CHECK(batch.max_retries() == 2);
    CHECK(client.max_retries() == 1);
    CHECK(batch.timeout_s() == client.timeout_s());
}

TEST_CASE("other clients keep the sdk default", "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestCleanupRetryCap::test_other_clients_keep_the_sdk_default
    auto fake = std::make_shared<http::FakeHttpClient>();
    const auto client = create_openai_client(fake, *builtin_profile("openai"), 15.0, "test-key");
    CHECK(client.max_retries() == kSdkDefaultMaxRetries);
    CHECK(kSdkDefaultMaxRetries == 2);
}
