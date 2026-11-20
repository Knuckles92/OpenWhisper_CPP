// The client backend against a scripted host (MiniHost), for what a real host cannot be made to
// do: one too old to time its requests or to list its models, one that stays busy, and what
// exactly the client put on the wire. The same backend over a real host is
// tests/remote_host/test_remote_backend.cpp. Each case carries its OpenWhisper function's name.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <format>

#include "base/identity.h"
#include "client_test_support.h"
#include "engine/error.h"
#include "remote/connection.h"
#include "remote/remote_backend.h"

using namespace fv;
using namespace fv::remote;
using namespace fv::remote::test;
using namespace std::chrono_literals;
using Catch::Matchers::Matches;

namespace {

// A RemoteSpeechBackend paired by code with a MiniHost ("devbox", serving "Fake Parakeet").
struct ScriptedBackend {
    explicit ScriptedBackend(MiniHost::Options options = {})
        : host(rig.reactor, std::move(options)),
          approved(pair_with_host(rig.env, "127.0.0.1", host.port(), "246810", "laptop")),
          pairing([&] {
              ClientPairing saved;
              saved.host = "127.0.0.1";
              saved.port = host.port();
              saved.fingerprint = approved.fingerprint;
              saved.host_name = approved.host_name;
              saved.device_id = approved.device_id;
              return saved;
          }()),
          backend([&] { return std::optional(pairing); },
                  [&](const ClientPairing& peer) {
                      return std::make_unique<RemoteConnection>(rig.env, peer.host, peer.port,
                                                                approved.token, peer.fingerprint);
                  },
                  system_clock()) {}

    static speech::DecodeJob job(std::size_t samples, std::vector<std::string> phrases = {}) {
        auto audio = std::make_shared<const std::vector<float>>(tone(samples));
        speech::DecodeJob result;
        result.input = speech::AudioClip{audio, 0, audio->size(), 16000};
        result.phrases = std::move(phrases);
        return result;
    }

    ClientRig rig;
    MiniHost host;
    PairingResult approved;
    ClientPairing pairing;
    RemoteSpeechBackend backend;
};

} // namespace

TEST_CASE("test_a_host_that_doesnt_time_requests_gives_no_split", "[test_remote_engine]") {
    MiniHost::Options old;
    old.timed = false;
    ScriptedBackend fixture(old);
    auto& backend = fixture.backend;
    REQUIRE(backend.load({}).state == speech::ReadyState::Ready);
    const auto mark = backend.timing_mark();
    CHECK(backend.decode(fixture.job(1600)).transcript.text == "heard 1600");
    const auto timing = backend.timing_since(mark);
    CHECK(timing.host == "devbox");
    CHECK(timing.requests == 1);
    CHECK(timing.round_trip_s > 0.0);
    CHECK_FALSE(timing.host_s.has_value());
    CHECK_FALSE(timing.network_s().has_value());
}

TEST_CASE("a host that times its requests gives the split it reports", "[test_remote_engine]") {
    ScriptedBackend fixture;
    auto& backend = fixture.backend;
    REQUIRE(backend.load({}).state == speech::ReadyState::Ready);
    const auto mark = backend.timing_mark();
    (void)backend.decode(fixture.job(1600));
    (void)backend.decode(fixture.job(3200));
    const auto timing = backend.timing_since(mark);
    CHECK(timing.requests == 2);
    REQUIRE(timing.host_s.has_value());
    CHECK(*timing.host_s == 0.003); // host_ms 1.5, twice
}

TEST_CASE("test_a_host_from_before_model_switching", "[test_remote_engine]") {
    MiniHost::Options old;
    old.list_models = false;
    ScriptedBackend fixture(old);
    auto& backend = fixture.backend;
    REQUIRE(backend.load({}).state == speech::ReadyState::Ready);
    const auto choices = backend.model_choices();
    CHECK_FALSE(choices.models.has_value());
    REQUIRE(choices.current.has_value());
    CHECK(choices.current->key() == std::pair<std::string, std::string>{"parakeet", "parakeet-v3"});

    (void)backend.request_model("nemotron", "nemotron-3.5");
    const auto readiness = backend.load({});
    REQUIRE(readiness.state == speech::ReadyState::Ready);
    CHECK(backend.is_available());
    const auto expected =
        std::format("Update {} on devbox to choose its model from here.", identity().product);
    CHECK(backend.switch_error() == expected);
    CHECK(readiness.message == expected);
    const auto requests = fixture.host.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].value("op", "") == "select_model");
    CHECK(requests[0].value("family", "") == "nemotron");
    CHECK(requests[0].value("model", "") == "nemotron-3.5");
}

TEST_CASE("test_a_host_that_stays_busy_fails_with_a_reason", "[test_remote_engine]") {
    MiniHost::Options full;
    full.transcribe_busy_count = 100000;
    ScriptedBackend fixture(full);
    auto& backend = fixture.backend;
    REQUIRE(backend.load({}).state == speech::ReadyState::Ready);
    backend.set_busy_retry(0.3, 0.05);
    const auto began = std::chrono::steady_clock::now();
    try {
        (void)backend.decode(fixture.job(1600));
        FAIL("a host that never had room answered");
    } catch (const EngineError& error) {
        CHECK_THAT(std::string(error.what()), Matches(".*busy.*stayed busy for 0 seconds.*"));
        CHECK(std::string_view(error.what()) == "The host is busy. It stayed busy for 0 seconds.");
    }
    const auto took = std::chrono::steady_clock::now() - began;
    CHECK(took >= 300ms);
    CHECK(took < 3s);
    // Sent again while it waited: the host refuses before it starts any work.
    CHECK(fixture.host.busy_replies() >= 2);
    // The connection outlives the refusals.
    CHECK(backend.is_available());
}

TEST_CASE("test_client_keeps_phrases_from_a_host_without_the_capability",
          "[test_personalize_s4_remote]") {
    ScriptedBackend fixture; // its `ready` says recognition_hints false
    auto& backend = fixture.backend;
    REQUIRE(backend.load({}).state == speech::ReadyState::Ready);
    CHECK_FALSE(backend.supports_recognition());
    CHECK(backend.recognition_support() == "after");
    CHECK(backend.decode(fixture.job(16000, {"Ksenia", "Kubernetes"})).transcript.text ==
          "heard 16000");
    const auto requests = fixture.host.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].value("op", "") == "transcribe");
    CHECK(std::ranges::none_of(requests,
                               [](const Json& header) { return header.contains("phrases"); }));
}
