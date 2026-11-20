// ThreadPool, Strand and TimerHandle (architecture 3.2).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "base/executor.h"

using namespace std::chrono_literals;
using fv::ShutdownMode;
using fv::Strand;
using fv::ThreadPool;

namespace {

// A gate tasks wait on until the test opens it.
class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

// Waits until `done` holds, polling; false after the timeout.
template <class Fn> bool eventually(Fn done, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Occupies one of pool's threads with a task waiting on gate, returning once it runs: a
// timer due meanwhile waits for the gate too.
void hold(ThreadPool& pool, Gate& gate) {
    std::atomic<bool> entered{false};
    pool.post([&entered, &gate] {
        entered = true;
        gate.wait();
    });
    REQUIRE(eventually([&] { return entered.load(); }));
}

struct Recorder {
    std::mutex mutex;
    std::vector<int> order;
    void add(int value) {
        std::lock_guard lock(mutex);
        order.push_back(value);
    }
    std::vector<int> get() {
        std::lock_guard lock(mutex);
        return order;
    }
};

void raise_to(std::atomic<int>& most, int value) {
    int seen = most.load();
    while (seen < value && !most.compare_exchange_weak(seen, value)) {
    }
}

} // namespace

TEST_CASE("a pool runs posted tasks and knows its own threads") {
    ThreadPool pool("fv-test-pool", 2);
    std::atomic<int> ran{0};
    std::atomic<bool> current_inside{false};
    for (int i = 0; i < 20; ++i) {
        pool.post([&] {
            if (pool.is_current()) {
                current_inside = true;
            }
            ++ran;
        });
    }
    REQUIRE(eventually([&] { return ran.load() == 20; }));
    CHECK(current_inside);
    CHECK_FALSE(pool.is_current());
}

TEST_CASE("a strand of one runs its tasks in posting order, one at a time") {
    ThreadPool pool("fv-test-pool", 4);
    Strand strand(pool, "fv-test-serial", 1, ShutdownMode::Drain);
    Recorder recorder;
    std::atomic<int> running{0};
    std::atomic<int> most{0};
    std::atomic<bool> always_current{true};
    for (int i = 0; i < 50; ++i) {
        strand.post([&, i] {
            const int now = ++running;
            raise_to(most, now);
            if (!strand.is_current()) {
                always_current = false;
            }
            std::this_thread::sleep_for(100us);
            recorder.add(i);
            --running;
        });
    }
    REQUIRE(eventually([&] { return recorder.get().size() == 50; }));
    std::vector<int> expected(50);
    for (int i = 0; i < 50; ++i) {
        expected[static_cast<std::size_t>(i)] = i;
    }
    CHECK(recorder.get() == expected);
    CHECK(most.load() == 1);
    CHECK(always_current);
    CHECK_FALSE(strand.is_current());
}

TEST_CASE("a strand of two runs two tasks at once and never three") {
    ThreadPool pool("fv-test-pool", 4);
    Strand strand(pool, "fv-test-jobs", 2, ShutdownMode::CancelQueued);
    Gate gate;
    std::atomic<int> running{0};
    std::atomic<int> most{0};
    std::atomic<int> done{0};
    for (int i = 0; i < 6; ++i) {
        strand.post([&] {
            const int now = ++running;
            raise_to(most, now);
            gate.wait();
            --running;
            ++done;
        });
    }
    REQUIRE(eventually([&] { return running.load() == 2; }));
    std::this_thread::sleep_for(20ms); // a third would have started by now
    CHECK(running.load() == 2);
    gate.open();
    REQUIRE(eventually([&] { return done.load() == 6; }));
    CHECK(most.load() == 2);
}

TEST_CASE("cancel-queued shutdown drops the queued tasks and lets the running one finish") {
    ThreadPool pool("fv-test-pool", 2);
    Strand strand(pool, "fv-test-jobs", 1, ShutdownMode::CancelQueued);
    Gate gate;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<int> queued_ran{0};
    strand.post([&] {
        started = true;
        gate.wait();
        finished = true;
    });
    for (int i = 0; i < 5; ++i) {
        strand.post([&] { ++queued_ran; });
    }
    REQUIRE(eventually([&] { return started.load(); }));
    std::thread opener([&] {
        std::this_thread::sleep_for(30ms);
        gate.open();
    });
    strand.shutdown(5s);
    opener.join();
    CHECK(finished);
    strand.post([&] { ++queued_ran; }); // after shutdown: dropped
    std::this_thread::sleep_for(20ms);
    CHECK(queued_ran.load() == 0);
}

TEST_CASE("drain shutdown runs the queued tasks within the budget") {
    ThreadPool pool("fv-test-pool", 2);
    Strand strand(pool, "fv-test-persistence", 1, ShutdownMode::Drain);
    Recorder recorder;
    for (int i = 0; i < 10; ++i) {
        strand.post([&, i] {
            std::this_thread::sleep_for(1ms);
            recorder.add(i);
        });
    }
    strand.shutdown(5s);
    CHECK(recorder.get().size() == 10);
}

TEST_CASE("drain shutdown drops what the budget does not reach") {
    ThreadPool pool("fv-test-pool", 2);
    Strand strand(pool, "fv-test-persistence", 1, ShutdownMode::Drain);
    std::atomic<int> ran{0};
    for (int i = 0; i < 20; ++i) {
        strand.post([&] {
            std::this_thread::sleep_for(20ms);
            ++ran;
        });
    }
    const auto start = std::chrono::steady_clock::now();
    strand.shutdown(50ms);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took < 1s);
    const int at_shutdown = ran.load();
    CHECK(at_shutdown < 20);
    std::this_thread::sleep_for(100ms);
    CHECK(ran.load() <= at_shutdown + 1); // only the task running at the deadline may still end
}

