#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <string>

#include "appkit/app_context.h"
#include "base/testing/manual_executor.h"
#include "remote/testing/fake_remote_engine.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_remote/remote_feature.h"
#include "ui_remote/remote_models_dialog.h"
#include "ui_remote_support.h"
#include "ui_settings/settings_window.h"

TEST_CASE("a paired runtime language change requires confirmation for the named host",
          "[widget][remote_models]") {
    QWidget window;
    auto remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    remote->pairing = fvtest::pairedWith();
    const auto fingerprint = remote->pairing->fingerprint;
    remote->request_reply = {
        {"engine", {{"label", "Parakeet TDT"}, {"model", "parakeet"}}},
        {"can_configure_runtime", true},
        {"active_runtime",
         {{"family", "parakeet"},
          {"model", "parakeet"},
          {"languages", {"auto", "en"}},
          {"selected", {{"language", "auto"}}}}},
    };
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    fv::openRemoteModelsDialog(&window, QStringLiteral("studio"), fingerprint, remote, io, ui, {});
    io.run_all();
    ui.run_all();
    auto* dialog = window.findChild<QDialog*>();
    REQUIRE(dialog);
    auto* languages = dialog->findChild<QComboBox*>(QStringLiteral("remoteHostLanguage"));
    REQUIRE(languages);
    REQUIRE(languages->isEnabled());
    languages->setCurrentIndex(languages->findData(QStringLiteral("en")));
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("remoteSetHostLanguage"));
    REQUIRE(apply);
    REQUIRE(apply->isEnabled());
    QString question;
    const auto answer = [&](QMessageBox::StandardButton choice) {
        QTimer::singleShot(0, dialog, [&, choice] {
            auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (message) {
                question = message->text();
                message->button(choice)->click();
            } else {
                dialog->close();
            }
        });
        apply->click();
        io.run_all();
        ui.run_all();
    };
    const auto attempted = [&] {
        return std::ranges::any_of(remote->calls(), [](const std::string& call) {
            return call.starts_with("remote_model_request(configure_runtime,") &&
                   call.find("\"language\":\"en\"") != std::string::npos;
        });
    };

    answer(QMessageBox::No);
    CHECK(question.contains(QStringLiteral("studio")));
    CHECK(question.contains(QStringLiteral("English")));
    CHECK_FALSE(attempted());
    answer(QMessageBox::Yes);
    CHECK(attempted());
}

TEST_CASE("an old manage models button cannot retarget another host", "[widget][remote_models]") {
    QWidget window;
    auto remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    remote->pairing = fvtest::pairedWith();
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    fv::openRemoteModelsDialog(&window, QStringLiteral("studio"), "old-fingerprint", remote, io, ui,
                               {});
    io.run_all();
    ui.run_all();
    CHECK_FALSE(std::ranges::any_of(remote->calls(), [](const std::string& call) {
        return call.starts_with("remote_model_request(");
    }));
}

TEST_CASE("host model navigation uses the settings snapshot and reuses the current fingerprint",
          "[widget][remote_models]") {
    fv::MemorySettingsStore settings;
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    fv::AppContext context(fv::AppServices{.settings = &settings, .ui = &ui, .io = &io});
    QWidget mainWindow;
    mainWindow.show();
    auto settingsWindow = std::make_shared<fv::SettingsWindow>(context);
    settingsWindow->setParentWindow(&mainWindow);
    context.provide<fv::SettingsWindow>(settingsWindow);
    auto remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    context.provide<fv::remote::RemoteEngineControl>(remote);
    fv::RemoteFeature feature;
    feature.install(context);

    const auto pair = [&](std::string fingerprint) {
        auto pairing = fvtest::pairedWith();
        pairing.fingerprint = std::move(fingerprint);
        remote->pairing = pairing;
        settings.save_setting(fv::keys::remote_engine_client.key(),
                              fv::Json{{"host", pairing.host},
                                       {"host_name", pairing.host_name},
                                       {"port", pairing.port},
                                       {"fingerprint", pairing.fingerprint}});
    };
    const auto topic = QStringLiteral("remote_models");
    const auto name = QStringLiteral("remoteModelsDialog");
    const auto dialogs = [&] {
        return mainWindow.findChildren<QDialog*>(name, Qt::FindDirectChildrenOnly);
    };

    pair("first-peer");
    settingsWindow->openEngineHelp(topic);
    CHECK(remote->calls().empty());
    REQUIRE(dialogs().size() == 1);
    const QPointer<QDialog> first(dialogs().front());
    settingsWindow->openEngineHelp(topic);
    REQUIRE(dialogs().size() == 1);
    CHECK(dialogs().front() == first.data());
    CHECK(remote->calls().empty());
    io.run_all();
    ui.run_all();
    CHECK(std::ranges::count_if(remote->calls(), [](const std::string& call) {
              return call.starts_with("remote_model_request(");
          }) == 1);

    pair("other-peer-with-same-host-name");
    settingsWindow->openEngineHelp(topic);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(first.isNull());
    REQUIRE(dialogs().size() == 1);
    io.run_all();
    ui.run_all();
    CHECK(std::ranges::count_if(remote->calls(), [](const std::string& call) {
              return call.starts_with("remote_model_request(");
          }) == 2);
}
