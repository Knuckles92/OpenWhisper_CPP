// The warm-up decode (transcriber/optional_backend.py warmup): tests/test_engine_warmup.py's
// backend tests, on ParakeetBackend over a fake decoder and, where the decode lock is the
// subject, on the SpeechService queue that replaces it.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/manual_executor.h"
#include "speech/parakeet_backend.h"
#include "speech/service.h"
#include "speech/testing/fake_decoder.h"
#include "speech_test_support.h"

using Catch::Matchers::ContainsSubstring;
using fv::speech::ParakeetBackend;
using fv::speech::ReadyState;
using fv::speech::Slot;
using fv::speech::test::LogCapture;
using fv::speech::test::parakeet_config;
using fv::speech::test::pump_until;
using fv::speech::testing::FakeDecoders;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<FakeDecoders> decoders() {
    return std::make_shared<FakeDecoders>();
}

// _loaded(): a Parakeet engine loaded on the GPU.
std::unique_ptr<ParakeetBackend> loaded(const std::shared_ptr<FakeDecoders>& fake) {
    auto backend = std::make_unique<ParakeetBackend>(parakeet_config("cuda"), fake->factory());
    backend->reload_model();
    REQUIRE(backend->is_available());
    return backend;
}

float peak(const std::vector<float>& audio) {
    float largest = 0.0f;
    for (const float sample : audio) {
        largest = std::max(largest, std::fabs(sample));
    }
    return largest;
}

// A SpeechService whose dictation slot runs ParakeetBackend over `fake`, loaded.
struct Service {
    explicit Service(std::shared_ptr<FakeDecoders> fake_) : fake(std::move(fake_)) {
        service = std::make_unique<fv::speech::SpeechService>(
            [decoders = fake](const fv::speech::BackendConfig& config) {
                return std::make_unique<ParakeetBackend>(config, decoders->factory());
            },
            notify);
        links.push_back(
            service->warmed_up.connect(notify, [this](Slot, bool ran) { warmed.push_back(ran); }));
        links.push_back(service->readiness_changed.connect(
            notify, [this](Slot, fv::speech::Readiness r) { states.push_back(r.state); }));
    }

    void load() {
        const auto before = states.size();
        service->select(Slot::Dictation, parakeet_config("cuda"));
        REQUIRE(pump_until(notify, [&] {
            return std::count(states.begin() + static_cast<std::ptrdiff_t>(before), states.end(),
                              ReadyState::Ready) > 0;
        }));
    }

    std::shared_ptr<FakeDecoders> fake;
    fv::ManualExecutor notify;
    std::vector<bool> warmed;
    std::vector<ReadyState> states;
    std::vector<fv::Subscription> links;
    std::unique_ptr<fv::speech::SpeechService> service;
};

} // namespace

// from tests/test_engine_warmup.py::test_warmup_sends_quiet_non_silent_audio_through_transcribe
TEST_CASE("warmup sends quiet non silent audio through transcribe", "[test_engine_warmup]") {
    auto fake = decoders();
    auto backend = loaded(fake);
    std::vector<bool> seen_transcribing;
    FakeDecoders::Script script;
    script.on_transcribe = [&] {
        seen_transcribing.push_back(backend->is_transcribing());
    };
    fake->set_script(script);

    CHECK(backend->warmup());
    CHECK(backend->warmup());

    const auto decodes = fake->decodes();
    REQUIRE(decodes.size() == 2);
    const auto& first = decodes[0].samples;
    CHECK(first.size() == 16000);
    // transcribe_audio would drop a window this quiet or quieter, so the warm-up must stay
    // above it to reach the engine at all.
    CHECK(peak(first) > 0.00025f);
    CHECK(peak(first) < 0.1f);
    CHECK(first == decodes[1].samples);
    // A cancel press while idle must not see a job to tear down.
    CHECK(seen_transcribing == std::vector<bool>{false, false});
}

// from tests/test_engine_warmup.py::test_warmup_failure_is_non_fatal_and_keeps_engine_loaded
TEST_CASE("warmup failure is non fatal and keeps engine loaded", "[test_engine_warmup]") {
    auto fake = decoders();
    auto backend = loaded(fake);
    FakeDecoders::Script failing;
    failing.transcribe_error = "CUDA error: out of memory";
    fake->set_script(failing);
    LogCapture log;

    CHECK_FALSE(backend->warmup());

    CHECK(backend->is_available());
    CHECK_THAT(log.text(), ContainsSubstring("WARNING transcriber.optional_backend: Parakeet "
                                             "warmup failed (non-fatal): CUDA error"));
    // The engine takes the next decode.
    fake->set_script({});
    CHECK(backend->decode_window(std::vector<float>(1600, 0.2f), "en") == "w2");
}

// from tests/test_engine_warmup.py::test_warmup_logs_its_duration
TEST_CASE("warmup logs its duration", "[test_engine_warmup]") {
    auto fake = decoders();
    auto backend = loaded(fake);
    LogCapture log;
    backend->warmup();
    CHECK_THAT(log.text(), ContainsSubstring("INFO transcriber.optional_backend: Parakeet "
                                             "warmed up in "));
    CHECK_THAT(log.text(), ContainsSubstring(" ms\n"));
}

