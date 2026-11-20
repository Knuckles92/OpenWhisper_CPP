// The Scratchpad's Transform menu: the saved transforms listed, and a choice rewriting the
// selection or everything off the GUI thread (tests/test_personalize_s8_scratchpad.py's
// TestTransforms and tests/test_personalize_fix_f2_scratchpad_transform.py). The rewrite is
// replaced by a script of the test, or runs llm::rewrite_standalone over a scripted HTTP client
// and in-memory keys; nothing reaches a network or the owner's credentials.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "llm/credentials.h"
#include "llm/rewrite.h"
#include "pyport/json.h"
#include "scratchpad_support.h"
#include "ui_record/scratchpad_transforms.h"
#include "ui_widgets/eliding_label.h"

namespace {

using fv::Json;
using fv::ScratchpadWindow;

Json polishRow() {
    return Json{
        {"id", "polish"}, {"name", "Polish"}, {"instruction", "Improve the flow."}, {"hotkey", ""}};
}

fv::text::Transform polish() {
    return {"polish", "Polish", "Improve the flow.", ""};
}

using Script = std::function<std::pair<std::string, std::optional<std::string>>(
    std::string_view text, std::string_view instruction, const Json& settings)>;

// The messages the log facade gets while the test lives.
class LogCapture final : public fv::log::Sink {
public:
    LogCapture() : self_(std::shared_ptr<Sink>(std::shared_ptr<Sink>(), this)) {
        fv::log::add_sink(self_);
    }
    ~LogCapture() override { fv::log::remove_sink(self_); }
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        text_ += std::string(record.logger) + ": " + std::string(record.message) + "\n";
    }
    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
    std::shared_ptr<Sink> self_;
};

// The Scratchpad service with the Transform menu over real worker threads (the jobs strand) and
// the GUI thread's executor, and a rewrite the test scripts.
class TransformRig {
public:
    explicit TransformRig(Json saved = Json{{"text_transforms", Json::array({polishRow()})}})
        : pads(std::move(saved)), ui(qApp), pool("test-pool", 2),
          jobs(pool, "test-jobs", 2, fv::ShutdownMode::CancelQueued) {
        fv::TransformEnvironment environment;
        environment.settings = &pads.settings;
        environment.jobs = &jobs;
        environment.ui = &ui;
        environment.rewrite = [this](std::string_view text, std::string_view instruction,
                                     const Json& settings) {
            return script(text, instruction, settings);
        };
        transforms = std::make_shared<fv::ScratchpadTransforms>(std::move(environment));
        pads.service->setTransformMenuFiller(transforms->filler());
    }

    // The window, shown and focused as far as the rig says.
    ScratchpadWindow& open() { return pads.open(); }

    // Runs the loop until the editor is writable again: the transform ended.
    static void waitForTransform(ScratchpadWindow& pad, int timeoutMs = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (pad.editor()->isReadOnly() && timer.elapsed() < timeoutMs) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        REQUIRE_FALSE(pad.editor()->isReadOnly());
    }

    fvtest::PadRig pads;
    // Declared before the strand, so a rewrite still running when the test ends finds it.
    Script script = [](std::string_view text, std::string_view, const Json&) {
        return std::make_pair(std::string(text), std::optional<std::string>());
    };
    fv::qt::QtUiExecutor ui;
    fv::ThreadPool pool;
    fv::Strand jobs;
    std::shared_ptr<fv::ScratchpadTransforms> transforms;
};

std::vector<QString> texts(const QMenu* menu) {
    std::vector<QString> found;
    for (const QAction* action : menu->actions()) {
        found.push_back(action->text());
    }
    return found;
}

void select(ScratchpadWindow& pad, int from, int to) {
    QTextCursor cursor = pad.editor()->textCursor();
    cursor.setPosition(from);
    cursor.setPosition(to, QTextCursor::KeepAnchor);
    pad.editor()->setTextCursor(cursor);
}

