// ActionRelay (ui_qt/widgets/action_relay.py): blocking work runs off the GUI thread, answers
// come back on it with their tokens, a stale secret answer is dropped, a failed scan still says
// it is done, and nothing reaches a relay that is gone.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include "agents/testing/fake_installed_agents.h"
#include "automation/action_result.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "pyport/errors.h"
#include "ui_automation/action_relay.h"

namespace {

using fv::ActionRelay;
using fv::ActionRelayWork;

// What the work functions saw.
struct Calls {
    std::mutex mutex;
    std::vector<std::string> log;
    std::vector<bool> offGuiThread;

    void add(std::string entry) {
        std::lock_guard lock(mutex);
        log.push_back(std::move(entry));
        offGuiThread.push_back(!fv::is_gui_thread() && QThread::currentThread() != qApp->thread());
    }
};

ActionRelayWork recordingWork(Calls& calls) {
    ActionRelayWork work;
    work.scanAgents = [&calls](bool refresh) {
        calls.add(refresh ? "scan refresh" : "scan");
    };
    work.ensureSecret = [&calls](const std::string& id) {
        calls.add("ensure " + id);
        return "whsec_" + id;
    };
    work.deleteSecret = [&calls](const std::string& id) {
        calls.add("delete " + id);
    };
    return work;
}

} // namespace

TEST_CASE("relay tokens count up from one", "[widget]") {
    fv::ManualExecutor io;
    Calls calls;
    ActionRelay relay(io, recordingWork(calls));
    CHECK(relay.nextToken() == 1);
    CHECK(relay.nextToken() == 2);
    CHECK(relay.nextToken() == 3);
}

TEST_CASE("a scan's answer carries every requester's token", "[widget]") {
    fv::ManualExecutor io;
    Calls calls;
    ActionRelay relay(io, recordingWork(calls));
    QSignalSpy done(&relay, &ActionRelay::scanDone);
    const int card = relay.nextToken();
    const int editor = relay.nextToken();
    relay.startAgentScan(card);
    relay.startAgentScan(editor, true);
    // Nothing runs on the GUI thread's own call.
    CHECK(calls.log.empty());
    io.run_all();
    CHECK(calls.log == std::vector<std::string>{"scan", "scan refresh"});
    // Answers are queued, never emitted from the worker's call.
    CHECK(done.count() == 0);
    QApplication::processEvents();
    REQUIRE(done.count() == 2);
    CHECK(done.at(0).at(0).toInt() == card);
    CHECK(done.at(1).at(0).toInt() == editor);
}

TEST_CASE("a scan that raised still says it is done", "[widget]") {
    fv::ManualExecutor io;
    ActionRelayWork work;
    work.scanAgents = [](bool) {
        throw std::runtime_error("PATH unreadable");
    };
    ActionRelay relay(io, std::move(work));
    QSignalSpy done(&relay, &ActionRelay::scanDone);
    relay.startAgentScan(7);
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 1);
    CHECK(done.at(0).at(0).toInt() == 7);
}

TEST_CASE("a secret is made off the GUI thread, renewed by deleting it first", "[widget]") {
    fv::ManualExecutor io;
    Calls calls;
    ActionRelay relay(io, recordingWork(calls));
    QSignalSpy done(&relay, &ActionRelay::secretDone);
    relay.fetchSecret(4, QStringLiteral("abc"), true);
    io.run_all();
    QApplication::processEvents();
    CHECK(calls.log == std::vector<std::string>{"delete abc", "ensure abc"});
    REQUIRE(done.count() == 1);
    CHECK(done.at(0).at(0).toInt() == 4);
    CHECK(done.at(0).at(1).toString() == QStringLiteral("whsec_abc"));
    CHECK(done.at(0).at(2).toString().isEmpty());
}