TEST_CASE("a pool's shutdown drains within the budget and then refuses work") {
    ThreadPool pool("fv-test-pool", 2);
    std::atomic<int> ran{0};
    for (int i = 0; i < 10; ++i) {
        pool.post([&] { ++ran; });
    }
    pool.shutdown(5s);
    CHECK(ran.load() == 10);
    pool.post([&] { ++ran; });
    CHECK_FALSE(pool.post_after(0ms, [&] { ++ran; }).active());
    std::this_thread::sleep_for(20ms);
    CHECK(ran.load() == 10);
}

TEST_CASE("timers fire after their delay, in deadline order") {
    ThreadPool pool("fv-test-pool", 1);
    Recorder recorder;
    // The pool's one thread waits on the gate, so no timer can fire before it opens however
    // slowly this thread runs; the deadlines are far enough apart that posting late first
    // still gives it the later deadline under load.
    Gate gate;
    hold(pool, gate);
    auto late = pool.post_after(600ms, [&] { recorder.add(2); });
    auto early = pool.post_after(20ms, [&] { recorder.add(1); });
    CHECK(late.active());
    CHECK(early.active());
    gate.open();
    REQUIRE(eventually([&] { return recorder.get().size() == 2; }));
    CHECK(recorder.get() == std::vector<int>{1, 2});
    CHECK_FALSE(late.active());
    CHECK_FALSE(early.active());
}

TEST_CASE("a timer handle cancels its timer when it is destroyed") {
    ThreadPool pool("fv-test-pool", 1);
    std::atomic<int> ran{0};
    {
        auto handle = pool.post_after(30ms, [&] { ++ran; });
        CHECK(handle.active());
    }
    auto cancelled = pool.post_after(30ms, [&] { ++ran; });
    cancelled.cancel();
    CHECK_FALSE(cancelled.active());
    auto detached = pool.post_after(30ms, [&] { ran += 10; });
    detached.detach();
    CHECK_FALSE(detached.active());
    REQUIRE(eventually([&] { return ran.load() == 10; }));
    std::this_thread::sleep_for(50ms);
    CHECK(ran.load() == 10);
}

TEST_CASE("a moved timer handle keeps the timer; assigning over one cancels it") {
    ThreadPool pool("fv-test-pool", 1);
    std::atomic<int> ran{0};
    // No timer fires while the pool's one thread waits on the gate.
    Gate gate;
    hold(pool, gate);
    fv::TimerHandle kept = pool.post_after(20ms, [&] { ran += 1; });
    fv::TimerHandle moved = std::move(kept);
    CHECK_FALSE(kept.active());
    CHECK(moved.active());
    moved = pool.post_after(20ms, [&] { ran += 100; }); // the first is cancelled
    gate.open();
    // The first timer's deadline is the earlier, so had it survived it would have run first.
    REQUIRE(eventually([&] { return ran.load() >= 100; }));
    CHECK(ran.load() == 100);
    std::this_thread::sleep_for(40ms);
    CHECK(ran.load() == 100);
}

TEST_CASE("strand timers run on the strand and can be cancelled until they run") {
    ThreadPool pool("fv-test-pool", 2);
    Strand strand(pool, "fv-test-serial", 1, ShutdownMode::CancelQueued);
    std::atomic<bool> on_strand{false};
    std::atomic<int> ran{0};
    auto fired = strand.post_after(10ms, [&] {
        on_strand = strand.is_current();
        ++ran;
    });
    auto cancelled = strand.post_after(10ms, [&] { ran += 100; });
    cancelled.cancel();
    REQUIRE(eventually([&] { return ran.load() == 1; }));
    CHECK(on_strand);
    std::this_thread::sleep_for(30ms);
    CHECK(ran.load() == 1);
}

TEST_CASE("strand shutdown cancels its timers") {
    ThreadPool pool("fv-test-pool", 2);
    Strand strand(pool, "fv-test-serial", 1, ShutdownMode::CancelQueued);
    std::atomic<int> ran{0};
    auto timer = strand.post_after(30ms, [&] { ++ran; });
    strand.shutdown(1s);
    CHECK_FALSE(timer.active());
    std::this_thread::sleep_for(60ms);
    CHECK(ran.load() == 0);
}

TEST_CASE("a throwing task does not stop the pool") {
    ThreadPool pool("fv-test-pool", 1);
    std::atomic<bool> after{false};
    pool.post([] { throw std::runtime_error("boom"); });
    pool.post([&] { after = true; });
    CHECK(eventually([&] { return after.load(); }));
}

TEST_CASE("destroying a strand waits for its running task") {
    ThreadPool pool("fv-test-pool", 2);
    std::atomic<bool> finished{false};
    {
        Strand strand(pool, "fv-test-serial", 1, ShutdownMode::Drain);
        std::atomic<bool> started{false};
        strand.post([&] {
            started = true;
            std::this_thread::sleep_for(30ms);
            finished = true;
        });
        REQUIRE(eventually([&] { return started.load(); }));
    }
    CHECK(finished);
}

TEST_CASE("the GUI thread is the one that marked itself") {
    std::atomic<bool> other_is_gui{true};
    fv::mark_gui_thread();
    CHECK(fv::is_gui_thread());
    std::thread other([&] { other_is_gui = fv::is_gui_thread(); });
    other.join();
    CHECK_FALSE(other_is_gui);
    std::thread([] { fv::mark_gui_thread(); }).join(); // leave no thread of this test marked
    CHECK_FALSE(fv::is_gui_thread());
    fv::assert_not_gui_thread(); // not the GUI thread: returns
}
