// The client backend over a real host on 127.0.0.1 (tests/test_remote_engine.py's paired_backend
// tests): the host and client talk over a TLS WebSocket; only the speech engine behind the host
// is fake. Each case carries its OpenWhisper function's name.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "engine/error.h"
#include "host_rig.h"
#include "paired_backend.h"
#include "remote/client.h"
#include "remote/connection.h"
#include "remote/errors.h"
#include "remote/remote_backend.h"
#include "speech/service.h"

using namespace fv;
using namespace fv::remote_host;
using namespace fv::remote_host::rig;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Matches;
using fv::remote::RemoteSpeechBackend;
using fv::speech::ReadyState;

namespace {

std::string heard(RemoteSpeechBackend& backend, std::size_t samples) {
    return backend.decode(job_of(tone(samples))).transcript.text;
}

std::size_t transcribes(const HostRig& rig) {
    std::size_t count = 0;
    for (const auto& call : rig.engine->calls()) {
        count += call.op == "transcribe" ? 1 : 0;
    }
    return count;
}

// A host engine whose decode waits until the test lets it go.
class HeldEngine final : public HostEngine {
public:
    std::vector<std::string> identity() const override { return {"fake", "parakeet", "held"}; }
    Json describe() const override {
        return Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Fake Parakeet"},
                    {"device", "cuda"},     {"streaming", false},     {"available", true},
                    {"status", "ready"}};
    }
    Json transcribe(std::span<const float> audio, std::optional<std::string>,
                    std::span<const std::string>) override {
        std::unique_lock lock(mutex_);
        ++calls_;
        if (hold_) {
            entered_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, 5s, [&] { return !hold_; });
            return Json{{"text", "done"}, {"segments", Json::array()}};
        }
        const std::string text = "heard " + std::to_string(audio.size());
        return Json{{"text", text}, {"segments", Json::array()}};
    }
    void hold() {
        std::lock_guard lock(mutex_);
        hold_ = true;
        entered_ = false;
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [&] { return entered_; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            hold_ = false;
        }
        changed_.notify_all();
    }
    int calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool hold_ = false;
    bool entered_ = false;
    int calls_ = 0;
};

const Json kHostModels = Json::array({
    Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Parakeet TDT 0.6B v3"}},
    Json{{"family", "nemotron"}, {"model", "nemotron-3.5"}, {"label", "Nemotron 3.5 ASR 0.6B"}},
    Json{{"family", "local_whisper"}, {"model", "small"}, {"label", "Whisper small"}},
});

// The host lists models and switches its fake engine the way the app does (the `switchable`
// fixture).
struct Switchable {
    explicit Switchable(Executor* reconnect_on = nullptr)
        : paired(
              [this](SpeechHostDeps& deps) {
                  deps.models = [] {
                      return kHostModels;
                  };
                  deps.select_model = [this](std::string family, std::string model,
                                             std::string device_name, std::optional<std::string>) {
                      {
                          std::lock_guard lock(mutex);
                          chosen.push_back(family + "|" + model + "|" + device_name);
                      }
                      if (slow.valid()) {
                          started.set_value();
                          slow.wait_for(3s);
                      }
                      if (model == "busy") {
                          throw std::runtime_error(
                              "devbox is transcribing right now. Try again in a moment.");
                      }
                      paired.host.engine->set_family(family);
                      paired.host.engine->set_model(model);
                      return paired.host.engine->describe();
                  };
              },
              reconnect_on) {}

    std::vector<std::string> switches() {
        std::lock_guard lock(mutex);
        return chosen;
    }

    std::mutex mutex;
    std::vector<std::string> chosen;
    // Set to make a switch wait (for at most 3 s) until the test lets it go.
    std::shared_future<void> slow;
    std::promise<void> started;
    PairedBackend paired;
};

std::vector<std::pair<std::string, std::string>> keys(const remote::RemoteModels& choices) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& model : choices.models.value_or(std::vector<remote::HostModel>{})) {
        out.push_back(model.key());
    }
    return out;
}

// What the SpeechService would hear from the backend between loads.
struct Reports {
    explicit Reports(RemoteSpeechBackend& backend)
        : link(backend.watch_readiness(inline_executor, [this](speech::Readiness read) {
              std::lock_guard lock(mutex);
              seen.push_back(std::move(read));
          })) {}

