#include <QApplication>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <memory>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/action_model.h"
#include "automation/action_result.h"
#include "automation/event_bus.h"
#include "automation/programs.h"
#include "automation/webhooks.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "settings/testing/memory_store.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/automation_feature.h"

TEST_CASE("manual automation tests work without subscribing saved hooks", "[widget]") {
    using namespace std::chrono_literals;
    fv::MemorySettingsStore settings(fv::Json{
        {"automation_programs", fv::Json::array({{{"id", "p"},
                                                  {"name", "Program"},
                                                  {"enabled", true},
                                                  {"events", {"recording.started"}},
                                                  {"program", "fake.exe"},
                                                  {"args", fv::Json::array()},
                                                  {"timeout_s", 1}}})},
        {"automation_webhooks", fv::Json::array({{{"id", "w"},
                                                  {"name", "Webhook"},
                                                  {"enabled", true},
                                                  {"events", {"recording.started"}},
                                                  {"url", "https://example.test/hook"}}})}});
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    fv::http::FakeHttpClient http;
    http.push({});
    fv::automation::EventBus events;
    fv::qt::QtUiExecutor ui(qApp);
    fv::ThreadPool io("manual-test", 1);
    std::atomic<int> runs = 0;
    fv::AppServices services;
    services.settings = &settings;
    services.credentials = &credentials;
    services.http = &http;
    services.events = &events;
    services.ui = &ui;
    services.io = &io;
    {
        fv::AppContext context(services);
        auto options = std::make_shared<fv::automation::ProgramOptions>();
        options->run_once = [&runs](const auto&, std::string_view, double, const auto&,
                                    const auto&) {
            ++runs;
            fv::RunOnceResult result;
            result.exit_code = 0;
            return result;
        };
        context.provide<fv::automation::ProgramOptions>(options);
        fv::AutomationFeature feature;
        feature.provide(context);
        auto* programs = context.find<fv::automation::ProgramControl>();
        auto* webhooks = context.find<fv::automation::WebhookControl>();
        REQUIRE(programs);
        REQUIRE(webhooks);
        REQUIRE(context.find<fv::automation::ActionTester>());
        fv::automation::Action action;
        action.kind = fv::automation::ActionKind::CHANNEL;
        action.name = "Session";
        const auto previousQuestion = fv::action_prompts::question();
        struct RestoreQuestion {
            fv::action_prompts::Question previous;
            ~RestoreQuestion() { fv::action_prompts::question() = std::move(previous); }
        } restore{previousQuestion};
        QString promptText;
        fv::action_prompts::question() = [&promptText](QWidget*, const QString&,
                                                       const QString& text, auto, auto) {
            promptText = text;
            return QMessageBox::Cancel;
        };
        bool actionAnswered = false;
        context.service<fv::automation::ActionTester>().test(
            action, "sample", [&actionAnswered](auto) { actionAnswered = true; });
        CHECK(actionAnswered);
        CHECK(promptText.contains(QStringLiteral("sample")));
        CHECK(promptText.contains(QStringLiteral("Session")));
        CHECK(events.subscribers() == 0);
        events.publish_event("recording.started", fv::Json::object());
        CHECK(runs == 0);
        CHECK(http.requests().empty());
        auto answer = std::make_shared<std::promise<fv::automation::RunRecord>>();
        auto result = answer->get_future();
        programs->run_test("p", [answer](auto record) { answer->set_value(std::move(record)); });
        REQUIRE(result.wait_for(5s) == std::future_status::ready);
        CHECK(result.get().ok);
        CHECK(runs == 1);
        auto delivery = std::make_shared<std::promise<fv::automation::DeliveryRecord>>();
        auto delivered = delivery->get_future();
        webhooks->send_test("w",
                            [delivery](auto record) { delivery->set_value(std::move(record)); });
        REQUIRE(delivered.wait_for(5s) == std::future_status::ready);
        const auto record = delivered.get();
        INFO(record.error);
        CHECK(record.ok);
        CHECK(http.requests().size() == 1);
    }
    io.shutdown(5s);
}

TEST_CASE("manual action tester is available when the app context has no event sink", "[widget]") {
    fv::MemorySettingsStore settings(fv::Json::object());
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    fv::http::FakeHttpClient http;
    fv::qt::QtUiExecutor ui(qApp);
    fv::ThreadPool io("manual-no-events", 1);
    fv::AppServices services;
    services.settings = &settings;
    services.credentials = &credentials;
    services.http = &http;
    services.ui = &ui;
    services.io = &io;
    {
        fv::AppContext context(services);
        fv::AutomationFeature feature;
        feature.provide(context);
        CHECK(context.find<fv::automation::ActionTester>());
    }
    io.shutdown(std::chrono::seconds(5));
}
