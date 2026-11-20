#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hotkeys/dispatcher.h"

using fv::hotkeys::monotonic_clock;
using fv::hotkeys::OrderedDispatcher;
using namespace std::chrono_literals;

namespace {

// threading.Event.
class Event {
public:
    void set() {
        {
            std::lock_guard lock(mutex_);
            set_ = true;
        }
        cv_.notify_all();
    }
    void clear() {
        std::lock_guard lock(mutex_);
        set_ = false;
    }
    bool wait(std::chrono::duration<double> timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return set_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool set_ = false;
};

// A list the worker appends to while the test reads it.
class Seen {
public:
    void append(std::string item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }
    std::vector<std::string> items() {
        std::lock_guard lock(mutex_);
        return items_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> items_;
};

bool wait_until(const std::function<bool()>& condition,
                std::chrono::duration<double> timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

double now() {
    return monotonic_clock()();
}

} // namespace

// from tests/test_personalize_s6_runtime.py::test_dispatcher_runs_calls_in_order_on_one_thread
TEST_CASE("test_dispatcher_runs_calls_in_order_on_one_thread", "[test_personalize_s6_runtime]") {
    OrderedDispatcher dispatcher("test-dispatch");
    std::mutex mutex;
    std::vector<std::pair<int, std::thread::id>> seen;
    for (int index = 0; index < 50; ++index) {
        dispatcher.submit([&, index] {
            std::lock_guard lock(mutex);
            seen.emplace_back(index, std::this_thread::get_id());
        });
    }
    Event done;
    dispatcher.submit([&done] { done.set(); });
    CHECK(done.wait(2s));
    dispatcher.stop();

    std::lock_guard lock(mutex);
    REQUIRE(seen.size() == 50);
    std::set<std::thread::id> threads;
    for (int index = 0; index < 50; ++index) {
        CHECK(seen[static_cast<std::size_t>(index)].first == index);
        threads.insert(seen[static_cast<std::size_t>(index)].second);
    }
    CHECK(threads.size() == 1);
    CHECK_FALSE(threads.contains(std::this_thread::get_id()));
}

// from
// tests/test_personalize_s6_runtime.py::test_dispatcher_deadline_waits_for_queued_calls_and_can_be_replaced
TEST_CASE("test_dispatcher_deadline_waits_for_queued_calls_and_can_be_replaced",
          "[test_personalize_s6_runtime]") {
    OrderedDispatcher dispatcher("test-deadline");
    Seen seen;
    // Hold the worker first, so no deadline can fire between the calls that replace or cancel
    // it, however late this thread is scheduled.
    Event gate;
    dispatcher.submit([&gate] { gate.wait(5s); });
    dispatcher.call_at(now(), [&seen] { seen.append("first deadline"); });
    dispatcher.call_at(now() + 0.05, [&seen] { seen.append("deadline"); });
    dispatcher.submit([&seen] { seen.append("queued"); });
    std::this_thread::sleep_for(100ms);
    gate.set();
    CHECK(wait_until([&seen] { return seen.items().size() == 2; }));
    gate.clear();
    dispatcher.submit([&gate] { gate.wait(5s); });
    dispatcher.call_at(now(), [&seen] { seen.append("canceled"); });
    dispatcher.cancel_deadline();
    gate.set();
    std::this_thread::sleep_for(150ms);
    dispatcher.stop();

    CHECK(seen.items() == std::vector<std::string>{"queued", "deadline"});
}

// from
// tests/test_personalize_s6_runtime.py::test_dispatcher_survives_a_failing_call_and_restarts_after_stop
TEST_CASE("test_dispatcher_survives_a_failing_call_and_restarts_after_stop",
          "[test_personalize_s6_runtime]") {
    OrderedDispatcher dispatcher("test-restart");
    Seen seen;
    dispatcher.submit([] { throw std::runtime_error("division by zero"); });
    dispatcher.submit([&seen] { seen.append("after failure"); });
    CHECK(
        wait_until([&seen] { return seen.items() == std::vector<std::string>{"after failure"}; }));
    dispatcher.stop();
    dispatcher.submit([&seen] { seen.append("after restart"); });
    CHECK(wait_until([&seen] {
        const auto items = seen.items();
        return !items.empty() && items.back() == "after restart";
    }));
    dispatcher.stop();
}