    std::vector<speech::Readiness> all() {
        std::lock_guard lock(mutex);
        return seen;
    }
    std::size_t count() {
        std::lock_guard lock(mutex);
        return seen.size();
    }

    InlineExecutor inline_executor;
    std::mutex mutex;
    std::vector<speech::Readiness> seen;
    Subscription link;
};

} // namespace

// ---- the client backend ----

TEST_CASE("test_remote_backend_adopts_the_host_engine") {
    PairedBackend fixture;
    auto& backend = *fixture.backend;
    CHECK_FALSE(backend.is_available());
    const auto readiness = backend.load({});
    REQUIRE(readiness.state == ReadyState::Ready);
    CHECK(backend.is_available());
    // The settings select "remote"; the engine behind it is the host's from now on, which is
    // what lets the preview and the early decode follow whichever engine the host runs.
    CHECK(backend.id() == "remote");
    CHECK(backend.family() == "parakeet");
    CHECK(backend.capabilities().windows);
    CHECK(backend.capabilities().live_preview);
    CHECK(backend.model_name() == "parakeet-v3");
    CHECK(backend.name() == "Fake Parakeet on devbox");
    CHECK(backend.device_info() == "Fake Parakeet on devbox | cuda");
    CHECK(readiness.device_info == backend.device_info());
    // Nothing of the model lives here: no model to download, nothing to warm.
    CHECK(readiness.message.empty());
    CHECK_FALSE(backend.warm_up({}));
    CHECK(backend.last_error().empty());
}

TEST_CASE("test_silent_windows_never_leave_the_client") {
    PairedBackend fixture;
    auto& backend = *fixture.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.decode(job_of(std::vector<float>(16000, 0.0f))).transcript.text.empty());
    CHECK(transcribes(fixture.host) == 0);
}

TEST_CASE("test_remote_backend_reports_a_host_without_a_ready_engine") {
    PairedBackend fixture;
    fixture.host.engine->set_available(false);
    const auto readiness = fixture.backend->load({});
    CHECK(readiness.state == ReadyState::Offline);
    CHECK_FALSE(fixture.backend->is_available());
    CHECK(fixture.backend->device_info() == "devbox: not loaded");
    CHECK(readiness.device_info == "devbox: not loaded");
}

TEST_CASE("test_remote_backend_without_pairing_says_how_to_pair") {
    PairedBackend fixture;
    fixture.forget_pairing();
    auto& backend = *fixture.backend;
    const auto readiness = backend.load({});
    CHECK(readiness.state == ReadyState::Offline);
    CHECK_FALSE(backend.is_available());
    CHECK(backend.device_info() == "Pair with a host in Settings → Remote engine.");
    CHECK(fixture.connections == 0);
}

TEST_CASE("the next job after a cancel is decoded on the same connection") {
    // test_the_next_job_clears_a_remote_engines_cancel: OpenWhisper's cancel is a flag the next
    // job clears (TranscriptionRuntime._rearm_remote_engine); here a cancel belongs to the job it
    // stopped, so nothing is left to clear and the connection is kept all the same.
    auto engine = std::make_shared<HeldEngine>();
    PairedBackend fixture;
    fixture.host.serve(engine);
    auto& backend = *fixture.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);

    backend.cancel_transcription(); // nothing runs: nothing to stop
    CHECK(heard(backend, 160) == "heard 160");

    engine->hold();
    auto canceled = std::async(std::launch::async, [&]() -> std::string {
        try {
            return heard(backend, 1600);
        } catch (const std::exception& error) {
            return error.what();
        }
    });
    REQUIRE(engine->wait_entered());
    backend.cancel_transcription();
    REQUIRE(canceled.wait_for(2s) == std::future_status::ready);
    CHECK(canceled.get() == "Transcription canceled");
    CHECK(backend.is_available());
    engine->release();
    // The canceled request's late reply is skipped, not taken for this one's.
    CHECK(heard(backend, 320) == "heard 320");
    CHECK(fixture.connections == 1);
}

// ---- the link: what the engine card shows ----

TEST_CASE("test_link_without_a_pairing_is_unpaired") {
    PairedBackend fixture;
    fixture.forget_pairing();
    auto& backend = *fixture.backend;
    CHECK(backend.link().state == "unpaired");
    (void)backend.load({});
    const auto link = backend.link();
    CHECK(link.state == "unpaired");
    CHECK_THAT(link.detail, ContainsSubstring("Pair with a host"));
}

