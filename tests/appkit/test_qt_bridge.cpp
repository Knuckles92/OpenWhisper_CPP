// The Qt bridge: qt::post, TokenGate and QtUiExecutor (architecture 3.11), which replace
// OpenWhisper's qt_thread_callback (ui_qt/bootstrap.py) and its relay-with-token classes.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include "appkit/qt_bridge.h"
#include "base/log.h"

using namespace std::chrono_literals;

namespace {

void process_events() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

class CaptureSink final : public fv::log::Sink {
public:
    struct Entry {
        fv::log::Level level;
        std::string logger;
        std::string message;
        std::string exception_type;
    };

    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        entries_.push_back(Entry{record.level, std::string(record.logger),
                                 std::string(record.message), std::string(record.exception_type)});
    }

    std::vector<Entry> entries() const {
        std::lock_guard lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

// Adds the sink for the scope of a test.
struct ScopedSink {
    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    ScopedSink() { fv::log::add_sink(sink); }
    ~ScopedSink() { fv::log::remove_sink(sink); }
};

} // namespace

TEST_CASE("qt::post runs on the context's thread, queued") {
    QObject context;
    QThread* ran_on = nullptr;
    bool ran = false;
    fv::qt::post(&context, [&] {
        ran = true;
        ran_on = QThread::currentThread();
    });
    CHECK_FALSE(ran); // queued even when posted from the context's own thread

    std::thread worker([&] { fv::qt::post(&context, [&] { ran_on = QThread::currentThread(); }); });
    worker.join();
    REQUIRE(QTest::qWaitFor([&] { return ran; }, 5000));
    process_events();
    CHECK(ran_on == context.thread());
}

TEST_CASE("qt::post drops work for a destroyed context") {
    auto context = std::make_unique<QObject>();
    bool ran = false;
    std::thread worker([&] { fv::qt::post(context.get(), [&] { ran = true; }); });
    worker.join();
    context.reset();
    process_events();
    CHECK_FALSE(ran);

    fv::qt::post(nullptr, [&] { ran = true; });
    process_events();
    CHECK_FALSE(ran);
}

TEST_CASE("qt::post takes move-only functors") {
    QObject context;
    auto value = std::make_unique<int>(7);
    int seen = 0;
    fv::qt::post(&context, [owned = std::move(value), &seen] { seen = *owned; });
    process_events();
    CHECK(seen == 7);
}

TEST_CASE("a posted functor that throws is logged with its type and the loop continues") {
    ScopedSink capture;
    QObject context;
    bool after = false;
    fv::qt::post(&context, [] { throw std::runtime_error("boom"); });
    fv::qt::post(&context, [&] { after = true; });
    process_events();
    CHECK(after);
    const auto entries = capture.sink->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].level == fv::log::Level::Error);
    CHECK(entries[0].logger == "appkit.qt");
    CHECK(entries[0].exception_type == "std::runtime_error");
    CHECK(entries[0].message == "A task run by qt::post raised an exception");
}

TEST_CASE("TokenGate delivers only the newest token") {
    QObject context;
    fv::qt::TokenGate gate;
    CHECK(gate.current() == 0);
    const auto first = gate.next();
    const auto second = gate.next();
    CHECK(second == first + 1);
    CHECK(gate.isCurrent(second));
    CHECK_FALSE(gate.isCurrent(first));

    std::vector<std::string> delivered;
    std::thread worker([&] {
        gate.deliver(&context, first, [&] { delivered.push_back("first"); });
        gate.deliver(&context, second, [&] { delivered.push_back("second"); });
    });
    worker.join();
    process_events();
    CHECK(delivered == std::vector<std::string>{"second"});
}

TEST_CASE("TokenGate checks the token again at delivery") {
    QObject context;
    fv::qt::TokenGate gate;
    const auto token = gate.next();
    bool delivered = false;
    gate.deliver(&context, token, [&] { delivered = true; });
    gate.next(); // a newer request starts while the result is queued
    process_events();
    CHECK_FALSE(delivered);

    // Copies share the counter.
    fv::qt::TokenGate copy = gate;
    const auto newest = copy.next();
    CHECK(gate.isCurrent(newest));
}

