// Sweep B21: run_cancellable's fallback when no thread can be started, and a judgment over the
// keep-alive pool going out at most twice, as _http_post sends it (services/typesafe.py
// 310-360): curl resending on a fresh connection by itself already is that second send. Fake
// connections only; nothing leaves the process.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include "base/cancel.h"
#include "http/testing/fake_http_client.h"
#include "llm/detail/keep_alive.h"
#include "llm/judge.h"
#include "llm_test_support.h"
#include "pyport/json.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using namespace std::chrono_literals;

namespace {

const Json kGood = Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 0.9}}}}}};

struct Rig {
    explicit Rig(JudgeHooks hooks = {}) {
        hooks.proxy_for = [](std::string_view) {
            return std::optional<std::string>();
        };
        hooks.new_connection = [client = connection]() -> std::shared_ptr<http::Client> {
            return client;
        };
        env = std::make_unique<JudgeEnvironment>(one_shot, keys.store, keys.env, keys.dotenv,
                                                 std::move(hooks));
    }

    std::expected<llm::detail::PostReply, http::Error> post() {
        return llm::detail::http_post(env->pool(), *one_shot, env->proxy_lookup(), Json{{"m", 1}},
                                      5.0, "k", "https://judge.test/v1/answer", {});
    }

    TestKeys keys;
    std::shared_ptr<http::FakeHttpClient> one_shot = std::make_shared<http::FakeHttpClient>();
    // Every pooled connection is this one fake, which counts what went out.
    std::shared_ptr<http::FakeHttpClient> connection = std::make_shared<http::FakeHttpClient>();
    std::unique_ptr<JudgeEnvironment> env;
};

http::ScriptedResponse failing(http::ErrorKind kind, std::string message, int sends) {
    http::ScriptedResponse reply;
    http::Error error{kind, std::move(message)};
    error.after_request = true;
    error.sends = sends;
    reply.error = error;
    return reply;
}

JudgeHooks no_threads() {
    JudgeHooks hooks;
    hooks.start_thread = [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    };
    return hooks;
}

} // namespace

TEST_CASE("run_cancellable runs the work on the caller's thread when no thread can start",
          "[sweep][judge]") {
    Rig rig(no_threads());
    std::optional<std::thread::id> ran_on;
    const bool finished = rig.env->run_cancellable(
        [&](const CancelToken& token) {
            CHECK_FALSE(token.cancelled());
            ran_on = std::this_thread::get_id();
        },
        {});
    CHECK(finished);
    CHECK(ran_on == std::this_thread::get_id());
}

TEST_CASE("run_cancellable reports a cancel that came while the work ran inline",
          "[sweep][judge]") {
    Rig rig(no_threads());
    CancelSource source;
    const bool finished =
        rig.env->run_cancellable([&](const CancelToken&) { source.cancel(); }, source.token());
    CHECK_FALSE(finished);
}

TEST_CASE("run_cancellable with a deadline fails at once when no thread can start",
          "[sweep][judge]") {
    Rig rig(no_threads());
    bool ran = false;
    const bool finished = rig.env->run_cancellable([&](const CancelToken&) { ran = true; }, {},
                                                   std::chrono::steady_clock::now() + 10s);
    CHECK_FALSE(finished);
    CHECK_FALSE(ran);
}

TEST_CASE("a judgment curl already resent on a fresh connection is not sent a third time",
          "[sweep][judge]") {
    Rig rig;
    rig.connection->push(json_reply(200, kGood));
    REQUIRE(rig.post()->status == 200);
    // The reused connection died, curl sent the request again on a fresh one, and that failed
    // too: two sends, so the pool gives up.
    rig.connection->push(failing(http::ErrorKind::Connect, "Connection reset by peer", 2));
    rig.connection->push(json_reply(200, kGood));
    const auto reply = rig.post();
    REQUIRE_FALSE(reply.has_value());
    CHECK(rig.connection->requests().size() == 2);
    CHECK(rig.env->pool().discards() == 0);
}

TEST_CASE("a judgment dropped once on a reused connection is resent once", "[sweep][judge]") {
    Rig rig;
    rig.connection->push(json_reply(200, kGood));
    REQUIRE(rig.post()->status == 200);
    // A silent connection (curl does not resend a timeout), then a failure on the fresh one.
    rig.connection->push(failing(http::ErrorKind::Timeout, "timed out", 1));
    rig.connection->push(failing(http::ErrorKind::Connect, "Connection reset by peer", 1));
    rig.connection->push(json_reply(200, kGood));
    const auto reply = rig.post();
    REQUIRE_FALSE(reply.has_value());
    CHECK(rig.connection->requests().size() == 3);
    CHECK(rig.env->pool().discards() == 1);
}

TEST_CASE("a TLS end of stream on a reused connection is resent, as SSLEOFError is",
          "[sweep][judge]") {
    Rig rig;
    rig.connection->push(json_reply(200, kGood));
    REQUIRE(rig.post()->status == 200);
    rig.connection->push(
        failing(http::ErrorKind::Tls, "OpenSSL SSL_read: unexpected eof while reading", 1));
    rig.connection->push(json_reply(200, kGood));
    const auto reply = rig.post();
    REQUIRE(reply.has_value());
    CHECK(reply->status == 200);
    CHECK(rig.connection->requests().size() == 3);
    // Any other TLS failure is not.
    rig.connection->push(failing(http::ErrorKind::Tls, "SSL certificate problem", 1));
    CHECK_FALSE(rig.post().has_value());
    CHECK(rig.connection->requests().size() == 4);
}