TEST_CASE("test_link_reads_connected_and_the_round_trip") {
    PairedBackend fixture;
    auto& backend = *fixture.backend;
    CHECK(backend.link().state == "offline"); // paired, not reached yet
    CHECK(backend.link().host == "devbox");
    InlineExecutor here;
    std::mutex mutex;
    std::vector<std::string> changes;
    const auto watching = backend.link_changed.connect(here, [&] {
        auto state = backend.link().state;
        std::lock_guard lock(mutex);
        changes.push_back(std::move(state));
    });
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    {
        std::lock_guard lock(mutex);
        CHECK(std::ranges::find(changes, "connecting") != changes.end());
        REQUIRE_FALSE(changes.empty());
        CHECK(changes.back() == "connected");
    }
    const auto link = backend.link();
    CHECK(link.host == "devbox");
    CHECK(link.route == "local network");
    CHECK(link.engine_label == "Fake Parakeet");
    CHECK(link.device == "cuda");
    CHECK(link.address == remote::format_address("127.0.0.1", fixture.host.port));
    CHECK(link.detail.empty());
    // One ping goes out on connect, so the round trip is known at once.
    CHECK(wait_for([&] { return backend.link().latency_ms.has_value(); }));
    CHECK(backend.link().beat > 0);
}

TEST_CASE("test_link_is_busy_while_a_request_waits_and_counts_replies") {
    auto engine = std::make_shared<HeldEngine>();
    PairedBackend fixture;
    fixture.host.serve(engine);
    auto& backend = *fixture.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    engine->hold();
    auto decoded = std::async(std::launch::async, [&] { return heard(backend, 1600); });
    REQUIRE(engine->wait_entered());
    CHECK(backend.link().busy);
    CHECK(backend.link().replies == 0);
    engine->release();
    CHECK(decoded.get() == "done");
    CHECK_FALSE(backend.link().busy);
    CHECK(backend.link().replies == 1);
}

TEST_CASE("test_check_link_notices_a_host_that_went_away_between_requests") {
    PairedBackend fixture;
    auto& backend = *fixture.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK_FALSE(backend.check_link());
    fixture.stop_host();
    REQUIRE(wait_for([&] { return backend.link().state != "connected"; }));
    CHECK(backend.check_link());
    CHECK_FALSE(backend.is_available());
    const auto link = backend.link();
    CHECK(link.state == "offline");
    CHECK(link.detail == "devbox stopped answering.");
    CHECK(backend.device_info() == "devbox stopped answering.");
    CHECK_FALSE(backend.check_link()); // reported once
}

TEST_CASE("test_restore_link_reconnects_quietly_or_adopts_a_new_engine") {
    PairedBackend fixture;
    auto& backend = *fixture.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    fixture.stop_host();
    REQUIRE(wait_for([&] { return backend.check_link() || !backend.is_available(); }));
    CHECK(backend.restore_link() == "offline");
    CHECK_THAT(backend.last_error(), ContainsSubstring("Couldn't reach"));
    fixture.start_host();
    CHECK(backend.restore_link() == "connected");
    CHECK(backend.is_available());
    CHECK(backend.family() == "parakeet");
    CHECK(heard(backend, 160) == "heard 160");

    // Back with another engine: adopted, and the caller told so. The host says it switched by
    // closing the idle connection (4409), which stands in for OpenWhisper's _drop here.
    fixture.host.engine->set_family("nemotron");
    fixture.host.engine->set_model("nemotron-streaming");
    CHECK(fixture.host.host->engine_changed() == 1);
    REQUIRE(wait_for([&] { return backend.link().state != "connected"; }));
    CHECK(backend.check_link());
    CHECK(backend.last_error() == "devbox switched engines. Reconnecting...");
    CHECK(backend.restore_link() == "changed");
    CHECK(backend.family() == "nemotron");
    CHECK(backend.is_available());
    CHECK(backend.restore_link() == "connected"); // nothing to do

    // Forgotten meanwhile: nothing to connect to.
    fixture.stop_host();
    REQUIRE(wait_for([&] { return backend.check_link() || !backend.is_available(); }));
    fixture.forget_pairing();
    CHECK(backend.restore_link() == "unpaired");
    CHECK(backend.link().state == "unpaired");
}

