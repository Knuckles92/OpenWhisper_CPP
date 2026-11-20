// SpeechService (architecture 5.1, 5.3 to 5.6) over speech::testing::FakeBackend: lanes,
// coalescing, windows, cancel, generations, leases and the meeting slot.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/manual_executor.h"
#include "engine/error.h"
#include "speech/file_windows.h"
#include "speech/service.h"
#include "speech/testing/fake_backend.h"
#include "speech_test_support.h"
#include "testkit/testkit.h"

using fv::speech::DecodeResult;
using fv::speech::Lane;
using fv::speech::ReadyState;
using fv::speech::Slot;
using fv::speech::test::clip;
using fv::speech::test::job;
using fv::speech::test::parakeet_config;
using fv::speech::test::pump_until;
using fv::speech::testing::FakeEngine;
using fv::speech::testing::FakeScript;
using namespace std::chrono_literals;

namespace {

using Outcome = std::expected<DecodeResult, fv::EngineError>;

std::vector<float> audio(double seconds, float level = 0.2f) {
    return std::vector<float>(static_cast<std::size_t>(seconds * 16000), level);
}

struct Harness {
    explicit Harness(FakeScript script = {})
        : engine(std::make_shared<FakeEngine>(std::move(script))),
          service(std::make_unique<fv::speech::SpeechService>(engine->factory(), notify)) {
        links.push_back(
            service->readiness_changed.connect(notify, [this](Slot slot, fv::speech::Readiness r) {
                readiness.emplace_back(slot, std::move(r));
            }));
        links.push_back(
            service->busy_changed.connect(notify, [this](bool b) { busy.push_back(b); }));
        links.push_back(
            service->warmed_up.connect(notify, [this](Slot, bool ran) { warmed.push_back(ran); }));
    }

    // Selects and waits until the slot reports a final readiness.
    void load(Slot slot = Slot::Dictation, fv::speech::BackendConfig config = parakeet_config()) {
        const auto before = readiness.size();
        service->select(slot, std::move(config));
        REQUIRE(pump_until(notify, [&] {
            for (std::size_t i = before; i < readiness.size(); ++i) {
                if (readiness[i].first == slot &&
                    readiness[i].second.state != ReadyState::Loading) {
                    return true;
                }
            }
            return false;
        }));
    }

    std::shared_ptr<std::optional<Outcome>> submit(fv::speech::DecodeJob decode,
                                                   fv::speech::JobHandle* handle = nullptr) {
        auto result = std::make_shared<std::optional<Outcome>>();
        auto made = service->submit(std::move(decode), notify,
                                    [result](Outcome outcome) { *result = std::move(outcome); });
        if (handle != nullptr) {
            *handle = made;
        }
        return result;
    }

    void wait(const std::shared_ptr<std::optional<Outcome>>& result) {
        REQUIRE(pump_until(notify, [&] { return result->has_value(); }));
    }

    std::vector<Lane> decode_lanes() const {
        std::vector<Lane> lanes;
        for (const auto& call : engine->calls()) {
            if (call.op == "decode") {
                lanes.push_back(call.lane);
            }
        }
        return lanes;
    }

    std::shared_ptr<FakeEngine> engine;
    fv::ManualExecutor notify;
    std::vector<std::pair<Slot, fv::speech::Readiness>> readiness;
    std::vector<bool> busy;
    std::vector<bool> warmed;
    std::vector<fv::Subscription> links;
    std::unique_ptr<fv::speech::SpeechService> service;
};

bool canceled(const std::optional<Outcome>& outcome) {
    return outcome && !outcome->has_value() &&
           std::string(outcome->error().what()) == "Transcription canceled";
}

} // namespace

TEST_CASE("a selected engine loads on the speech thread and reports ready") {
    Harness h;
    h.load();
    REQUIRE(h.readiness.size() == 2);
    CHECK(h.readiness[0].second.state == ReadyState::Loading);
    CHECK(h.readiness[1].second.state == ReadyState::Ready);
    CHECK(h.readiness[1].second.device_info == "Parakeet TDT 0.6B v3 | cuda");
    CHECK(h.service->readiness(Slot::Dictation).state == ReadyState::Ready);
    CHECK(h.engine->ops() == std::vector<std::string>{"load"});
    REQUIRE(pump_until(h.notify, [&] { return h.busy.size() >= 2; }));
    CHECK(h.busy == std::vector<bool>{true, false});
}

