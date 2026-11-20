// SpeechService's waits around a model load, and readiness posted outside the service's lock.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "base/executor.h"
#include "base/testing/manual_executor.h"
#include "speech/service.h"
#include "speech_test_support.h"

using fv::speech::Backend;
using fv::speech::Capabilities;
using fv::speech::DecodeJob;
using fv::speech::DecodeResult;
using fv::speech::Readiness;
using fv::speech::ReadyState;
using fv::speech::Slot;
using fv::speech::SpeechService;
using fv::speech::test::parakeet_config;
using fv::speech::test::pump_until;
using namespace std::chrono_literals;

namespace {

// A load that says when it started and waits for `release` (or its cancel token).
struct LoadGate {
    std::promise<void> entered;
    std::promise<void> release; // before `released`, which is made from it
    std::shared_future<void> released;
    std::atomic<bool> cancelled{false};
    std::atomic<int> loads{0};
    LoadGate() : released(release.get_future().share()) {}
};

class GatedBackend final : public Backend {
public:
    explicit GatedBackend(std::shared_ptr<LoadGate> gate) : gate_(std::move(gate)) {}
    std::string_view id() const override { return "parakeet"; }
    Capabilities capabilities() const override { return {}; }
    Readiness load(const fv::CancelToken& cancel) override {
        if (gate_->loads++ == 0) {
            gate_->entered.set_value();
        }
        while (gate_->released.wait_for(5ms) != std::future_status::ready) {
            if (cancel.cancelled()) {
                gate_->cancelled = true;
                return Readiness{ReadyState::Failed, "canceled", "canceled"};
            }
        }
        return Readiness{ReadyState::Ready, "Parakeet | cpu", ""};
    }
    DecodeResult decode(const DecodeJob&) override { return {}; }
    void release() override {}

private:
    std::shared_ptr<LoadGate> gate_;
};

fv::speech::BackendFactory gated(std::shared_ptr<LoadGate> gate) {
    return [gate](const fv::speech::BackendConfig&) {
        return std::make_unique<GatedBackend>(gate);
    };
}

} // namespace

TEST_CASE("cancel_all waits for a model load as it does for a decode window") {
    fv::ManualExecutor notify;
    auto gate = std::make_shared<LoadGate>();
    SpeechService service(gated(gate), notify);
    service.select(Slot::Dictation, parakeet_config());
    REQUIRE(gate->entered.get_future().wait_for(2s) == std::future_status::ready);

    auto waited = std::async(std::launch::async, [&service] { service.cancel_all(5s); });
    const bool returnedWhileLoading = waited.wait_for(150ms) == std::future_status::ready;
    gate->release.set_value();
    const bool returnedAfterLoad = waited.wait_for(2s) == std::future_status::ready;

    CHECK_FALSE(returnedWhileLoading);
    CHECK(returnedAfterLoad);
    // The load was waited for, not cut short.
    CHECK_FALSE(gate->cancelled.load());
    CHECK(service.readiness(Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("cancel_all gives up on a load at its budget") {
    fv::ManualExecutor notify;
    auto gate = std::make_shared<LoadGate>();
    SpeechService service(gated(gate), notify);
    service.select(Slot::Dictation, parakeet_config());
    REQUIRE(gate->entered.get_future().wait_for(2s) == std::future_status::ready);
    const auto started = std::chrono::steady_clock::now();
    service.cancel_all(100ms);
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(took >= 100ms);
    CHECK(took < 2s);
    gate->release.set_value();
}

TEST_CASE("destroying the service stops a model load running on its thread") {
    fv::ManualExecutor notify;
    auto gate = std::make_shared<LoadGate>();
    auto service = std::make_unique<SpeechService>(gated(gate), notify);
    service->select(Slot::Dictation, parakeet_config());
    REQUIRE(gate->entered.get_future().wait_for(2s) == std::future_status::ready);

    auto destroyed = std::async(std::launch::async, [&service] { service.reset(); });
    const bool joined = destroyed.wait_for(2s) == std::future_status::ready;
    gate->release.set_value(); // lets a load that was not told to stop end, so the test ends
    destroyed.get();

    CHECK(joined);
    CHECK(gate->cancelled.load());
}

namespace {

// Posts to a queue, and on each post made from the test's thread asks another thread whether the
// service's state can be read meanwhile: a post made under the service's lock could not be.
class ProbingExecutor final : public fv::Executor {
public:
    void post(fv::Task task) override {
        if (service != nullptr && std::this_thread::get_id() == owner) {
            auto read = std::async(std::launch::async, [target = service] {
                return target->readiness(Slot::Dictation);
            });
            if (read.wait_for(1s) != std::future_status::ready) {
                ++blocked;
            }
            pending.push_back(std::move(read));
        }
        queue.post(std::move(task));
    }
    fv::TimerHandle post_after(std::chrono::milliseconds delay, fv::Task task) override {
        return queue.post_after(delay, std::move(task));
    }
    bool is_current() const override { return queue.is_current(); }

    SpeechService* service = nullptr;
    std::thread::id owner = std::this_thread::get_id();
    int blocked = 0;
    std::vector<std::future<Readiness>> pending;
    fv::ManualExecutor queue;
};

} // namespace

TEST_CASE("readiness is posted after the service's lock is released, in order") {
    ProbingExecutor notify;
    auto gate = std::make_shared<LoadGate>();
    gate->release.set_value();
    SpeechService service(gated(gate), notify);
    notify.service = &service;
    std::vector<ReadyState> seen;
    const auto subscription = service.readiness_changed.connect(
        notify.queue, [&seen](Slot, Readiness readiness) { seen.push_back(readiness.state); });

    service.select(Slot::Dictation, parakeet_config());
    REQUIRE(pump_until(notify.queue, [&] { return seen.size() >= 2; }));
    service.release_local(Slot::Dictation);
    service.restore_local(Slot::Dictation);
    notify.service = nullptr;
    REQUIRE(pump_until(notify.queue, [&] { return seen.size() >= 5; }));

    CHECK(notify.blocked == 0);
    // Loading reaches the subscribers before the load's Ready, each time.
    CHECK(seen == std::vector<ReadyState>{ReadyState::Loading, ReadyState::Ready,
                                          ReadyState::Offline, ReadyState::Loading,
                                          ReadyState::Ready});
}
