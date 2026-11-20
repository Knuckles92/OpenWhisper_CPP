// What the record feature wires for the AI text features: Quick Record's Command Mode hint asks
// the credential store through the io strand, and the Scratchpad's Transform menu rewrites through
// the jobs strand over the shared HTTP client. Everything is a fake or loopback-free: the keys are
// in memory, the endpoint is a scripted client, the provider is a local server that needs no key
// (so the process environment is never consulted for one).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>

#include "app_theme.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "pyport/json.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_record/scratchpad.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;

Json localProvider(const std::string& model) {
    return Json{{"transcript_cleanup_provider", "ollama"},
                {"transcript_cleanup_model", model},
                {"hotkeys", {{"command_mode", "ctrl+alt+k"}}},
                {"text_transforms", Json::array({Json{{"id", "polish"},
                                                      {"name", "Polish"},
                                                      {"instruction", "Improve the flow."},
                                                      {"hotkey", ""}}})}};
}

// What Bootstrap builds for the feature, with the executors the text features run on.
class TextFixture {
public:
    explicit TextFixture(const Json& saved)
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), settings_(saved), ui_(qApp),
          credentials_(std::make_shared<fv::MemoryCredentialBackend>(), fv::identity()),
          http_(std::make_shared<fv::http::FakeHttpClient>()), pool_("test-pool", 3),
          jobs_(pool_, "test-jobs", 2, fv::ShutdownMode::CancelQueued),
          io_(pool_, "test-io", 2, fv::ShutdownMode::CancelQueued), context_(services()) {
        http_->set_handler([](const fv::http::Request&) {
            fv::http::ScriptedResponse reply;
            reply.headers = {{"Content-Type", "application/json"}};
            reply.body = fv::py::dumps(Json{
                {"choices",
                 Json::array({Json{{"message", {{"role", "assistant"}, {"content", "Polished."}}},
                                   {"finish_reason", "stop"}}})}});
            return reply;
        });
        feature_.provide(context_);
        feature_.install(context_);
    }

    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    fv::http::FakeHttpClient& http() { return *http_; }

    // Runs the GUI loop until `done`, at most five seconds.
    template <class Done> bool waitFor(Done done) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        return done();
    }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.persistence = &persistence_;
        made.jobs = &jobs_;
        made.io = &io_;
        made.credentials = &credentials_;
        made.http = http_.get();
        return made;
    }

    fvtest::AppTheme theme_;
    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::ManualExecutor persistence_;
    fv::qt::QtUiExecutor ui_;
    // What the strands' tasks use is declared before the strands, so it outlives them: a
    // readiness check still running on io when the test ends reads a live credential store.
    fv::CredentialStore credentials_;
    std::shared_ptr<fv::http::FakeHttpClient> http_;
    fv::ThreadPool pool_;
    fv::Strand jobs_;
    fv::Strand io_;
    fv::AppContext context_;
    fv::RecordFeature feature_;
};

} // namespace

TEST_CASE("Quick Record's Command Mode hint asks the credential store on the io strand",
          "[widget]") {
    // A model is not chosen: the provider is not set up.
    TextFixture fixture(localProvider(""));
    fv::QuickRecordTab* tab = fixture.context().service<fv::RecordWidgets>().quickRecord();
    REQUIRE(tab != nullptr);
    // Until the first answer lands the provider is assumed to work.
    CHECK_FALSE(tab->commandHint()->isHidden());

    REQUIRE(fixture.waitFor([&] { return tab->commandHint()->isHidden(); }));
    CHECK_FALSE(tab->commandLink()->isHidden());
    CHECK(tab->commandLink()->text() == QStringLiteral(u"Set up Command Mode  →"));

    // A model is chosen in Settings; coming back to the window reads it again.
    fixture.settings().save_setting("transcript_cleanup_model", "llama3.2");
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(tab, &activate);
    REQUIRE(fixture.waitFor([&] { return !tab->commandHint()->isHidden(); }));
    CHECK(tab->commandLink()->isHidden());
}

TEST_CASE("the Scratchpad's Transform menu rewrites through the jobs strand and the shared client",
          "[widget]") {
    TextFixture fixture(localProvider("llama3.2"));
    auto* service = fixture.context().find<fv::ScratchpadService>();
    REQUIRE(service != nullptr);
    fv::ScratchpadWindow& pad = service->ensureWindow();
    pad.editor()->setPlainText(QStringLiteral("rough words here"));
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("Polish"));

    pad.transformMenu()->actions()[0]->trigger();
    CHECK(pad.editor()->isReadOnly());
    REQUIRE(fixture.waitFor([&] { return !pad.editor()->isReadOnly(); }));
    CHECK(pad.text() == QStringLiteral("Polished."));
    CHECK(pad.statusLabel()->text().startsWith(QStringLiteral("Polish applied")));
    // A local server is asked what it serves first; the rewrite itself is one chat request.
    int chats = 0;
    for (const fv::http::Request& request : fixture.http().requests()) {
        chats += request.url.ends_with("/chat/completions") ? 1 : 0;
    }
    CHECK(chats == 1);
}

TEST_CASE("without credentials, paths or executors the feature leaves the menu empty", "[widget]") {
    // The partial context of the other record tests: no jobs strand, no HTTP client.
    fvtest::AppTheme theme;
    fv::testkit::TempDataRoot root;
    fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    fv::MemorySettingsStore settings(localProvider("llama3.2"));
    fv::ManualExecutor persistence;
    fv::qt::QtUiExecutor ui(qApp);
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    services.settings = &settings;
    services.ui = &ui;
    services.persistence = &persistence;
    fv::AppContext context(services);
    fv::RecordFeature feature;
    feature.provide(context);
    feature.install(context);

    auto* service = context.find<fv::ScratchpadService>();
    REQUIRE(service != nullptr);
    fv::ScratchpadWindow& pad = service->ensureWindow();
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("No transforms yet"));
    CHECK_FALSE(pad.transformMenu()->actions()[0]->isEnabled());
}
