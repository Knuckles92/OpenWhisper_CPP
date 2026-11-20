// Signal delivery with Qt::AutoConnection semantics (architecture 3.2), and the fakes it is
// tested with.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "base/executor.h"
#include "base/signal.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"

using namespace std::chrono_literals;
using fv::FakeClock;
using fv::ManualExecutor;
using fv::Signal;

TEST_CASE("a subscriber whose executor is current runs inline, in connection order") {
    ManualExecutor ui;
    Signal<int, std::string> changed;
    std::vector<std::string> calls;
    auto first = changed.connect(
        ui, [&](int n, std::string s) { calls.push_back("first " + std::to_string(n) + s); });
    auto second = changed.connect(
        ui, [&](int n, std::string s) { calls.push_back("second " + std::to_string(n) + s); });

    ui.post([&] {
        changed.emit(7, "!");
        calls.push_back("after emit");
    });
    ui.run_all();

    CHECK(calls == std::vector<std::string>{"first 7!", "second 7!", "after emit"});
}

TEST_CASE("a subscriber on another executor is posted, in connection order") {
    ManualExecutor ui;
    ManualExecutor worker;
    Signal<int> changed;
    std::vector<int> calls;
    auto a = changed.connect(worker, [&](int n) { calls.push_back(n); });
    auto b = changed.connect(worker, [&](int n) { calls.push_back(n * 10); });

    changed.emit(3); // from the test thread: no executor is current
    CHECK(calls.empty());
    CHECK(worker.queued() == 2);
    worker.run_all();
    CHECK(calls == std::vector<int>{3, 30});
}

TEST_CASE("a posted call carries a copy of the arguments") {
    ManualExecutor worker;
    Signal<std::string> changed;
    std::string seen;
    auto subscription = changed.connect(worker, [&](std::string s) { seen = s; });
    {
        std::string text = "dictated";
        changed.emit(text);
        text = "changed";
    }
    worker.run_all();
    CHECK(seen == "dictated");
}

TEST_CASE("a disconnected subscriber is never started again, even with a call already posted") {
    ManualExecutor worker;
    Signal<> ping;
    int calls = 0;
    auto subscription = ping.connect(worker, [&] { ++calls; });
    ping.emit();
    CHECK(subscription.connected());
    subscription.disconnect();
    CHECK_FALSE(subscription.connected());
    worker.run_all();
    ping.emit();
    worker.run_all();
    CHECK(calls == 0);
    CHECK(ping.subscriber_count() == 0);
}

TEST_CASE("destroying the subscription disconnects; a subscriber may disconnect itself") {
    ManualExecutor ui;
    Signal<> ping;
    int calls = 0;
    fv::Subscription self;
    self = ping.connect(ui, [&] {
        ++calls;
        self.disconnect();
    });
    {
        auto scoped = ping.connect(ui, [&] { calls += 100; });
    }
    ui.post([&] {
        ping.emit();
        ping.emit();
    });
    ui.run_all();
    CHECK(calls == 1);
}

TEST_CASE("a subscription outliving its signal disconnects quietly") {
    ManualExecutor ui;
    fv::Subscription subscription;
    {
        Signal<int> changed;
        subscription = changed.connect(ui, [](int) {});
    }
    subscription.disconnect();
    CHECK_FALSE(subscription.connected());
}

TEST_CASE("disconnect waits for a call running on another thread") {
    fv::ThreadPool pool("fv-test-pool", 1);
    Signal<> ping;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    auto subscription = ping.connect(pool, [&] {
        started = true;
        std::this_thread::sleep_for(50ms);
        finished = true;
    });
    ping.emit();
    while (!started.load()) {
        std::this_thread::yield();
    }
    subscription.disconnect();
    CHECK(finished);
}

TEST_CASE("a manual executor is current only while it runs a task") {
    ManualExecutor executor;
    bool inside = false;
    executor.post([&] { inside = executor.is_current(); });
    CHECK_FALSE(executor.is_current());
    CHECK(executor.run_all() == 1);
    CHECK(inside);
    CHECK_FALSE(executor.is_current());
}

TEST_CASE("advance runs due timers in deadline order, each at its deadline") {
    ManualExecutor executor;
    std::vector<std::string> calls;
    fv::TimerHandle tick;
    std::function<void()> rearm = [&] {
        calls.push_back("tick");
        tick = executor.post_after(100ms, rearm);
    };
    tick = executor.post_after(100ms, rearm);
    auto once = executor.post_after(150ms, [&] { calls.push_back("once"); });
    auto never = executor.post_after(400ms, [&] { calls.push_back("never"); });

    executor.advance(250ms);

    CHECK(calls == std::vector<std::string>{"tick", "once", "tick"});
    CHECK(never.active());
    never.cancel();
    executor.advance(1000ms);
    CHECK(std::count(calls.begin(), calls.end(), "never") == 0);
    tick.cancel();
}

TEST_CASE("a fake clock drives the timers of a manual executor built on it") {
    FakeClock clock;
    ManualExecutor executor(clock);
    const auto start = clock.steady();
    const auto wall = clock.utc();
    std::vector<std::chrono::milliseconds> fired_at;
    auto a = executor.post_after(30ms, [&] {
        fired_at.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock.steady() - start));
    });
    auto b = executor.post_after(10ms, [&] {
        fired_at.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock.steady() - start));
    });

    clock.advance(20ms); // moved by the test, not the executor
    CHECK(fired_at == std::vector<std::chrono::milliseconds>{20ms});

    executor.advance(15ms);
    CHECK(fired_at == std::vector<std::chrono::milliseconds>{20ms, 30ms});
    CHECK(clock.steady() - start == 35ms);
    CHECK(clock.utc() - wall == 35ms);
    CHECK(clock.zone() == std::chrono::locate_zone("UTC")); // MSVC names it "Etc/UTC"
}

TEST_CASE("a fake clock's wall time can jump without moving steady time") {
    FakeClock clock(FakeClock::default_utc(), "Europe/Rome");
    const auto steady = clock.steady();
    clock.set_utc(FakeClock::default_utc() + std::chrono::hours(5));
    CHECK(clock.steady() == steady);
    CHECK(clock.utc() - FakeClock::default_utc() == std::chrono::hours(5));
    CHECK(clock.zone()->name() == "Europe/Rome");
}

TEST_CASE("destroying a manual executor cancels its timers") {
    fv::TimerHandle handle;
    {
        ManualExecutor executor;
        handle = executor.post_after(10ms, [] {});
        CHECK(handle.active());
    }
    CHECK_FALSE(handle.active());
}
