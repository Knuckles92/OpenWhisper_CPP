// The Manage host models dialog while the host works (ui_qt/dialogs/remote_models.py
// RemoteModelsDialog in tests/test_remote_model_management.py and test_remote_dependencies.py):
// requests run on the io executor, the actions lock while one is in flight, a download or runtime
// installation is confirmed and then polled until the host says it is done, and a dialog closed
// mid-request is not kept by the request. The host is a FakeRemoteEngine answering a catalog.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <string>

#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "remote/errors.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/remote_models_dialog.h"
#include "ui_remote_support.h"
#include "ui_widgets/wrapped_label.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

Json dependency(const char* device, const char* label, bool ready) {
    return Json{{"device", device},
                {"label", label},
                {"ready", ready},
                {"installable", !ready},
                {"download_bytes", 1730000000}};
}

// The host runs Parakeet v3 on its CPU. v2 is downloaded but has no runtime ready; Canary is not
// downloaded.
Json catalog() {
    const Json runtimes = Json::array(
        {dependency("cpu", "CPU runtime", true), dependency("cuda", "GPU Acceleration", false)});
    const Json none = Json::array(
        {dependency("cpu", "CPU runtime", false), dependency("cuda", "GPU Acceleration", false)});
    return Json{
        {"engine",
         {{"family", "parakeet"},
          {"model", "parakeet-tdt-0.6b-v3"},
          {"label", "Parakeet TDT 0.6B v3"},
          {"device", "cpu"},
          {"available", true}}},
        {"can_select", true},
        {"can_install_runtime", true},
        {"models", Json::array({Json{{"family", "parakeet"},
                                     {"model", "parakeet-tdt-0.6b-v3"},
                                     {"label", "Parakeet TDT 0.6B v3"},
                                     {"cached", true},
                                     {"runtime_ready", true},
                                     {"selected_device", "cpu"},
                                     {"dependencies", runtimes}},
                                Json{{"family", "parakeet"},
                                     {"model", "parakeet-tdt-0.6b-v2"},
                                     {"label", "Parakeet TDT 0.6B v2"},
                                     {"cached", true},
                                     {"runtime_ready", false},
                                     {"selected_device", "cpu"},
                                     {"dependencies", none}},
                                Json{{"family", "parakeet"},
                                     {"model", "canary-1b"},
                                     {"label", "Canary 1B"},
                                     {"cached", false},
                                     {"download_size", "1.2 GB"},
                                     {"selected_device", "cpu"},
                                     {"dependencies", runtimes}}})},
    };
}

struct Dialog {
    explicit Dialog(Json reply = catalog()) {
        remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
        remote->pairing = fvtest::pairedWith();
        remote->request_reply = std::move(reply);
        fv::openRemoteModelsDialog(&window, QStringLiteral("studio"), remote->pairing->fingerprint,
                                   remote, io, ui, {});
        dialog = window.findChild<QDialog*>(QStringLiteral("remoteModelsDialog"));
        REQUIRE(dialog != nullptr);
        list = dialog->findChild<QListWidget*>(QStringLiteral("remoteModelList"));
        REQUIRE(list != nullptr);
    }
    ~Dialog() {
        io.run_all();
        ui.run_all();
    }
    // Runs requests and their answers until neither executor has anything left (an answer can
    // start the next request).
    void pump() {
        while (io.queued() > 0 || ui.queued() > 0) {
            io.run_all();
            ui.run_all();
        }
    }
    void reply(Json answer) { remote->request_reply = std::move(answer); }
    QPushButton* button(const char* name) const {
        auto* found = dialog->findChild<QPushButton*>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    }
    QPushButton* buttonText(const QString& text) const {
        for (auto* candidate : dialog->findChildren<QPushButton*>()) {
            if (candidate->text() == text) {
                return candidate;
            }
        }
        FAIL("no button " << text.toStdString());
        return nullptr;
    }
    QPushButton* use() const {
        for (auto* candidate : dialog->findChildren<QPushButton*>()) {
            if (candidate->text() == QStringLiteral("Use on host") ||
                candidate->text() == QStringLiteral("Active on host")) {
                return candidate;
            }
        }
        FAIL("no Use on host button");
        return nullptr;
    }
    QPushButton* refresh() const { return buttonText(QStringLiteral("Refresh host")); }
    QComboBox* devices() const {
        return dialog->findChild<QComboBox*>(QStringLiteral("remoteRuntimeChoice"));
    }
    QString text(const char* name) const {
        auto* label = dialog->findChild<fv::WrappedLabel*>(QString::fromLatin1(name));
        return label != nullptr ? label->text() : QString();
    }
    bool anyLabel(const std::string& part) const {
        const auto labels = dialog->findChildren<fv::WrappedLabel*>();
        return std::ranges::any_of(labels, [&](const fv::WrappedLabel* label) {
            return label->text().toStdString().find(part) != std::string::npos;
        });
    }
    // The dialog's poll: its single-shot timer of two seconds.
    bool polling() const {
        const auto timers = dialog->findChildren<QTimer*>();
        return std::ranges::any_of(timers, [](const QTimer* timer) {
            return timer->interval() == 2000 && timer->isActive();
        });
    }
    void choose(int row) { list->setCurrentRow(row); }
    QString confirmWith(QPushButton* target, QMessageBox::StandardButton choice) {
        QString question;
        QTimer::singleShot(0, dialog, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                question = box->text();
                box->button(choice)->click();
            }
        });
        target->click();
        pump();
        return question;
    }
    int asked(const std::string& prefix) const {
        const auto calls = remote->calls();
        return static_cast<int>(std::ranges::count_if(
            calls, [&](const std::string& call) { return call.starts_with(prefix); }));
    }

    QWidget window;
    std::shared_ptr<fv::remote::testing::FakeRemoteEngine> remote;
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    QDialog* dialog = nullptr;
    QListWidget* list = nullptr;
};

} // namespace