TEST_CASE("a secret the store can't give is explained", "[widget]") {
    fv::ManualExecutor io;
    ActionRelayWork work;
    work.ensureSecret = [](const std::string&) -> std::string {
        throw fv::CredentialStoreError("The credential store is locked.");
    };
    ActionRelay relay(io, std::move(work));
    QSignalSpy done(&relay, &ActionRelay::secretDone);
    relay.fetchSecret(1, QStringLiteral("abc"));
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 1);
    CHECK(done.at(0).at(1).toString().isEmpty());
    CHECK(done.at(0).at(2).toString() == QStringLiteral("The credential store is locked."));

    // No message: the exception's type.
    ActionRelayWork silent;
    silent.ensureSecret = [](const std::string&) -> std::string {
        throw fv::CredentialStoreError("");
    };
    ActionRelay other(io, std::move(silent));
    QSignalSpy otherDone(&other, &ActionRelay::secretDone);
    other.fetchSecret(1, QStringLiteral("abc"));
    io.run_all();
    QApplication::processEvents();
    REQUIRE(otherDone.count() == 1);
    CHECK(otherDone.at(0).at(2).toString() == QStringLiteral("CredentialStoreError"));

    // A pyport error stands in for a Python one: its Python class is the type.
    ActionRelayWork python;
    python.ensureSecret = [](const std::string&) -> std::string {
        throw fv::py::ValueError("");
    };
    ActionRelay third(io, std::move(python));
    QSignalSpy thirdDone(&third, &ActionRelay::secretDone);
    third.fetchSecret(1, QStringLiteral("abc"));
    io.run_all();
    QApplication::processEvents();
    REQUIRE(thirdDone.count() == 1);
    CHECK(thirdDone.at(0).at(2).toString() == QStringLiteral("ValueError"));
}

TEST_CASE("a stale secret answer is dropped", "[widget]") {
    fv::ManualExecutor io;
    Calls calls;
    ActionRelay relay(io, recordingWork(calls));
    QSignalSpy done(&relay, &ActionRelay::secretDone);
    const int copy = relay.nextToken();
    relay.fetchSecret(copy, QStringLiteral("abc"));
    const int renew = relay.nextToken();
    relay.fetchSecret(renew, QStringLiteral("abc"), true);
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 1);
    CHECK(done.at(0).at(0).toInt() == renew);

    // Also when the newer request comes after the older answer was queued.
    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"));
    io.run_all();
    relay.fetchSecret(relay.nextToken(), QStringLiteral("def"));
    QApplication::processEvents();
    CHECK(done.count() == 1);
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 2);
    CHECK(done.at(1).at(1).toString() == QStringLiteral("whsec_def"));
}

TEST_CASE("forgetting a secret waits for nothing and survives a failure", "[widget]") {
    fv::ManualExecutor io;
    std::vector<std::string> forgotten;
    ActionRelayWork work;
    work.deleteSecret = [&](const std::string& id) {
        forgotten.push_back(id);
        throw fv::CredentialStoreError("refused");
    };
    ActionRelay relay(io, std::move(work));
    relay.forgetSecret(QStringLiteral("abc"));
    CHECK(forgotten.empty());
    io.run_all();
    CHECK(forgotten == std::vector<std::string>{"abc"});
}

TEST_CASE("a try's answer comes back on the GUI thread with its token", "[widget]") {
    fv::ManualExecutor io;
    Calls calls;
    ActionRelay relay(io, recordingWork(calls));
    std::vector<std::pair<int, fv::automation::ActionResult>> answers;
    std::vector<bool> onGui;
    QObject::connect(&relay, &ActionRelay::tryDone,
                     [&](int token, const fv::automation::ActionResult& result) {
                         answers.emplace_back(token, result);
                         onGui.push_back(QThread::currentThread() == qApp->thread());
                     });
    auto done = relay.tryCallback(9);
    std::thread worker([done] {
        done(fv::automation::ActionResult{.ok = true, .message = "Sent", .output = "Hi"});
    });
    worker.join();
    CHECK(answers.empty());
    QApplication::processEvents();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].first == 9);
    CHECK(answers[0].second.message == "Sent");
    CHECK(answers[0].second.output == "Hi");
    CHECK(onGui == std::vector<bool>{true});
}