TEST_CASE("TokenGate drops a result for a destroyed context") {
    auto context = std::make_unique<QObject>();
    fv::qt::TokenGate gate;
    const auto token = gate.next();
    bool delivered = false;
    gate.deliver(context.get(), token, [&] { delivered = true; });
    context.reset();
    process_events();
    CHECK_FALSE(delivered);
}

TEST_CASE("QtUiExecutor::post_after fires on the GUI thread with the requested timer type") {
    QObject context;
    fv::qt::QtUiExecutor executor(&context);
    CHECK(executor.is_current());

    std::atomic<bool> fired = false;
    QThread* fired_on = nullptr;
    Qt::TimerType type_seen = Qt::VeryCoarseTimer;
    fv::TimerHandle handle;
    bool current_on_worker = true;
    std::thread worker([&] {
        current_on_worker = executor.is_current();
        // From a thread without an event loop: the timer still starts on the GUI thread.
        handle = executor.postAfter(5ms, Qt::PreciseTimer, [&] {
            fired_on = QThread::currentThread();
            const auto timers = context.findChildren<QTimer*>();
            if (timers.size() == 1) {
                type_seen = timers.front()->timerType();
            }
            fired = true;
        });
    });
    worker.join();
    CHECK_FALSE(current_on_worker);
    CHECK(handle.active());
    REQUIRE(QTest::qWaitFor([&] { return fired.load(); }, 5000));
    CHECK(fired_on == context.thread());
    CHECK(type_seen == Qt::PreciseTimer);
    CHECK_FALSE(handle.active());
    process_events();
    CHECK(context.findChildren<QTimer*>().isEmpty());
}

TEST_CASE("QtUiExecutor::post_after picks QTimer.singleShot's timer type") {
    CHECK(fv::qt::QtUiExecutor::defaultTimerType(0ms) == Qt::PreciseTimer);
    CHECK(fv::qt::QtUiExecutor::defaultTimerType(1999ms) == Qt::PreciseTimer);
    CHECK(fv::qt::QtUiExecutor::defaultTimerType(2000ms) == Qt::CoarseTimer);

    QObject context;
    fv::qt::QtUiExecutor executor(&context);
    {
        auto precise = executor.post_after(1999ms, [] {});
        auto coarse = executor.postAfter(2000ms, Qt::CoarseTimer, [] {});
        auto very_coarse = executor.postAfter(60s, Qt::VeryCoarseTimer, [] {});
        const auto timers = context.findChildren<QTimer*>();
        REQUIRE(timers.size() == 3);
        CHECK(timers[0]->timerType() == Qt::PreciseTimer);
        CHECK(timers[1]->timerType() == Qt::CoarseTimer);
        CHECK(timers[2]->timerType() == Qt::VeryCoarseTimer);
        auto defaulted = executor.post_after(2000ms, [] {});
        CHECK(context.findChildren<QTimer*>().back()->timerType() == Qt::CoarseTimer);
    }
    // The handles cancelled their timers on the way out.
    CHECK(context.findChildren<QTimer*>().isEmpty());
}

TEST_CASE("QtUiExecutor timers cancel, detach and end with their context") {
    auto context = std::make_unique<QObject>();
    fv::qt::QtUiExecutor executor(context.get());

    bool cancelled_ran = false;
    auto cancelled = executor.post_after(1ms, [&] { cancelled_ran = true; });
    cancelled.cancel();
    CHECK_FALSE(cancelled.active());

    bool detached_ran = false;
    executor.post_after(1ms, [&] { detached_ran = true; }).detach();

    bool posted_ran = false;
    executor.post([&] { posted_ran = true; });

    REQUIRE(QTest::qWaitFor([&] { return detached_ran; }, 5000));
    CHECK(posted_ran);
    CHECK_FALSE(cancelled_ran);

    // Cancelled from another thread: the QTimer goes on the GUI thread.
    bool cross_ran = false;
    auto cross = executor.postAfter(50ms, Qt::PreciseTimer, [&] { cross_ran = true; });
    std::thread worker([&] { cross.cancel(); });
    worker.join();
    QTest::qWait(100);
    CHECK_FALSE(cross_ran);
    CHECK(context->findChildren<QTimer*>().isEmpty());

    bool orphan_ran = false;
    auto orphan = executor.post_after(1ms, [&] { orphan_ran = true; });
    context.reset();
    CHECK_FALSE(orphan.active());
    bool late_ran = false;
    executor.post([&] { late_ran = true; });
    CHECK_FALSE(executor.post_after(1ms, [&] { late_ran = true; }).active());
    QTest::qWait(20);
    CHECK_FALSE(orphan_ran);
    CHECK_FALSE(late_ran);
}