// from
// tests/test_remote_model_management.py::test_management_dialog_runs_off_ui_thread_and_reflects_host_readiness
TEST_CASE("test_management_dialog_runs_off_ui_thread_and_reflects_host_readiness",
          "[widget][remote_models]") {
    Dialog opened;
    // The first catalog read waits on the io executor; nothing can be done meanwhile.
    CHECK(opened.asked("remote_model_request(model_catalog,") == 0);
    CHECK_FALSE(opened.refresh()->isEnabled());
    CHECK_FALSE(opened.list->isEnabled());
    opened.pump();
    CHECK(opened.refresh()->isEnabled());
    // Downloaded, but no runtime for it on the host.
    opened.choose(1);
    CHECK_FALSE(opened.use()->isEnabled());
    CHECK(opened.anyLabel("runtime is missing"));
    opened.choose(0);
    opened.reply([] {
        Json other = catalog();
        other["engine"]["model"] = "canary-1b";
        return other;
    }());
    opened.refresh()->click();
    // A request in flight locks every action.
    CHECK_FALSE(opened.use()->isEnabled());
    CHECK_FALSE(opened.refresh()->isEnabled());
    opened.pump();
    CHECK(opened.use()->isEnabled());
    CHECK(opened.list->currentRow() == 0);
    // Model management turned off on the host: said, and nothing offered.
    opened.remote->fail_next(std::make_exception_ptr(fv::remote::RemoteEngineError(
        "Model management is disabled on the host. Enable it in Settings → Remote engine "
        "there.")));
    opened.refresh()->click();
    opened.pump();
    CHECK_THAT(opened.text("remoteModelStatus").toStdString(), ContainsSubstring("disabled"));
    CHECK_FALSE(opened.use()->isEnabled());
    CHECK_FALSE(opened.button("remoteModelDownloadButton")->isEnabled());
    CHECK_FALSE(opened.polling());
    QPointer<QDialog> closed(opened.dialog);
    opened.dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(closed.isNull()); // its poll goes with it
}

// from
// tests/test_remote_model_management.py::test_dialog_confirms_download_and_polls_until_complete
TEST_CASE("test_dialog_confirms_download_and_polls_until_complete", "[widget][remote_models]") {
    Dialog opened;
    opened.pump();
    opened.choose(2);
    QPushButton* download = opened.button("remoteModelDownloadButton");
    REQUIRE(download->isEnabled());
    CHECK_FALSE(opened.confirmWith(download, QMessageBox::No).isEmpty());
    CHECK(opened.asked("remote_model_request(download_model,") == 0);
    Json running = catalog();
    running["download"] = {{"state", "downloading"}, {"family", "parakeet"}, {"model", "canary-1b"},
                           {"label", "Canary 1B"},   {"done", 25},           {"total", 100}};
    opened.reply(running);
    CHECK_FALSE(opened.confirmWith(download, QMessageBox::Yes).isEmpty());
    CHECK(opened.asked("remote_model_request(download_model,") == 1);
    CHECK(opened.polling());
    CHECK_THAT(opened.text("remoteModelStatus").toStdString(), ContainsSubstring("25%"));
    CHECK_FALSE(download->isEnabled());

    Json done = catalog();
    done["models"][2]["cached"] = true;
    done["models"][2]["runtime_ready"] = true;
    done["download"] = {{"state", "complete"}, {"label", "Canary 1B"}};
    opened.reply(done);
    opened.refresh()->click();
    opened.pump();
    CHECK(opened.list->currentRow() == 2);
    CHECK(opened.use()->isEnabled());
    CHECK_FALSE(opened.polling());
    // Downloading never changed the host's engine.
    CHECK(opened.asked("remote_model_request(select_model,") == 0);
}