TEST_CASE("a feature backend loads on demand and leaves injected backends in precedence") {
    const auto local = std::make_shared<FakeEngine>();
    const auto paired = std::make_shared<FakeEngine>();
    fv::ManualExecutor notify;
    fv::speech::SpeechService service(
        [factory = local->factory()](const fv::speech::BackendConfig& config) {
            return config.backend == "parakeet" ? factory(config) : nullptr;
        },
        notify);
    service.set_backend_factory("remote", paired->factory());
    service.set_backend_factory("parakeet", paired->factory());

    auto config = parakeet_config();
    service.select(Slot::Dictation, config);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state == ReadyState::Ready; }));
    CHECK(local->created() == 1);
    CHECK(paired->created() == 0);

    config.backend = "remote";
    service.select(Slot::Dictation, config);
    REQUIRE(pump_until(notify, [&] {
        return paired->created() == 1 &&
               service.readiness(Slot::Dictation).state == ReadyState::Ready;
    }));
    service.set_backend_factory("remote", {});
    service.select(Slot::Dictation, config);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state == ReadyState::Failed; }));
    CHECK(paired->created() == 1);
}

TEST_CASE("a decode comes back on the reply executor with its generation") {
    Harness h;
    h.load();
    auto result = h.submit(job(clip(audio(2.0), 3.0)));
    h.wait(result);
    REQUIRE((*result)->has_value());
    const DecodeResult& done = ***result;
    CHECK(done.transcript.text == "w1");
    CHECK(done.generation == h.service->generation(Slot::Dictation));
    REQUIRE(done.transcript.segments.size() == 1);
    CHECK(done.transcript.segments[0].end == 2.0);
    const auto calls = h.engine->calls();
    CHECK(calls.back().offset_s == 3.0);
    CHECK(calls.back().language == "en");
}

TEST_CASE("jobs run in lane order, then in submission order") {
    Harness h;
    h.load();
    h.engine->hold();
    auto first = h.submit(job(clip(audio(1)), Lane::Background));
    REQUIRE(h.engine->wait_entered(2s));
    std::vector<std::shared_ptr<std::optional<Outcome>>> results;
    for (const Lane lane : {Lane::Warmup, Lane::Preview, Lane::Incremental, Lane::Meeting,
                            Lane::Background, Lane::RemoteClient, Lane::Final, Lane::Final}) {
        results.push_back(h.submit(job(clip(audio(1)), lane)));
    }
    h.engine->resume();
    for (const auto& result : results) {
        h.wait(result);
    }
    CHECK(h.decode_lanes() == std::vector<Lane>{Lane::Background, Lane::Final, Lane::Final,
                                                Lane::RemoteClient, Lane::Meeting,
                                                Lane::Incremental, Lane::Preview, Lane::Background,
                                                Lane::Warmup});
}

TEST_CASE("a newer preview replaces the one still queued") {
    Harness h;
    h.load();
    h.engine->hold();
    auto running = h.submit(job(clip(audio(1)), Lane::Final));
    REQUIRE(h.engine->wait_entered(2s));
    std::vector<std::shared_ptr<std::optional<Outcome>>> previews;
    for (int i = 0; i < 3; ++i) {
        auto preview = job(clip(audio(1)), Lane::Preview);
        preview.coalesce_key = "preview:7";
        previews.push_back(h.submit(std::move(preview)));
    }
    REQUIRE(pump_until(h.notify, [&] { return previews[1]->has_value(); }));
    CHECK(canceled(*previews[0]));
    CHECK(canceled(*previews[1]));
    h.engine->resume();
    h.wait(previews[2]);
    CHECK((*previews[2])->has_value());
    CHECK(h.decode_lanes() == std::vector<Lane>{Lane::Final, Lane::Preview});
}