TEST_CASE("QtUiExecutor takes posts from workers while its context is destroyed") {
    auto context = std::make_unique<QObject>();
    fv::qt::QtUiExecutor executor(context.get());
    std::atomic<bool> stop = false;
    std::atomic<int> posted = 0;
    std::atomic<int> ran = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            while (!stop.load()) {
                executor.post([&] { ++ran; });
                ++posted;
            }
        });
    }
    REQUIRE(QTest::qWaitFor([&] { return posted.load() > 1000; }, 5000));
    process_events();
    context.reset(); // workers are still posting
    const int ran_before_end = ran.load();
    QTest::qWait(20);
    stop = true;
    for (auto& worker : workers) {
        worker.join();
    }
    process_events();
    CHECK(ran_before_end > 0);
    CHECK(ran.load() == ran_before_end); // nothing ran after the context went
}

TEST_CASE("QtUiExecutor timers outlive the executor and cancel safely after the context") {
    auto context = std::make_unique<QObject>();
    auto executor = std::make_unique<fv::qt::QtUiExecutor>(context.get());
    bool ran = false;
    auto pending = executor->postAfter(1h, Qt::CoarseTimer, [&] { ran = true; });
    executor.reset(); // the timer keeps the context's lifetime, not the executor's
    CHECK(pending.active());
    CHECK(context->findChildren<QTimer*>().size() == 1);
    context.reset();
    CHECK_FALSE(pending.active());

    auto queued_context = std::make_unique<QObject>();
    fv::qt::QtUiExecutor queued_executor(queued_context.get());
    fv::TimerHandle from_worker;
    std::thread arm([&] { from_worker = queued_executor.postAfter(1h, Qt::CoarseTimer, [] {}); });
    arm.join();
    queued_context.reset(); // before the queued start ran
    std::thread cancel([&] { from_worker.cancel(); });
    cancel.join();
    process_events();
    CHECK_FALSE(ran);
}

TEST_CASE("the first QtUiExecutor marks its thread as the GUI thread") {
    QObject context;
    fv::qt::QtUiExecutor executor(&context);
    CHECK(fv::is_gui_thread());

    bool worker_is_gui = true;
    std::thread worker([&] {
        QObject other;
        fv::qt::QtUiExecutor second(&other); // not the first: the GUI thread stays
        worker_is_gui = fv::is_gui_thread();
    });
    worker.join();
    CHECK_FALSE(worker_is_gui);
    CHECK(fv::is_gui_thread());
}

TEST_CASE("assert_not_gui_thread fires in debug on the thread of the first QtUiExecutor") {
    const auto run = [](const char* mode) {
        QProcess helper;
        helper.start(QString::fromUtf8(FV_GUI_THREAD_HELPER), QStringList{QString::fromUtf8(mode)});
        REQUIRE(helper.waitForFinished(30000));
        return std::pair{helper.exitCode(), QString::fromUtf8(helper.readAllStandardOutput())};
    };

    const auto [worker_code, worker_out] = run("worker");
    CHECK(worker_code == 0);
    CHECK(worker_out.contains(QStringLiteral("RETURNED")));

    const auto [gui_code, gui_out] = run("gui");
#ifdef NDEBUG
    CHECK(gui_code == 0);
    CHECK(gui_out.contains(QStringLiteral("RETURNED")));
#else
    CHECK(gui_code != 0);
    CHECK_FALSE(gui_out.contains(QStringLiteral("RETURNED")));
#endif
}

TEST_CASE("toQString and toUtf8 convert UTF-8 both ways") {
    const std::string text = "Caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x8e\x99";
    const QString converted = fv::qt::toQString(text);
    CHECK(converted == QStringLiteral(u"Café — 🎙"));
    CHECK(fv::qt::toUtf8(converted) == text);
    CHECK(fv::qt::toQString({}).isEmpty());
    CHECK(fv::qt::toUtf8(QString()).empty());
    const std::string with_nul("a\0b", 3);
    CHECK(fv::qt::toUtf8(fv::qt::toQString(with_nul)) == with_nul);
}