TEST_CASE("the relay never touches the GUI thread's objects off it", "[widget]") {
    fv::ThreadPool pool("relay-test", 2);
    fv::Strand io(pool, "io", 1, fv::ShutdownMode::Drain);
    Calls calls;
    ActionRelay relay(io, recordingWork(calls));
    std::vector<bool> answeredOnGui;
    QObject::connect(&relay, &ActionRelay::scanDone, [&](int) {
        answeredOnGui.push_back(QThread::currentThread() == qApp->thread());
    });
    QObject::connect(&relay, &ActionRelay::secretDone, [&](int, const QString&, const QString&) {
        answeredOnGui.push_back(QThread::currentThread() == qApp->thread());
    });
    relay.startAgentScan(relay.nextToken());
    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"));
    REQUIRE(QTest::qWaitFor([&] { return answeredOnGui.size() == 2; }, 5000));
    CHECK(answeredOnGui == std::vector<bool>{true, true});
    std::lock_guard lock(calls.mutex);
    CHECK(calls.log.size() == 2);
    CHECK(calls.offGuiThread == std::vector<bool>{true, true});
}

TEST_CASE("a relay that is gone gets no answer and no crash", "[widget]") {
    fv::ThreadPool pool("relay-test", 2);
    fv::Strand io(pool, "io", 1, fv::ShutdownMode::Drain);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> finished = false;
    ActionRelayWork work;
    work.scanAgents = [released, &finished](bool) {
        released.wait();
        finished = true;
    };
    int answers = 0;
    {
        auto relay = std::make_unique<ActionRelay>(io, std::move(work));
        QObject::connect(relay.get(), &ActionRelay::scanDone, [&](int) { ++answers; });
        relay->startAgentScan(1);
        auto late = relay->tryCallback(2);
        relay.reset();
        late(fv::automation::ActionResult{});
    }
    release.set_value();
    REQUIRE(QTest::qWaitFor([&] { return finished.load(); }, 5000));
    io.shutdown(std::chrono::milliseconds(2000));
    QApplication::processEvents();
    CHECK(answers == 0);
}

TEST_CASE("the app's relay work scans the installed agents and keeps action secrets", "[widget]") {
    auto agents = std::make_shared<fv::agents::FakeInstalledAgents>();
    agents->set_agent("codex", fv::agents::InstalledAgent{.id = "codex", .version = "1.0"});
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store(backend, fv::identity());
    fv::ThreadPool pool("relay-test", 2);
    fv::Strand io(pool, "io", 1, fv::ShutdownMode::Drain);
    ActionRelay relay(io, fv::actionRelayWork(agents, store));
    QSignalSpy scanned(&relay, &ActionRelay::scanDone);
    QSignalSpy secrets(&relay, &ActionRelay::secretDone);

    relay.startAgentScan(relay.nextToken());
    REQUIRE(QTest::qWaitFor([&] { return scanned.count() == 1; }, 5000));
    REQUIRE(agents->cached_scan().has_value());
    CHECK(agents->cached_scan()->at("codex").has_value());
    CHECK_FALSE(agents->cached_scan()->at("claude_code").has_value());

    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"));
    REQUIRE(QTest::qWaitFor([&] { return secrets.count() == 1; }, 5000));
    const QString first = secrets.at(0).at(1).toString();
    CHECK_FALSE(first.isEmpty());
    CHECK(secrets.at(0).at(2).toString().isEmpty());
    // The same secret on a second ask; a new one once renewed.
    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"));
    REQUIRE(QTest::qWaitFor([&] { return secrets.count() == 2; }, 5000));
    CHECK(secrets.at(1).at(1).toString() == first);
    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"), true);
    REQUIRE(QTest::qWaitFor([&] { return secrets.count() == 3; }, 5000));
    const QString renewed = secrets.at(2).at(1).toString();
    CHECK(renewed != first);
    // Forgotten: the next ask makes another (the strand runs them in order).
    relay.forgetSecret(QStringLiteral("abc"));
    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"));
    REQUIRE(QTest::qWaitFor([&] { return secrets.count() == 4; }, 5000));
    CHECK(secrets.at(3).at(1).toString() != renewed);
    io.shutdown(std::chrono::milliseconds(5000));
}