// from
// tests/test_remote_model_management.py::test_a_dialog_closed_mid_request_is_not_held_by_the_worker
TEST_CASE("test_a_dialog_closed_mid_request_is_not_held_by_the_worker", "[widget][remote_models]") {
    Dialog opened;
    QPointer<QDialog> dialog(opened.dialog);
    // The catalog request is queued on io; the dialog goes before it runs.
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(dialog.isNull());
    opened.pump(); // the request runs and its answer finds no dialog
    CHECK(opened.asked("remote_model_request(model_catalog,") == 1);
    CHECK(opened.io.queued() == 0);
    CHECK(opened.ui.queued() == 0);
}

// from
// tests/test_remote_dependencies.py::test_browser_installs_then_enables_selected_gpu_without_changing_local_settings
TEST_CASE("test_browser_installs_then_enables_selected_gpu_without_changing_local_settings",
          "[widget][remote_models]") {
    Dialog opened;
    opened.pump();
    opened.choose(0);
    QComboBox* devices = opened.devices();
    REQUIRE(devices != nullptr);
    devices->setCurrentIndex(devices->findData(QStringLiteral("cuda")));
    QPushButton* install = opened.button("remoteRuntimeInstallButton");
    CHECK(install->isEnabled());
    CHECK_FALSE(opened.use()->isEnabled());
    CHECK(opened.anyLabel("GPU Acceleration runtime is missing"));
    CHECK_FALSE(opened.confirmWith(install, QMessageBox::No).isEmpty());
    CHECK(opened.asked("remote_model_request(install_runtime,") == 0);
    Json running = catalog();
    running["installation"] = {{"state", "installing"},
                               {"label", "GPU Acceleration"},
                               {"phase", "downloading"},
                               {"done", 25},
                               {"total", 100}};
    opened.reply(running);
    CHECK_FALSE(opened.confirmWith(install, QMessageBox::Yes).isEmpty());
    REQUIRE(opened.asked("remote_model_request(install_runtime,") == 1);
    const auto calls = opened.remote->calls();
    CHECK(std::ranges::any_of(calls, [](const std::string& call) {
        return call.starts_with("remote_model_request(install_runtime,") &&
               call.find(R"("device":"cuda")") != std::string::npos;
    }));
    CHECK(opened.polling());
    const auto bars = opened.dialog->findChildren<QProgressBar*>();
    CHECK(std::ranges::any_of(
        bars, [](QProgressBar* bar) { return !bar->isHidden() && bar->value() == 25; }));
    CHECK_FALSE(install->isEnabled());

    Json done = catalog();
    done["models"][0]["dependencies"][1]["ready"] = true;
    done["models"][0]["dependencies"][1]["installable"] = false;
    done["installation"] = {{"state", "complete"}, {"label", "GPU Acceleration"}};
    opened.reply(done);
    opened.refresh()->click();
    opened.pump();
    CHECK(devices->currentData().toString() == QStringLiteral("cuda"));
    CHECK(opened.use()->isEnabled());
    CHECK_FALSE(opened.polling());
    // Only the host was asked anything: this computer's settings are not the dialog's to touch.
    for (const auto& call : opened.remote->calls()) {
        CHECK((call.starts_with("remote_model_request(model_catalog,") ||
               call.starts_with("remote_model_request(install_runtime,") ||
               call.starts_with("client_pairing")));
    }
    auto* search = opened.dialog->findChild<QLineEdit*>(QStringLiteral("remoteModelSearch"));
    REQUIRE(search != nullptr);
    search->setText(QStringLiteral("Canary"));
    int shown = 0;
    for (int row = 0; row < opened.list->count(); ++row) {
        shown += opened.list->item(row)->isHidden() ? 0 : 1;
    }
    CHECK(shown == 1);
}

// from
// tests/test_remote_dependencies.py::test_browser_legacy_host_keeps_use_action_and_explains_setup
TEST_CASE("test_browser_legacy_host_keeps_use_action_and_explains_setup",
          "[widget][remote_models]") {
    const Json legacy{{"models", Json::array({Json{{"family", "parakeet"},
                                                   {"model", "parakeet-tdt-0.6b-v2"},
                                                   {"label", "Parakeet TDT 0.6B v2"},
                                                   {"cached", true},
                                                   {"runtime_ready", true}}})},
                      {"can_select", true}};
    Dialog opened(legacy);
    opened.pump();
    CHECK(opened.use()->isEnabled());
    CHECK(opened.anyLabel("Update " + std::string(fv::identity().product)));
    QPushButton* install = opened.button("remoteRuntimeInstallButton");
    CHECK_FALSE(install->isEnabled());
}