TEST_CASE("a long input is decoded one window at a time and a final job overtakes it") {
    Harness h;
    h.load();
    h.engine->hold();
    // 75 s of a constant: windows of 24.1 s, 24.1 s and the rest, as transcribe_audio cuts.
    auto long_job = h.submit(job(clip(audio(75.0), 100.0), Lane::Background));
    REQUIRE(h.engine->wait_entered(2s));
    auto final_job = h.submit(job(clip(audio(1)), Lane::Final));
    h.engine->resume();
    h.wait(long_job);
    h.wait(final_job);

    CHECK(h.decode_lanes() ==
          std::vector<Lane>{Lane::Background, Lane::Final, Lane::Background, Lane::Background});
    REQUIRE((*long_job)->has_value());
    const DecodeResult& done = ***long_job;
    CHECK(done.transcript.text == "w1 w3 w4");
    REQUIRE(done.transcript.segments.size() == 3);
    CHECK(done.transcript.segments[1].start == 385600.0 / 16000);
    CHECK(done.transcript.segments[2].start == 771200.0 / 16000);
    CHECK(done.transcript.audio_processed == 75.0);
    std::vector<std::size_t> sizes;
    std::vector<double> offsets;
    for (const auto& call : h.engine->calls()) {
        if (call.op == "decode" && call.lane == Lane::Background) {
            sizes.push_back(call.samples);
            offsets.push_back(call.offset_s);
        }
    }
    CHECK(sizes == std::vector<std::size_t>{385600, 385600, 428800});
    CHECK(offsets == std::vector<double>{100.0, 100.0 + 24.1, 100.0 + 48.2});
}

TEST_CASE("a cancelled job drops its queued windows and ends canceled") {
    Harness h;
    h.load();
    h.engine->hold();
    fv::speech::JobHandle handle;
    auto long_job = h.submit(job(clip(audio(75.0)), Lane::Background), &handle);
    REQUIRE(h.engine->wait_entered(2s));
    fv::speech::JobHandle queued_handle;
    auto queued = h.submit(job(clip(audio(1)), Lane::Preview), &queued_handle);
    queued_handle.cancel();
    // A queued job ends at once, without a decode.
    REQUIRE(pump_until(h.notify, [&] { return queued->has_value(); }));
    CHECK(canceled(*queued));
    handle.cancel();
    h.engine->resume();
    h.wait(long_job);
    CHECK(canceled(*long_job));
    CHECK(h.decode_lanes() == std::vector<Lane>{Lane::Background});
}

TEST_CASE("cancelling a job's token cancels the job") {
    Harness h;
    h.load();
    h.engine->hold();
    auto running = h.submit(job(clip(audio(1)), Lane::Final));
    REQUIRE(h.engine->wait_entered(2s));
    fv::CancelSource source;
    auto decode = job(clip(audio(1)), Lane::Final);
    decode.cancel = source.token();
    auto result = h.submit(std::move(decode));
    source.cancel();
    REQUIRE(pump_until(h.notify, [&] { return result->has_value(); }));
    CHECK(canceled(*result));

    fv::CancelSource already;
    already.cancel();
    auto early = job(clip(audio(1)));
    early.cancel = already.token();
    auto refused = h.submit(std::move(early));
    REQUIRE(pump_until(h.notify, [&] { return refused->has_value(); }));
    CHECK(canceled(*refused));
    h.engine->resume();
    h.wait(running);
}

// Section 4 rule 6: a backend's cancel callback (an upload abort) runs with no service lock
// held, so it may call back into the service.
TEST_CASE("a running decode is cancelled outside the service's lock") {
    const std::string how = GENERATE("handle", "reload", "cancel_all");
    Harness h;
    h.load();
    std::atomic<int> callbacks{0};
    FakeScript script;
    script.on_cancel = [&] {
        (void)h.service->readiness(Slot::Dictation);
        (void)h.service->lease_holders();
        ++callbacks;
    };
    h.engine->set_script(script);
    h.engine->hold();
    fv::speech::JobHandle handle;
    auto running = h.submit(job(clip(audio(1))), &handle);
    REQUIRE(h.engine->wait_entered(2s));
    if (how == "handle") {
        handle.cancel();
    } else if (how == "reload") {
        h.service->select(Slot::Dictation, parakeet_config());
    } else {
        std::thread resume([&] {
            std::this_thread::sleep_for(20ms);
            h.engine->resume();
        });
        h.service->cancel_all(2s);
        resume.join();
    }
    CHECK(callbacks == 1);
    h.engine->resume();
    h.wait(running);
    CHECK(canceled(*running));
}

TEST_CASE("a reload drops what the old engine had queued and the result it was decoding") {
    Harness h;
    h.load();
    h.engine->hold();
    auto running = h.submit(job(clip(audio(1)), Lane::Final));
    REQUIRE(h.engine->wait_entered(2s));
    auto queued = h.submit(job(clip(audio(1)), Lane::Final));
    const auto before = h.service->generation(Slot::Dictation);
    h.service->select(Slot::Dictation, parakeet_config());
    CHECK(h.service->generation(Slot::Dictation) == before + 1);
    REQUIRE(pump_until(h.notify, [&] { return queued->has_value(); }));
    CHECK(canceled(*queued));
    h.engine->resume();
    h.wait(running);
    CHECK(canceled(*running));
    REQUIRE(pump_until(h.notify, [&] {
        return h.service->readiness(Slot::Dictation).state == ReadyState::Ready;
    }));
    // The old engine was released before the new one loaded.
    CHECK(h.engine->ops() == std::vector<std::string>{"load", "decode", "release", "load"});
}