TEST_CASE("test_timing_splits_the_host_from_the_network") {
    PairedBackend fixture;
    auto& backend = *fixture.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    auto mark = backend.timing_mark();
    (void)heard(backend, 1600);
    (void)heard(backend, 1600);
    const auto timing = backend.timing_since(mark);
    CHECK(timing.host == "devbox");
    CHECK(timing.requests == 2);
    REQUIRE(timing.host_s.has_value());
    CHECK(*timing.host_s >= 0.0);
    CHECK(*timing.host_s <= timing.round_trip_s);
    REQUIRE(timing.network_s().has_value());
    CHECK(std::abs(*timing.network_s() - (timing.round_trip_s - *timing.host_s)) < 1e-9);
    // Silent windows never leave, so they add nothing.
    mark = backend.timing_mark();
    (void)backend.decode(job_of(std::vector<float>(1600, 0.0f)));
    CHECK(backend.timing_since(mark).requests == 0);
    CHECK_FALSE(backend.timing_since(mark).host_s.has_value());
}

// ---- vocabulary hints (tests/test_personalize_s4_remote.py) ----

TEST_CASE("test_client_sends_phrases_only_to_a_host_that_uses_them") {
    PairedBackend fixture;
    fixture.host.engine->set_accepts_phrases(true); // a host engine that uses hints
    auto& backend = *fixture.backend;
    const std::vector<std::string> phrases{"Ksenia", "Kubernetes"};
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.supports_recognition());
    CHECK(backend.recognition_support() == "model");
    (void)backend.decode(job_of(tone(16000), "fr", phrases));
    auto calls = fixture.host.engine->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.back().language == std::optional<std::string>("fr"));
    CHECK(calls.back().phrases == phrases);
    (void)backend.decode(job_of(tone(16000)));
    calls = fixture.host.engine->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls.back().phrases.empty());
    backend.release();
    CHECK(backend.recognition_support() == "after");
}

// ---- choosing the host's model ----

TEST_CASE("test_model_choices_follow_the_connection") {
    Switchable fixture;
    auto& backend = *fixture.paired.backend;
    CHECK(backend.model_choices() == remote::RemoteModels{.host = "devbox"});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    const auto choices = backend.model_choices();
    CHECK(choices.host == "devbox");
    CHECK(keys(choices) ==
          std::vector<std::pair<std::string, std::string>>{{"parakeet", "parakeet-v3"},
                                                           {"nemotron", "nemotron-3.5"},
                                                           {"local_whisper", "small"}});
    REQUIRE(choices.current.has_value());
    CHECK(choices.current->key() == std::pair<std::string, std::string>{"parakeet", "parakeet-v3"});
    CHECK(choices.current->label == "Fake Parakeet");
    backend.release();
    CHECK_FALSE(backend.model_choices().current.has_value());
    // Not paired: nothing to show at all.
    fixture.paired.forget_pairing();
    CHECK(backend.model_choices() == remote::RemoteModels{});
}

TEST_CASE("test_choosing_a_host_model_switches_the_host_and_reconnects_to_it") {
    Switchable fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.request_model("local_whisper", "small").label == "Whisper small");
    const auto readiness = backend.load({});
    CHECK(fixture.switches() == std::vector<std::string>{"local_whisper|small|laptop"});
    REQUIRE(readiness.state == ReadyState::Ready);
    CHECK(backend.is_available());
    CHECK(backend.family() == "local_whisper");
    CHECK(backend.model_name() == "small");
    REQUIRE(backend.model_choices().current.has_value());
    CHECK(backend.model_choices().current->key() ==
          std::pair<std::string, std::string>{"local_whisper", "small"});
    CHECK(backend.switch_error().empty());
    CHECK(readiness.message.empty());
    // The choice rides on one reload only.
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.switches().size() == 1);
    // A model the host already serves is not asked for again.
    (void)backend.request_model("local_whisper", "small");
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.switches().size() == 1);
}

TEST_CASE("test_a_host_that_keeps_its_model_says_why") {
    Switchable fixture;
    auto& backend = *fixture.paired.backend;
    // Not listed (the host was never reached): the choice is called by its key.
    CHECK(backend.request_model("parakeet", "busy").label == "busy");
    const auto readiness = backend.load({});
    REQUIRE(readiness.state == ReadyState::Ready);
    CHECK(backend.is_available());
    CHECK(backend.model_name() == "parakeet-v3");
    CHECK(backend.switch_error() == "devbox is transcribing right now. Try again in a moment.");
    // The reason reaches the status line with the ready line, as a CPU fallback's note does.
    CHECK(readiness.message == backend.switch_error());
    // Said once: the next load asks for nothing.
    CHECK(backend.load({}).message.empty());
    CHECK(backend.switch_error().empty());
}