QString status(const ScratchpadWindow& pad) {
    return pad.statusLabel()->text();
}

} // namespace

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::test_menu_lists_saved_transforms
TEST_CASE("the menu lists the saved transforms", "[test_personalize_s8_scratchpad][widget]") {
    TransformRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.fillTransformMenu();
    CHECK(texts(pad.transformMenu()) == std::vector<QString>{QStringLiteral("Polish")});
}

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::test_menu_says_when_there_are_none
TEST_CASE("the menu says when there are none", "[test_personalize_s8_scratchpad][widget]") {
    TransformRig rig(Json{{"text_transforms", Json::array()}});
    ScratchpadWindow& pad = rig.open();
    pad.fillTransformMenu();
    const auto actions = pad.transformMenu()->actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0]->text() == QStringLiteral("No transforms yet"));
    CHECK_FALSE(actions[0]->isEnabled());
}

TEST_CASE("before any transform is saved the menu lists the starters, and fills again each time",
          "[widget]") {
    TransformRig rig(Json::object());
    ScratchpadWindow& pad = rig.open();
    pad.fillTransformMenu();
    CHECK(texts(pad.transformMenu()) ==
          std::vector<QString>{QStringLiteral("Polish"), QStringLiteral("Make concise"),
                               QStringLiteral("Fix grammar"), QStringLiteral("Prompt engineer")});

    // The menu is read when it opens, so a transform saved meanwhile is in the next one.
    rig.pads.settings.save_setting("text_transforms", Json::array({polishRow()}));
    pad.fillTransformMenu();
    CHECK(texts(pad.transformMenu()) == std::vector<QString>{QStringLiteral("Polish")});
}

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::
//      test_transform_rewrites_everything_off_the_qt_thread
TEST_CASE("a transform rewrites everything off the Qt thread",
          "[test_personalize_s8_scratchpad][widget]") {
    LogCapture log;
    TransformRig rig;
    std::vector<bool> onGuiThread;
    std::vector<std::string> instructions;
    std::mutex mutex;
    rig.script = [&](std::string_view text, std::string_view instruction, const Json&) {
        {
            std::lock_guard lock(mutex);
            onGuiThread.push_back(QThread::currentThread() == qApp->thread());
            instructions.emplace_back(instruction);
        }
        std::string upper(text);
        for (char& c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return std::make_pair(upper, std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("private draft words"));

    rig.transforms->apply(pad, polish());
    CHECK(pad.editor()->isReadOnly());
    CHECK_FALSE(pad.transformButton()->isEnabled());
    CHECK(status(pad) == QStringLiteral(u"Applying Polish…"));
    TransformRig::waitForTransform(pad);

    CHECK(onGuiThread == std::vector<bool>{false});
    CHECK(instructions == std::vector<std::string>{"Improve the flow."});
    CHECK(pad.text() == QStringLiteral("PRIVATE DRAFT WORDS"));
    CHECK(status(pad).startsWith(QStringLiteral("Polish applied")));
    CHECK(pad.transformButton()->isEnabled());
    // Lengths are logged, never the text.
    CHECK(log.text().find("private draft") == std::string::npos);
    CHECK(log.text().find("PRIVATE") == std::string::npos);
    CHECK(log.text().find("Scratchpad transform started (19 characters)") != std::string::npos);
    pad.editor()->undo();
    CHECK(pad.text() == QStringLiteral("private draft words"));
}

TEST_CASE("the success notice names the undo shortcut", "[widget]") {
    TransformRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("rough"));
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    const QString undo = QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText);
    REQUIRE_FALSE(undo.isEmpty());
    CHECK(status(pad) == QStringLiteral(u"Polish applied · %1 to undo").arg(undo));
    // The notice gives way to the word count after its time.
    CHECK(pad.noticeTimer()->isActive());
}

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::
//      test_transform_rewrites_only_the_selection
TEST_CASE("a transform rewrites only the selection", "[test_personalize_s8_scratchpad][widget]") {
    TransformRig rig;
    rig.script = [](std::string_view text, std::string_view, const Json&) {
        return std::make_pair("[" + std::string(text) + "]", std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("keep this but change that"));
    select(pad, 14, 25);
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    CHECK(pad.text() == QStringLiteral("keep this but [change that]"));
}

TEST_CASE("a delayed Scratchpad transform preserves action replies beyond its selection",
          "[widget]") {
    TransformRig rig;
    // Worker completion is posted to the Qt event queue. The reply is inserted before pumping it.
    std::string input;
    rig.script = [&](std::string_view text, std::string_view, const Json&) {
        input.assign(text);
        return std::make_pair(std::string("DONE"), std::optional<std::string>());
    };
    auto& pad = rig.open();
    SECTION("whole document") {
        pad.editor()->setPlainText(QStringLiteral("my notes"));
        rig.transforms->apply(pad, polish());
        fv::showScratchpadText(*rig.pads.service, QStringLiteral("the answer"));
        TransformRig::waitForTransform(pad);
        CHECK(input == "my notes");
        CHECK(pad.text() == QStringLiteral("DONE\n\nthe answer"));
        CHECK(pad.editor()->textCursor().position() == 4);
    }
    SECTION("backwards selection ending at the last character") {
        pad.editor()->setPlainText(QStringLiteral("keep this. shout this"));
        select(pad, 21, 11);
        rig.transforms->apply(pad, polish());
        fv::showScratchpadText(*rig.pads.service, QStringLiteral("the answer"));
        TransformRig::waitForTransform(pad);
        CHECK(input == "shout this");
        CHECK(pad.text() == QStringLiteral("keep this. DONE\n\nthe answer"));
    }
}

TEST_CASE("paragraph breaks reach the rewrite as new lines and the result replaces exactly the "
          "text read",
          "[widget]") {
    TransformRig rig;
    std::string seen;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    rig.script = [&](std::string_view text, std::string_view, const Json&) {
        seen = std::string(text);
        released.wait();
        return std::make_pair(std::string("NEW"), std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("one\ntwo\nthree"));
    select(pad, 4, 7); // "two"
    rig.transforms->apply(pad, polish());
    // Text added at either edge while the model works stays outside the replaced range.
    QTextCursor edge(pad.editor()->document());
    edge.setPosition(4);
    edge.insertText(QStringLiteral("<"));
    edge.setPosition(8);
    edge.insertText(QStringLiteral(">"));
    release.set_value();
    TransformRig::waitForTransform(pad);
    CHECK(seen == "two");
    CHECK(pad.text() == QStringLiteral("one\n<NEW>\nthree"));

    // A selection over a paragraph break is sent with a new line, not U+2029.
    rig.script = [&](std::string_view text, std::string_view, const Json&) {
        seen = std::string(text);
        return std::make_pair(std::string("done"), std::optional<std::string>());
    };
    pad.editor()->setPlainText(QStringLiteral("alpha\nbeta"));
    select(pad, 0, 10);
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    CHECK(seen == "alpha\nbeta");
    CHECK(pad.text() == QStringLiteral("done"));
}

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::
//      test_a_failed_transform_leaves_the_text
TEST_CASE("a failed transform leaves the text", "[test_personalize_s8_scratchpad][widget]") {
    TransformRig rig;
    rig.script = [](std::string_view, std::string_view, const Json&) {
        return std::make_pair(std::string(),
                              std::optional<std::string>("Set up AI cleanup to rewrite text"));
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("as it was"));
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    CHECK(pad.text() == QStringLiteral("as it was"));
    CHECK(status(pad) == QStringLiteral("Set up AI cleanup to rewrite text"));
}

TEST_CASE("a rewrite that throws says so without quoting the draft", "[widget]") {
    LogCapture log;
    TransformRig rig;
    rig.script = [](std::string_view text, std::string_view,
                    const Json&) -> std::pair<std::string, std::optional<std::string>> {
        throw std::runtime_error("model said: " + std::string(text));
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("secret words"));
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    CHECK(pad.text() == QStringLiteral("secret words"));
    CHECK(status(pad) == QStringLiteral("Couldn't transform the text"));
    CHECK(log.text().find("secret") == std::string::npos);
    CHECK(log.text().find("Scratchpad transform failed") != std::string::npos);
}

TEST_CASE("an empty answer keeps the text", "[widget]") {
    TransformRig rig;
    rig.script = [](std::string_view, std::string_view, const Json&) {
        return std::make_pair(std::string("  \n"), std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("as it was"));
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    CHECK(pad.text() == QStringLiteral("as it was"));
    CHECK(status(pad) == QStringLiteral("The transform returned nothing"));
}

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::test_nothing_to_transform
TEST_CASE("there is nothing to transform", "[test_personalize_s8_scratchpad][widget]") {
    TransformRig rig;
    int calls = 0;
    rig.script = [&calls](std::string_view, std::string_view, const Json&) {
        ++calls;
        return std::make_pair(std::string("x"), std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral(" \n "));
    rig.transforms->apply(pad, polish());
    CHECK(status(pad) == QStringLiteral("Write or dictate something to transform"));
    CHECK_FALSE(pad.editor()->isReadOnly());
    CHECK(pad.transformButton()->isEnabled());
    QApplication::processEvents();
    CHECK(calls == 0);
}

TEST_CASE("choosing a menu entry applies that transform", "[widget]") {
    TransformRig rig;
    std::string instruction;
    rig.script = [&instruction](std::string_view text, std::string_view given, const Json&) {
        instruction = std::string(given);
        return std::make_pair("<" + std::string(text) + ">", std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("words"));
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    pad.transformMenu()->actions()[0]->trigger();
    TransformRig::waitForTransform(pad);
    CHECK(instruction == "Improve the flow.");
    CHECK(pad.text() == QStringLiteral("<words>"));
}

TEST_CASE("an older answer never lands over a newer one", "[widget]") {
    TransformRig rig;
    std::promise<void> releaseFirst;
    std::shared_future<void> first = releaseFirst.get_future().share();
    std::atomic<int> calls{0};
    rig.script = [&](std::string_view, std::string_view, const Json&) {
        if (calls.fetch_add(1) == 0) {
            first.wait();
            return std::make_pair(std::string("FIRST"), std::optional<std::string>());
        }
        return std::make_pair(std::string("SECOND"), std::optional<std::string>());
    };
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("words"));
    rig.transforms->apply(pad, polish());
    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);
    CHECK(pad.text() == QStringLiteral("SECOND"));

    releaseFirst.set_value();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 300) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    CHECK(pad.text() == QStringLiteral("SECOND"));
}

// from tests/test_personalize_s8_scratchpad.py::TestTransforms::
//      test_a_window_deleted_mid_transform_is_not_held_by_the_worker
TEST_CASE("a window deleted mid-transform is not held by the worker",
          "[test_personalize_s8_scratchpad][widget]") {
    TransformRig rig;
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    rig.script = [&](std::string_view text, std::string_view, const Json&) {
        started = true;
        opened.wait_for(std::chrono::seconds(5));
        finished = true;
        return std::make_pair(std::string(text) + "!", std::optional<std::string>());
    };
    auto window = std::make_unique<ScratchpadWindow>(rig.pads.environment(), rig.pads.copier());
    window->editor()->setPlainText(QStringLiteral("private draft words"));
    QPointer<ScratchpadWindow> watcher(window.get());
    rig.transforms->apply(*window, polish());
    QElapsedTimer timer;
    timer.start();
    while (!started && timer.elapsed() < 5000) {
        QThread::msleep(5);
    }
    REQUIRE(started);

    // The worker is inside the rewrite; the window goes, at once.
    window.reset();
    CHECK(watcher.isNull());
    QApplication::processEvents();

    gate.set_value();
    timer.restart();
    while (!finished && timer.elapsed() < 5000) {
        QThread::msleep(5);
    }
    CHECK(finished);
    // The answer finds no window; the delivery on this thread must neither crash nor resurrect it.
    for (int i = 0; i < 10; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    CHECK(watcher.isNull());
}

// ---- through the real rewrite: only the HTTP client and the keys are faked ----

namespace {

fv::http::ScriptedResponse jsonReply(int status, const Json& body) {
    fv::http::ScriptedResponse reply;
    reply.status = status;
    reply.reason = status == 200 ? "OK" : "Internal Server Error";
    reply.headers = {{"Content-Type", "application/json"}};
    reply.body = fv::py::dumps(body);
    return reply;
}

// llm::rewrite_standalone over a scripted HTTP client and in-memory keys, as OpenWhisper's
// scratchpad transform tests fake only the network client and the provider check.
struct RealRewrite {
    RealRewrite() {
        options.retry_hooks.sleep = [](std::chrono::milliseconds, const fv::CancelToken&) {
        };
        options.retry_hooks.random = [] {
            return 0.5;
        };
    }
    Script script() {
        return [this](std::string_view text, std::string_view instruction, const Json& settings) {
            fv::llm::KeySources sources{store, env, dotenv};
            return fv::llm::rewrite_standalone(text, instruction, settings, http, sources, {},
                                               options);
        };
    }
    void answer(const std::string& words) {
        http->set_handler([words](const fv::http::Request&) {
            return jsonReply(
                200,
                Json{{"choices",
                      Json::array({Json{{"message", {{"role", "assistant"}, {"content", words}}},
                                        {"finish_reason", "stop"}}})}});
        });
    }

    std::shared_ptr<fv::http::FakeHttpClient> http = std::make_shared<fv::http::FakeHttpClient>();
    std::shared_ptr<fv::MemoryCredentialBackend> backend =
        std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store{backend, fv::identity()};
    fv::MapEnvReader env;
    fv::DotEnv dotenv{std::filesystem::path("fv-scratchpad-tests-no-such-dir") / ".env"};
    fv::llm::CleanupOptions options;
};

Json providerSettings() {
    return Json{{"text_transforms", Json::array({polishRow()})},
                {"transcript_cleanup_provider", "openai"},
                {"transcript_cleanup_model", "gpt-test"}};
}

} // namespace

// from tests/test_personalize_fix_f2_scratchpad_transform.py::
//      test_a_successful_rewrite_replaces_the_text
TEST_CASE("a successful rewrite replaces the text",
          "[test_personalize_fix_f2_scratchpad_transform]") {
    RealRewrite real;
    real.env.set("OPENAI_API_KEY", "sk-test");
    real.answer("Polished words here.");
    TransformRig rig(providerSettings());
    rig.script = real.script();
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("rough words here"));

    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);

    CHECK(pad.text() == QStringLiteral("Polished words here."));
    CHECK(status(pad).startsWith(QStringLiteral("Polish applied")));
    CHECK(real.http->requests().size() == 1);
}

// from tests/test_personalize_fix_f2_scratchpad_transform.py::
//      test_a_provider_error_is_shown_and_the_text_kept
TEST_CASE("a provider error is shown and the text kept",
          "[test_personalize_fix_f2_scratchpad_transform]") {
    RealRewrite real;
    real.env.set("OPENAI_API_KEY", "sk-test");
    real.http->set_handler([](const fv::http::Request&) {
        return jsonReply(500, Json{{"error", {{"message", "server error"}}}});
    });
    TransformRig rig(providerSettings());
    rig.script = real.script();
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("rough words here"));

    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);

    CHECK(pad.text() == QStringLiteral("rough words here"));
    CHECK(status(pad).startsWith(QStringLiteral("The AI model failed: ")));
}

// from tests/test_personalize_fix_f2_scratchpad_transform.py::
//      test_without_a_provider_it_says_how_to_set_one_up
TEST_CASE("without a provider it says how to set one up",
          "[test_personalize_fix_f2_scratchpad_transform]") {
    RealRewrite real;
    real.answer("never asked");
    TransformRig rig(providerSettings()); // no key anywhere
    rig.script = real.script();
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("rough words here"));

    rig.transforms->apply(pad, polish());
    TransformRig::waitForTransform(pad);

    CHECK(pad.text() == QStringLiteral("rough words here"));
    CHECK(status(pad) == QString::fromUtf8(fv::llm::kNoProviderMessage.data(),
                                           fv::llm::kNoProviderMessage.size()));
    CHECK(real.http->requests().empty());
}