TEST_CASE("a lease holds a reload back until it is released") {
    Harness h;
    h.load();
    {
        fv::speech::EngineLease lease = h.service->lease("meeting");
        CHECK(h.service->lease_holders() == std::vector<std::string>{"meeting"});
        const auto generation = h.service->generation(Slot::Dictation);
        h.service->select(Slot::Dictation, parakeet_config("cpu"));
        CHECK(h.service->generation(Slot::Dictation) == generation);
        std::this_thread::sleep_for(20ms);
        CHECK(h.engine->created() == 1);
        // Moved, it is still one lease.
        fv::speech::EngineLease moved = std::move(lease);
        CHECK(h.service->lease_holders().size() == 1);
    }
    CHECK(h.service->lease_holders().empty());
    REQUIRE(pump_until(h.notify, [&] {
        return h.engine->created() == 2 &&
               h.service->readiness(Slot::Dictation).state == ReadyState::Ready;
    }));
    CHECK(h.service->readiness(Slot::Dictation).device_info == "Parakeet TDT 0.6B v3 | cpu");
}

TEST_CASE("release_local frees the engine and restore_local loads it again") {
    Harness h;
    h.load();
    h.service->release_local(Slot::Dictation);
    CHECK(h.service->readiness(Slot::Dictation).state == ReadyState::Offline);
    CHECK(h.service->readiness(Slot::Dictation).device_info == "Released while Meeting Mode runs");
    REQUIRE(h.engine->wait_calls(2, 2s));
    CHECK(h.engine->ops() == std::vector<std::string>{"load", "release"});
    auto refused = h.submit(job(clip(audio(1))));
    h.wait(refused);
    REQUIRE_FALSE((*refused)->has_value());
    CHECK(std::string((*refused)->error().what()) == "Released while Meeting Mode runs");

    h.service->restore_local(Slot::Dictation);
    REQUIRE(pump_until(h.notify, [&] {
        return h.service->readiness(Slot::Dictation).state == ReadyState::Ready;
    }));
    CHECK(h.engine->ops() == std::vector<std::string>{"load", "release", "load"});
    // A second restore has nothing to restore.
    h.service->restore_local(Slot::Dictation);
    CHECK(h.service->readiness(Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("the meeting slot shares the dictation engine until it gets its own") {
    Harness h;
    h.load();
    auto shared = job(clip(audio(1)), Lane::Meeting);
    shared.slot = Slot::Meeting;
    auto first = h.submit(std::move(shared));
    h.wait(first);
    CHECK(h.engine->calls().back().backend == 1);
    CHECK(h.service->readiness(Slot::Meeting).state == ReadyState::Ready);

    auto meeting = parakeet_config("cpu");
    meeting.model_key = "parakeet-v2";
    h.load(Slot::Meeting, meeting);
    auto own = job(clip(audio(1)), Lane::Meeting);
    own.slot = Slot::Meeting;
    auto second = h.submit(std::move(own));
    h.wait(second);
    CHECK(h.engine->calls().back().backend == 2);
    CHECK(h.service->readiness(Slot::Meeting).device_info == "Parakeet TDT 0.6B v3 | cpu");
    CHECK(h.service->readiness(Slot::Dictation).device_info == "Parakeet TDT 0.6B v3 | cuda");
}

TEST_CASE("warm-up runs only when nothing else is queued") {
    Harness h;
    h.load();
    h.service->warm_up(Slot::Dictation);
    REQUIRE(pump_until(h.notify, [&] { return !h.warmed.empty(); }));
    CHECK(h.warmed == std::vector<bool>{true});
    CHECK(h.engine->ops().back() == "warm_up");

    // Nothing loaded: nothing to warm.
    Harness empty;
    empty.service->warm_up(Slot::Dictation);
    REQUIRE(pump_until(empty.notify, [&] { return !empty.warmed.empty(); }));
    CHECK(empty.warmed == std::vector<bool>{false});
    CHECK(empty.engine->ops().empty());
}

TEST_CASE("a final job submitted during a warm-up runs right after it") {
    Harness h;
    h.load();
    h.engine->hold();
    h.service->warm_up(Slot::Dictation);
    REQUIRE(h.engine->wait_entered(2s));
    auto final_job = h.submit(job(clip(audio(1)), Lane::Final));
    h.engine->resume();
    h.wait(final_job);
    CHECK((*final_job)->has_value());
    CHECK(h.engine->ops() == std::vector<std::string>{"load", "warm_up", "decode"});
}

TEST_CASE("a failed load is reported and decodes say why") {
    FakeScript script;
    script.load_error = "The model file x does not exist.";
    Harness h(script);
    h.load();
    CHECK(h.readiness.back().second.state == ReadyState::Failed);
    CHECK(h.readiness.back().second.message == "The model file x does not exist.");
    auto result = h.submit(job(clip(audio(1))));
    h.wait(result);
    REQUIRE_FALSE((*result)->has_value());
    CHECK(std::string((*result)->error().what()) == "The model file x does not exist.");
}

TEST_CASE("an auto device that falls back to the cpu says so") {
    FakeScript script;
    script.unsupported_gpu = "No kernel image for this GPU.";
    Harness h(script);
    h.load(Slot::Dictation, parakeet_config("cuda", "auto"));
    const auto& readiness = h.readiness.back().second;
    CHECK(readiness.state == ReadyState::Ready);
    CHECK(readiness.device_info == "Parakeet TDT 0.6B v3 | cpu");
    CHECK(readiness.message == "Parakeet is using the CPU. No kernel image for this GPU.");
}

TEST_CASE("a decode failure ends only its job") {
    FakeScript script;
    script.fail_call = 1;
    Harness h(script);
    h.load();
    auto failed = h.submit(job(clip(audio(1))));
    auto next = h.submit(job(clip(audio(1))));
    h.wait(failed);
    h.wait(next);
    REQUIRE_FALSE((*failed)->has_value());
    CHECK(std::string((*failed)->error().what()) == "Fake decode failed");
    REQUIRE((*next)->has_value());
    CHECK((**next)->transcript.text == "w2");
}

TEST_CASE("run blocks a worker thread until the result is in") {
    Harness h;
    h.load();
    auto result =
        std::async(std::launch::async, [&] { return h.service->run(job(clip(audio(1)))); });
    CHECK(result.get().transcript.text == "w1");

    FakeScript failing;
    failing.decode_error = "Fake decode failed";
    h.engine->set_script(failing);
    auto thrown =
        std::async(std::launch::async, [&] { return h.service->run(job(clip(audio(1)))); });
    CHECK_THROWS_AS(thrown.get(), fv::EngineError);
}

TEST_CASE("cancel_all ends every queued job and waits for the running window") {
    Harness h;
    h.load();
    FakeScript slow;
    slow.decode_delay = 50ms;
    h.engine->set_script(slow);
    auto running = h.submit(job(clip(audio(1))));
    REQUIRE(h.engine->wait_calls(2, 2s));
    auto queued = h.submit(job(clip(audio(1)), Lane::Preview));
    h.service->cancel_all(2s);
    h.wait(running);
    h.wait(queued);
    CHECK(canceled(*running));
    CHECK(canceled(*queued));
    CHECK(h.decode_lanes() == std::vector<Lane>{Lane::Final});
}

TEST_CASE("destroying the service cancels its jobs and releases its engines") {
    auto h = std::make_unique<Harness>();
    h->load();
    auto engine = h->engine;
    h->engine->hold();
    auto running = h->submit(job(clip(audio(1))));
    REQUIRE(h->engine->wait_entered(2s));
    auto queued = h->submit(job(clip(audio(1))));
    std::thread opener([engine] {
        std::this_thread::sleep_for(20ms);
        engine->resume();
    });
    h->service.reset();
    opener.join();
    h->notify.run_all();
    CHECK(canceled(*running));
    CHECK(canceled(*queued));
    CHECK(engine->ops().back() == "release");
}

TEST_CASE("decode_file decodes a file window by window on the service") {
    Harness h;
    h.load();
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "long.wav";
    // 70 s with quiet blocks: windows at 0, 24.8 and 50.1 s.
    const auto samples = fv::speech::test::stream_signal("gaps", 70 * 16000, 11);
    fv::speech::test::write_wav(path, 16000, 1, samples);
    fv::RecognitionOptions options;
    options.language = "es";
    auto decoded = std::async(std::launch::async, [&] {
        return fv::speech::decode_file(*h.service, path, options, Lane::Final);
    });
    const DecodeResult result = decoded.get();
    CHECK(result.transcript.text == "w1 w2 w3");
    REQUIRE(result.transcript.segments.size() == 3);
    CHECK(result.transcript.segments[1].start == 24.8);
    CHECK(result.transcript.segments[2].start == 50.1);
    CHECK(std::abs(result.transcript.audio_processed - 70.0) < 1e-9);
    for (const auto& call : h.engine->calls()) {
        if (call.op == "decode") {
            CHECK(call.language == "es");
            CHECK(call.lane == Lane::Final);
        }
    }
    CHECK(result.generation == h.service->generation(Slot::Dictation));
}

TEST_CASE("decode_file stops when it is cancelled") {
    Harness h;
    h.load();
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "long.wav";
    fv::speech::test::write_wav(path, 16000, 1,
                                fv::speech::test::stream_signal("gaps", 70 * 16000, 11));
    h.engine->hold();
    fv::CancelSource cancel;
    auto decoded = std::async(std::launch::async, [&] {
        return fv::speech::decode_file(*h.service, path, {}, Lane::Final, cancel.token());
    });
    REQUIRE(h.engine->wait_entered(2s));
    cancel.cancel();
    h.engine->resume();
    CHECK_THROWS_WITH(decoded.get(), "Transcription canceled");
    CHECK(h.decode_lanes().size() == 1);
}

TEST_CASE("cache removal cannot enter while the matching speech backend is loading") {
    fv::ManualExecutor notify;
    const auto fake = std::make_shared<FakeEngine>();
    std::promise<void> loadEntered;
    std::promise<void> releaseLoad;
    auto loadReady = loadEntered.get_future();
    auto gate = releaseLoad.get_future().share();
    const auto backend = fake->factory();
    fv::speech::SpeechService service(
        [&loadEntered, backend, gate](const fv::speech::BackendConfig& config) {
            loadEntered.set_value();
            gate.wait();
            return backend(config);
        },
        notify);
    service.select(Slot::Meeting, parakeet_config());
    const auto began = loadReady.wait_for(2s);

    std::atomic<bool> deleted = false;
    std::promise<void> removalEntered;
    auto attempted = removalEntered.get_future();
    auto removal = std::async(std::launch::async, [&] {
        removalEntered.set_value();
        return service.with_model_unused("parakeet-v3", [&] { deleted = true; });
    });
    const auto attemptedStatus = attempted.wait_for(2s);
    const auto whileLoading = removal.wait_for(100ms);
    releaseLoad.set_value();

    REQUIRE(began == std::future_status::ready);
    REQUIRE(attemptedStatus == std::future_status::ready);
    CHECK(whileLoading == std::future_status::timeout);
    CHECK_FALSE(removal.get());
    CHECK_FALSE(deleted.load());
    CHECK(service.readiness(Slot::Meeting).state == ReadyState::Ready);
}

TEST_CASE("a newly selected backend waits for an earlier cache removal to finish") {
    fv::ManualExecutor notify;
    const auto fake = std::make_shared<FakeEngine>();
    fv::speech::SpeechService service(fake->factory(), notify);
    std::promise<void> removalEntered;
    std::promise<void> finishRemoval;
    auto started = removalEntered.get_future();
    auto gate = finishRemoval.get_future().share();
    auto removal = std::async(std::launch::async, [&] {
        return service.with_model_unused("parakeet-v3", [&] {
            removalEntered.set_value();
            gate.wait();
        });
    });
    const auto began = started.wait_for(2s);
    service.select(Slot::Dictation, parakeet_config());
    const bool enteredDuringRemoval = fake->wait_calls(1, 100ms);
    finishRemoval.set_value();

    REQUIRE(began == std::future_status::ready);
    REQUIRE(removal.get());
    CHECK_FALSE(enteredDuringRemoval);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state == ReadyState::Ready; }));
    CHECK_FALSE(service.with_model_unused("parakeet-v3", [] {}));
}

TEST_CASE("a model selected behind a lease cannot be removed before its queued load") {
    Harness h;
    auto lease = h.service->lease("fixture");
    auto selected = parakeet_config();
    selected.model_key = "queued-model";
    h.service->select(Slot::Meeting, std::move(selected));
    CHECK_FALSE(h.service->with_model_unused("queued-model", [] {}));
    CHECK(h.service->with_model_unused("another-model", [] {}));
}