TEST_CASE("test_the_models_are_forgotten_when_the_host_is_out_of_reach") {
    Switchable fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    REQUIRE(backend.model_choices().models.has_value());
    CHECK_FALSE(backend.model_choices().models->empty());
    fixture.paired.stop_host();
    CHECK(backend.load({}).state == ReadyState::Offline);
    CHECK_FALSE(backend.model_choices().models.has_value());
    CHECK_FALSE(backend.model_choices().current.has_value());
}

TEST_CASE("test_cleanup_ends_a_switch_the_host_is_still_loading") {
    Switchable fixture;
    std::promise<void> finish;
    fixture.slow = finish.get_future().share();
    auto started = fixture.started.get_future();
    auto& backend = *fixture.paired.backend;
    (void)backend.request_model("nemotron", "nemotron-3.5");
    auto loading = std::async(std::launch::async, [&] { return backend.load({}); });
    REQUIRE(started.wait_for(5s) == std::future_status::ready);
    const auto began = std::chrono::steady_clock::now();
    backend.release();
    REQUIRE(loading.wait_for(5s) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - began < 2s);
    CHECK(loading.get().state == ReadyState::Offline);
    CHECK_FALSE(backend.is_available());
    CHECK(backend.last_error().empty());
    finish.set_value();
}

TEST_CASE("a newer load ends a switch the host is still loading") {
    // The SpeechService's way to end a load: the token it hands in.
    Switchable fixture;
    std::promise<void> finish;
    fixture.slow = finish.get_future().share();
    auto started = fixture.started.get_future();
    auto& backend = *fixture.paired.backend;
    (void)backend.request_model("nemotron", "nemotron-3.5");
    CancelSource superseded;
    auto loading = std::async(std::launch::async, [&] { return backend.load(superseded.token()); });
    REQUIRE(started.wait_for(5s) == std::future_status::ready);
    superseded.cancel();
    REQUIRE(loading.wait_for(2s) == std::future_status::ready);
    CHECK(loading.get().state == ReadyState::Offline);
    CHECK_FALSE(backend.is_available());
    CHECK(backend.last_error().empty());
    finish.set_value();
}

// ---- the link keeps itself (services/runtime/remote.py's watch and retries) ----

TEST_CASE("test_a_dropped_host_is_reported_at_once_and_retried_with_backoff") {
    FakeClock clock;
    ManualExecutor io(clock);
    PairedBackend fixture({}, &io, clock);
    auto& backend = *fixture.backend;
    Reports reports(backend);
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.link().state == "connected");
    CHECK_FALSE(backend.link().retry_at.has_value());
    CHECK(io.pending_timers() == 0);

    fixture.stop_host();
    // Said now, not at the next recording: nothing asked the backend anything.
    REQUIRE(wait_for([&] { return reports.count() == 1; }));
    CHECK(reports.all()[0].state == ReadyState::Offline);
    CHECK(reports.all()[0].device_info == "devbox stopped answering.");
    CHECK(reports.all()[0].message == "devbox stopped answering.");

    // The keeper's turn: the dead connection is dropped and the first retry scheduled.
    REQUIRE(wait_for([&] { return io.queued() > 0; }));
    io.run_all();
    CHECK_FALSE(backend.is_available());
    CHECK(backend.device_info() == "devbox stopped answering.");
    auto link = backend.link();
    CHECK(link.state == "offline");
    REQUIRE(link.retry_at.has_value());
    CHECK(*link.retry_at - clock.steady() == remote::REMOTE_RETRY_DELAYS[0]);
    CHECK(io.pending_timers() == 1);

    // Each failure waits longer, up to the last delay, which repeats.
    using namespace std::chrono;
    const std::vector<seconds> waits{2s, 5s, 15s, 30s, 60s, 60s, 60s};
    for (std::size_t attempt = 0; attempt + 1 < waits.size(); ++attempt) {
        const int before = fixture.connections;
        io.advance(waits[attempt] - 1ms);
        CHECK(fixture.connections == before); // not yet due
        io.advance(1ms);
        CHECK(fixture.connections == before + 1);
        CHECK_FALSE(backend.restoring());
        REQUIRE(backend.retry_at().has_value());
        CHECK(*backend.retry_at() - clock.steady() == waits[attempt + 1]);
        CHECK(io.pending_timers() == 1);
    }
    // test_repeat_failures_dont_clear_the_status_line_every_time: the first failed retry said
    // why the host can't be reached; the ones after it read the same and said nothing.
    const auto all = reports.all();
    REQUIRE(all.size() == 2);
    CHECK(all[1].state == ReadyState::Offline);
    CHECK_THAT(all[1].device_info, ContainsSubstring("Couldn't reach"));
}