// from tests/test_engine_warmup.py::test_warmup_skips_an_unloaded_engine
TEST_CASE("warmup skips an unloaded engine", "[test_engine_warmup]") {
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config("cuda"), fake->factory());
    CHECK_FALSE(backend.warmup());
    CHECK(fake->created_gpus().empty());
    CHECK(fake->decodes().empty());
}

// from tests/test_engine_warmup.py::test_warmup_never_queues_behind_another_decode
// The decode lock is the speech thread's queue: a warm-up asked for while a decode holds the
// engine is skipped, not queued.
TEST_CASE("warmup never queues behind another decode", "[test_engine_warmup]") {
    auto fake = decoders();
    Service s(fake);
    s.load();
    FakeDecoders::Script holding;
    holding.hold_decodes = true;
    fake->set_script(holding);
    std::optional<std::string> text;
    s.service->submit(fv::speech::test::job(fv::speech::test::clip(std::vector<float>(1600, 0.2f))),
                      s.notify,
                      [&](auto result) { text = result ? result->transcript.text : "!"; });
    REQUIRE(fake->wait_entered(2s));

    s.service->warm_up(Slot::Dictation);
    REQUIRE(pump_until(s.notify, [&] { return !s.warmed.empty(); }));
    CHECK(s.warmed == std::vector<bool>{false});

    fake->resume();
    REQUIRE(pump_until(s.notify, [&] { return text.has_value(); }));
    CHECK(*text == "w1");
    CHECK(fake->decodes().size() == 1);
}

// from tests/test_engine_warmup.py::test_cleanup_during_warmup_ends_it_quietly
// "cleanup" from another thread is supersede(): the decoder cannot be closed under a running
// decode in process, so the decode ends and its result is stale.
TEST_CASE("cleanup during warmup ends it quietly", "[test_engine_warmup]") {
    const std::string interrupt = GENERATE("cleanup", "cancel_transcription");
    auto fake = decoders();
    auto backend = loaded(fake);
    FakeDecoders::Script holding;
    holding.hold_decodes = true;
    fake->set_script(holding);
    LogCapture log;

    auto warming = std::async(std::launch::async, [&] { return backend->warmup(); });
    REQUIRE(fake->wait_entered(2s));
    if (interrupt == "cleanup") {
        backend->supersede();
    } else {
        backend->cancel_transcription();
    }
    fake->resume();

    CHECK_FALSE(warming.get());
    CHECK_THAT(log.text(), ContainsSubstring("stopped by a reload or cancel"));
    CHECK_THAT(log.text(), !ContainsSubstring("warmup failed"));
    CHECK_FALSE(backend->is_available());
    CHECK(fake->alive() == 0);
}

// from tests/test_engine_warmup.py::test_reload_during_warmup_leaves_the_new_worker_untouched
TEST_CASE("reload during warmup leaves the new worker untouched", "[test_engine_warmup]") {
    auto fake = decoders();
    Service s(fake);
    s.load();
    FakeDecoders::Script holding;
    holding.hold_decodes = true;
    fake->set_script(holding);

    s.service->warm_up(Slot::Dictation);
    REQUIRE(fake->wait_entered(2s));
    fake->set_script({}); // the reload's engine decodes without holding
    s.service->select(Slot::Dictation, parakeet_config("cuda"));
    fake->resume();

    REQUIRE(pump_until(s.notify, [&] {
        return !s.warmed.empty() && s.states.back() == ReadyState::Ready && s.states.size() >= 4;
    }));
    CHECK(s.warmed == std::vector<bool>{false});
    CHECK(fake->created_gpus().size() == 2);
    CHECK(fake->alive() == 1);
    // The stale warm-up spilled no decode onto the new engine.
    CHECK(fake->decodes().size() == 1);
}

// from
// tests/test_engine_warmup.py::test_generation_change_while_waiting_for_the_lock_skips_the_decode
TEST_CASE("generation change while waiting for the lock skips the decode", "[test_engine_warmup]") {
    SECTION("the backend") {
        // A reload lands between the warm-up reading the generation and starting its decode.
        auto fake = decoders();
        auto backend = loaded(fake);
        fv::CancelSource reload;
        reload.cancel();
        CHECK_FALSE(backend->warmup(reload.token()));
        CHECK(fake->decodes().empty());
        CHECK(backend->is_available());
    }
    SECTION("the service") {
        // The warm-up waits behind a load; a select before it runs drops it unrun.
        auto fake = decoders();
        Service s(fake);
        s.load();
        FakeDecoders::Script holding;
        holding.hold_loads = true;
        fake->set_script(holding);
        s.service->release_local(Slot::Dictation);
        s.service->restore_local(Slot::Dictation);
        REQUIRE(fake->wait_entered(2s));
        s.service->warm_up(Slot::Dictation);
        s.service->select(Slot::Dictation, parakeet_config("cuda"));
        REQUIRE(pump_until(s.notify, [&] { return !s.warmed.empty(); }));
        CHECK(s.warmed == std::vector<bool>{false});
        fake->resume();
        REQUIRE(pump_until(s.notify, [&] { return s.states.back() == ReadyState::Ready; }));
        CHECK(fake->decodes().empty());
    }
}