TEST_CASE("test_a_quiet_reconnect_restores_the_engine_card") {
    // Through the SpeechService, whose readiness is what the engine card shows.
    FakeClock clock;
    ManualExecutor io(clock);
    InlineExecutor notify;
    PairedBackend fixture({}, &io, clock);
    speech::SpeechService speech(
        [&](const speech::BackendConfig&) { return fixture.make(&io, clock); }, notify);
    const auto readiness = [&] {
        return speech.readiness(speech::Slot::Dictation);
    };
    speech.select(speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(wait_for([&] { return readiness().state == ReadyState::Ready; }));
    CHECK(readiness().device_info == "Fake Parakeet on devbox | cuda");
    const auto generation = speech.generation(speech::Slot::Dictation);

    fixture.stop_host();
    REQUIRE(wait_for([&] { return readiness().state == ReadyState::Offline; }));
    CHECK(readiness().device_info == "devbox stopped answering.");
    REQUIRE(wait_for([&] { return io.queued() > 0; }));
    io.run_all();
    REQUIRE(io.pending_timers() == 1);

    fixture.start_host();
    io.advance(remote::REMOTE_RETRY_DELAYS[0]);
    REQUIRE(wait_for([&] { return readiness().state == ReadyState::Ready; }));
    CHECK(readiness().device_info == "Fake Parakeet on devbox | cuda");
    // Reconnected in place: no reload ran, and nothing is left to retry.
    CHECK(speech.generation(speech::Slot::Dictation) == generation);
    CHECK(io.pending_timers() == 0);
    CHECK(speech.run(job_of(tone(160))).transcript.text == "heard 160");

    // The backoff starts over for the next outage.
    fixture.stop_host();
    REQUIRE(wait_for([&] { return readiness().state == ReadyState::Offline; }));
    REQUIRE(wait_for([&] { return io.queued() > 0; }));
    io.run_all();
    REQUIRE(io.pending_timers() == 1);
    const int before = fixture.connections;
    io.advance(remote::REMOTE_RETRY_DELAYS[0]);
    CHECK(fixture.connections == before + 1);
}

TEST_CASE("a host that is off when the engine is selected is tried again") {
    // RemoteRuntime._on_settled("reload"): a load that could not connect starts the retries.
    FakeClock clock;
    ManualExecutor io(clock);
    PairedBackend fixture({}, &io, clock);
    auto& backend = *fixture.backend;
    Reports reports(backend);
    fixture.stop_host();
    const auto readiness = backend.load({});
    CHECK(readiness.state == ReadyState::Offline);
    CHECK_THAT(readiness.device_info, ContainsSubstring("Couldn't reach"));
    REQUIRE(io.pending_timers() == 1);
    fixture.start_host();
    io.advance(remote::REMOTE_RETRY_DELAYS[0]);
    CHECK(backend.is_available());
    // The host came back with the engine this backend never got to adopt: said at once.
    REQUIRE(reports.count() == 1);
    CHECK(reports.all()[0].state == ReadyState::Ready);
    CHECK(reports.all()[0].device_info == "Fake Parakeet on devbox | cuda");
    CHECK(io.pending_timers() == 0);
    CHECK_FALSE(backend.retry_at().has_value());
}

TEST_CASE("retry_now reconnects without waiting for the next scheduled retry") {
    FakeClock clock;
    ManualExecutor io(clock);
    PairedBackend fixture({}, &io, clock);
    auto& backend = *fixture.backend;
    fixture.stop_host();
    CHECK(backend.load({}).state == ReadyState::Offline);
    REQUIRE(io.pending_timers() == 1);
    fixture.start_host();
    backend.retry_now();
    CHECK_FALSE(backend.is_available()); // off the caller's thread
    io.run_all();
    CHECK(backend.is_available());
    CHECK(io.pending_timers() == 0);
    // Connected: asking again does nothing.
    const int before = fixture.connections;
    backend.retry_now();
    io.run_all();
    CHECK(fixture.connections == before);
}

TEST_CASE("test_an_unpaired_remote_is_not_retried") {
    FakeClock clock;
    ManualExecutor io(clock);
    PairedBackend fixture({}, &io, clock);
    fixture.forget_pairing();
    auto& backend = *fixture.backend;
    CHECK(backend.load({}).state == ReadyState::Offline);
    CHECK(io.pending_timers() == 0);
    CHECK_FALSE(backend.retry_at().has_value());
    // Asked to anyway: it finds nothing to connect to and schedules nothing.
    backend.retry_now();
    io.run_all();
    CHECK(io.pending_timers() == 0);
    CHECK(fixture.connections == 0);
}

TEST_CASE("a released backend stops retrying") {
    FakeClock clock;
    ManualExecutor io(clock);
    PairedBackend fixture({}, &io, clock);
    auto& backend = *fixture.backend;
    fixture.stop_host();
    CHECK(backend.load({}).state == ReadyState::Offline);
    REQUIRE(io.pending_timers() == 1);
    backend.release();
    CHECK(io.pending_timers() == 0);
    fixture.backend.reset();
    io.advance(std::chrono::seconds(120));
    CHECK(fixture.connections == 1);
}

// ---- the dictation slot (services/application_controller.py's reload of the remote engine) ----

TEST_CASE("test_an_unreachable_host_never_asks_to_install_a_local_runtime") {
    InlineExecutor notify;
    PairedBackend fixture;
    fixture.forget_pairing();
    speech::SpeechService speech([&](const speech::BackendConfig&) { return fixture.make(); },
                                 notify);
    std::mutex mutex;
    std::vector<speech::Readiness> seen;
    const auto watching =
        speech.readiness_changed.connect(notify, [&](speech::Slot, speech::Readiness read) {
            std::lock_guard lock(mutex);
            seen.push_back(std::move(read));
        });
    speech.select(speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(wait_for(
        [&] { return speech.readiness(speech::Slot::Dictation).state == ReadyState::Offline; }));
    const auto readiness = speech.readiness(speech::Slot::Dictation);
    CHECK(readiness.device_info == "Pair with a host in Settings → Remote engine.");
    // Offline, never ModelMissing or RuntimeMissing: those are what start a download consent.
    std::lock_guard lock(mutex);
    for (const auto& read : seen) {
        CHECK(read.state != ReadyState::ModelMissing);
        CHECK(read.state != ReadyState::RuntimeMissing);
    }
}

TEST_CASE("test_selecting_the_remote_engine_connects_without_a_warmup_decode") {
    InlineExecutor notify;
    PairedBackend fixture;
    speech::SpeechService speech([&](const speech::BackendConfig&) { return fixture.make(); },
                                 notify);
    std::mutex mutex;
    std::vector<bool> warmed;
    const auto watching = speech.warmed_up.connect(notify, [&](speech::Slot, bool ran) {
        std::lock_guard lock(mutex);
        warmed.push_back(ran);
    });
    speech.select(speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(wait_for(
        [&] { return speech.readiness(speech::Slot::Dictation).state == ReadyState::Ready; }));
    CHECK(speech.readiness(speech::Slot::Dictation).device_info ==
          "Fake Parakeet on devbox | cuda");
    // Parakeet warms up locally, but the host warmed its own engine when it loaded, so even an
    // asked-for warm-up decodes nothing there.
    speech.warm_up(speech::Slot::Dictation);
    REQUIRE(wait_for([&] {
        std::lock_guard lock(mutex);
        return !warmed.empty();
    }));
    {
        std::lock_guard lock(mutex);
        CHECK(warmed == std::vector<bool>{false});
    }
    CHECK(fixture.host.engine->calls().empty());
}

// ---- Meeting Mode's own connection (tests/test_meeting_remote.py) ----

namespace {

std::shared_ptr<Json> meeting_route(const PairedBackend& fixture) {
    return std::make_shared<Json>(
        Json{{"fingerprint", fixture.pairing->fingerprint}, {"host_name", "devbox"}});
}

} // namespace

TEST_CASE("test_host_change_and_new_pairing_are_rejected_before_audio") {
    PairedBackend fixture;
    const auto route = meeting_route(fixture);
    auto backend = fixture.make(nullptr, system_clock(), route);
    REQUIRE(backend->load({}).state == ReadyState::Ready);
    const Json original = *route;
    CHECK(original.at("model") == "parakeet-v3");

    // The host switched models under the meeting: its audio is not sent to another model.
    fixture.host.engine->set_model("orukeet-v0.1");
    CHECK(fixture.host.host->engine_changed() == 1);
    REQUIRE(wait_for([&] { return backend->link().state != "connected"; }));
    try {
        (void)backend->decode(job_of(tone(16000)));
        FAIL("the meeting decoded with another model");
    } catch (const EngineError& error) {
        CHECK(std::string_view(error.what()) ==
              "Restore parakeet-v3 on devbox to resume this meeting.");
        CHECK(error.retry == EngineError::Retry::Connection);
    }
    CHECK(fixture.host.engine->calls().empty());

    // A new pairing is another computer as far as this meeting goes.
    {
        std::lock_guard lock(fixture.mutex);
        fixture.pairing->fingerprint = "different";
    }
    try {
        (void)backend->decode(job_of(tone(16000)));
        FAIL("the meeting decoded on a different computer");
    } catch (const EngineError& error) {
        CHECK(error.retry == EngineError::Retry::Connection);
    }
    CHECK(*route == original);
    CHECK(fixture.host.engine->calls().empty());
}

TEST_CASE("test_host_restart_recovers_without_switching_model") {
    PairedBackend fixture;
    const auto route = meeting_route(fixture);
    auto backend = fixture.make(nullptr, system_clock(), route);
    REQUIRE(backend->load({}).state == ReadyState::Ready);
    fixture.stop_host();
    fixture.start_host();
    // Once this computer has seen the old connection close: a request written into a socket
    // that is closing may have reached the host, and is then deferred rather than sent again.
    REQUIRE(wait_for([&] { return backend->link().state != "connected"; }));
    const auto decoded = backend->decode(job_of(tone(16000), "fr"));
    CHECK(decoded.transcript.text == "heard 16000");
    const auto calls = fixture.host.engine->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.back().language == std::optional<std::string>("fr"));
    CHECK(backend->is_available());
    CHECK(route->at("model") == "parakeet-v3");
}

TEST_CASE("test_unsupported_host_engine_is_not_used") {
    PairedBackend fixture;
    fixture.host.engine->set_family("qwen_asr");
    fixture.host.engine->set_model("qwen3-asr-0.6b");
    const auto route = meeting_route(fixture);
    auto backend = fixture.make(nullptr, system_clock(), route);
    const auto readiness = backend->load({});
    CHECK(readiness.state == ReadyState::Offline);
    CHECK_FALSE(backend->is_available());
    CHECK_THAT(backend->last_error(), ContainsSubstring("does not support meetings"));
    CHECK(fixture.host.engine->calls().empty());
    CHECK_FALSE(route->contains("model"));
}

TEST_CASE("test_meeting_connect_busy_keeps_retry_delay") {
    // tests/test_remote_host_admission.py: a host at capacity refuses the connection, and the
    // delay it asks for reaches the meeting's worker with the refusal.
    Json route{{"fingerprint", "TEST"}, {"host_name", "host"}};
    remote::ClientPairing pairing;
    pairing.host = "127.0.0.1";
    pairing.fingerprint = "TEST";
    pairing.host_name = "host";
    RemoteSpeechBackend backend(
        [&] { return std::optional(pairing); },
        [](const remote::ClientPairing&) -> std::unique_ptr<remote::RemoteConnection> {
            throw remote::RemoteHostBusy("host at capacity", 2500);
        },
        system_clock(), std::make_shared<Json>(route));
    CHECK(backend.load({}).state == ReadyState::Offline);
    try {
        (void)backend.decode(job_of(tone(16000)));
        FAIL("a busy host decoded");
    } catch (const EngineError& error) {
        CHECK(std::string_view(error.what()) == "host at capacity");
        CHECK(error.retry == EngineError::Retry::Capacity);
        REQUIRE(error.retry_after_s.has_value());
        CHECK(*error.retry_after_s == 2.5);
    }
}
