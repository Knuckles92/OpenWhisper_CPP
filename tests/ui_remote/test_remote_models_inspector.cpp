// The paired host's model library dialog: its list, inspector, texts and confirmations
// (ui_qt/dialogs/remote_models.py RemoteModelsDialog), over a fake host answering a catalog.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <string>

#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/remote_models_dialog.h"
#include "ui_remote_support.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;

namespace {

Json catalog() {
    return Json{
        {"engine",
         {{"family", "parakeet"},
          {"model", "parakeet-tdt-0.6b-v3"},
          {"label", "Parakeet TDT 0.6B v3"},
          {"device", "cuda"},
          {"available", true}}},
        {"runtime", {{"gpu", {{"name", "RTX 4090"}, {"total_mib", 24576}}}}},
        {"can_select", true},
        {"can_install_runtime", true},
        {"models",
         Json::array(
             {Json{{"family", "parakeet"},
                   {"model", "parakeet-tdt-0.6b-v3"},
                   {"label", "Parakeet TDT 0.6B v3"},
                   {"cached", true},
                   {"runtime_ready", true},
                   {"selected_device", "cuda"},
                   {"dependencies",
                    Json::array({Json{{"device", "cpu"}, {"label", "CPU runtime"}, {"ready", true}},
                                 Json{{"device", "cuda"},
                                      {"label", "GPU Acceleration"},
                                      {"ready", true}}})}},
              Json{{"family", "parakeet"},
                   {"model", "parakeet-tdt-0.6b-v2"},
                   {"label", "Parakeet TDT 0.6B v2"},
                   {"cached", false},
                   {"download_size", "640 MB"},
                   {"dependencies",
                    Json::array({Json{{"device", "cpu"}, {"label", "CPU runtime"}, {"ready", true}},
                                 Json{{"device", "cuda"},
                                      {"label", "GPU Acceleration"},
                                      {"installable", true},
                                      {"download_bytes", 1730000000}}})}}})},
    };
}

struct Opened {
    Opened() {
        remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
        remote->pairing = fvtest::pairedWith();
        remote->request_reply = catalog();
        fv::openRemoteModelsDialog(&window, QStringLiteral("studio"), remote->pairing->fingerprint,
                                   remote, io, ui, {});
        pump();
        dialog = window.findChild<QDialog*>(QStringLiteral("remoteModelsDialog"));
        REQUIRE(dialog != nullptr);
        list = dialog->findChild<QListWidget*>(QStringLiteral("remoteModelList"));
        REQUIRE(list != nullptr);
    }
    void pump() {
        io.run_all();
        ui.run_all();
    }
    QString text(const char* name) const {
        auto* label = dialog->findChild<fv::WrappedLabel*>(QString::fromLatin1(name));
        return label != nullptr ? label->text() : QString();
    }
    QPushButton* button(const QString& text) const {
        for (auto* candidate : dialog->findChildren<QPushButton*>()) {
            if (candidate->text() == text) {
                return candidate;
            }
        }
        return nullptr;
    }
    QComboBox* devices() const {
        return dialog->findChild<QComboBox*>(QStringLiteral("remoteRuntimeChoice"));
    }
    // Clicks `button` and answers the confirmation with `choice`; returns its text.
    QString confirmWith(QPushButton* target, QMessageBox::StandardButton choice) {
        QString question;
        QTimer::singleShot(0, dialog, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                question = box->windowTitle() + QStringLiteral("|") + box->text();
                box->button(choice)->click();
            }
        });
        target->click();
        pump();
        return question;
    }
    bool asked(const std::string& prefix) const {
        return std::ranges::any_of(
            remote->calls(), [&](const std::string& call) { return call.starts_with(prefix); });
    }

    QWidget window;
    std::shared_ptr<fv::remote::testing::FakeRemoteEngine> remote;
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    QDialog* dialog = nullptr;
    QListWidget* list = nullptr;
};

} // namespace

TEST_CASE("the host model library names the host and its engine", "[widget][remote_models]") {
    Opened opened;
    CHECK(opened.dialog->windowTitle() == QStringLiteral("Models on studio"));
    CHECK(opened.dialog->minimumSize() == QSize(760, 570));
    CHECK(opened.text("downloadsTitle") == QStringLiteral("Models on studio"));
    CHECK(opened.text("remoteHostSummary") ==
          QStringLiteral(u"Running: Parakeet TDT 0.6B v3 · GPU · RTX 4090 (24 GB VRAM)"));
    REQUIRE(opened.list->count() == 2);
    CHECK(opened.list->item(0)->text() == QStringLiteral("Parakeet TDT 0.6B v3\nActive"));
    CHECK(opened.list->item(1)->text() == QStringLiteral("Parakeet TDT 0.6B v2\n640 MB"));
    // The engine the host runs is selected first.
    CHECK(opened.list->currentRow() == 0);
    CHECK(opened.text("remoteModelTitle") == QStringLiteral("Parakeet TDT 0.6B v3"));
    CHECK(opened.text("remoteModelStatus") ==
          QStringLiteral("Download a model, prepare its device, then choose Use on host."));
    QPushButton* use = opened.button(QStringLiteral("Active on host"));
    REQUIRE(use != nullptr);
    CHECK_FALSE(use->isEnabled());
    REQUIRE(opened.devices() != nullptr);
    CHECK(opened.devices()->currentText() == QStringLiteral(u"GPU · Ready"));
    auto* search = opened.dialog->findChild<QLineEdit*>(QStringLiteral("remoteModelSearch"));
    REQUIRE(search != nullptr);
    CHECK(search->placeholderText() == QStringLiteral(u"Search host models…"));
}

TEST_CASE("searching the host's models hides the others and says when none match",
          "[widget][remote_models]") {
    Opened opened;
    auto* search = opened.dialog->findChild<QLineEdit*>(QStringLiteral("remoteModelSearch"));
    search->setText(QStringLiteral("v2"));
    CHECK(opened.list->item(0)->isHidden());
    CHECK_FALSE(opened.list->item(1)->isHidden());
    CHECK(opened.list->currentRow() == 1);
    search->setText(QStringLiteral("whisper"));
    auto* empty = opened.dialog->findChild<fv::WrappedLabel*>(QStringLiteral("remoteModelEmpty"));
    REQUIRE(empty != nullptr);
    CHECK(empty->text() == QStringLiteral("No matching models"));
    CHECK_FALSE(empty->isHidden());
}

TEST_CASE("a model not downloaded offers its download and its device's runtime",
          "[widget][remote_models]") {
    Opened opened;
    opened.list->setCurrentRow(1);
    QPushButton* download = opened.button(QStringLiteral("Download model on host"));
    REQUIRE(download != nullptr);
    CHECK(download->isEnabled());
    // Nothing ready on the GPU, so the CPU is chosen; the GPU needs setting up.
    CHECK(opened.devices()->currentText() == QStringLiteral(u"CPU · Ready"));
    CHECK(opened.devices()->itemText(1) == QStringLiteral(u"GPU · Setup needed"));
    opened.devices()->setCurrentIndex(1);
    QPushButton* install = opened.button(QStringLiteral("Install runtime on host"));
    REQUIRE(install != nullptr);
    CHECK(install->isEnabled());
    const auto labels = opened.dialog->findChildren<fv::WrappedLabel*>();
    CHECK(std::ranges::any_of(labels, [](const fv::WrappedLabel* label) {
        return label->text() == QStringLiteral("GPU Acceleration runtime is missing. Install it on "
                                               "the host (1.73 GB) to use this device.");
    }));
    CHECK(std::ranges::any_of(labels, [](const fv::WrappedLabel* label) {
        return label->text() == QStringLiteral("Model download: 640 MB");
    }));

    CHECK(opened.confirmWith(download, QMessageBox::No) ==
          QStringLiteral("Download on host?|Download Parakeet TDT 0.6B v2 (640 MB) on studio? "
                         "This uses the host's network and disk space."));
    CHECK_FALSE(opened.asked("remote_model_request(download_model,"));
    CHECK(opened.confirmWith(install, QMessageBox::Yes) ==
          QStringLiteral("Install runtime on host?|Install GPU Acceleration on studio? This "
                         "downloads verified runtime files using the host's network and disk "
                         "space."));
    CHECK(opened.asked("remote_model_request(install_runtime,"));
}

TEST_CASE("choosing another downloaded model asks before changing the host's engine",
          "[widget][remote_models]") {
    Opened opened;
    Json reply = catalog();
    reply["engine"]["model"] = "other";
    opened.remote->request_reply = reply;
    QPushButton* refresh = opened.button(QStringLiteral("Refresh host"));
    REQUIRE(refresh != nullptr);
    refresh->click();
    opened.pump();
    opened.list->setCurrentRow(0);
    QPushButton* use = opened.button(QStringLiteral("Use on host"));
    REQUIRE(use != nullptr);
    REQUIRE(use->isEnabled());
    CHECK(opened.confirmWith(use, QMessageBox::Yes) ==
          QStringLiteral("Change the host's engine?|Use Parakeet TDT 0.6B v3 on studio using GPU? "
                         "This changes the engine for the host and every connected client."));
    CHECK(std::ranges::any_of(opened.remote->calls(), [](const std::string& call) {
        return call.starts_with("remote_model_request(select_model,") &&
               call.find("\"device\":\"cuda\"") != std::string::npos;
    }));
}

TEST_CASE("a download running on the host shows its progress and polls",
          "[widget][remote_models]") {
    Opened opened;
    Json reply = catalog();
    reply["download"] = {
        {"state", "downloading"}, {"label", "Parakeet TDT 0.6B v2"}, {"done", 25}, {"total", 100}};
    reply["installation"] = {
        {"state", "installing"}, {"label", "GPU Acceleration"}, {"phase", "Verifying"}};
    opened.remote->request_reply = reply;
    opened.button(QStringLiteral("Refresh host"))->click();
    opened.pump();
    CHECK(opened.text("remoteModelStatus") ==
          QStringLiteral(u"Downloading Parakeet TDT 0.6B v2 on host (25%)…"));
    CHECK(opened.text("remoteInstallStatus") == QStringLiteral(u"GPU Acceleration · Verifying…"));
    const auto bars = opened.dialog->findChildren<QProgressBar*>();
    CHECK(std::ranges::count_if(bars, [](QProgressBar* bar) { return !bar->isHidden(); }) == 2);

    reply["download"] = {{"state", "failed"}};
    reply["installation"] = {{"state", "complete"}, {"label", "GPU Acceleration"}};
    opened.remote->request_reply = reply;
    opened.button(QStringLiteral("Refresh host"))->click();
    opened.pump();
    CHECK(opened.text("remoteModelStatus") ==
          QStringLiteral("Model download failed. Retry the download."));
    CHECK(opened.text("remoteInstallStatus") ==
          QStringLiteral("GPU Acceleration installed. Ready to use on the host."));
}

TEST_CASE("a failed request says to refresh", "[widget][remote_models]") {
    Opened opened;
    opened.remote->fail_next(std::make_exception_ptr(std::runtime_error("studio went away.")));
    opened.button(QStringLiteral("Refresh host"))->click();
    opened.pump();
    CHECK(opened.text("remoteModelStatus") ==
          QStringLiteral("studio went away. Refresh to check the host's current state."));
    // An older host that lists no runtimes says so.
    Json reply = catalog();
    reply["models"][0].erase("dependencies");
    opened.remote->request_reply = reply;
    opened.button(QStringLiteral("Refresh host"))->click();
    opened.pump();
    const auto labels = opened.dialog->findChildren<fv::WrappedLabel*>();
    CHECK(std::ranges::any_of(labels, [](const fv::WrappedLabel* label) {
        return label->text() ==
               QStringLiteral(u"Update %1 on the host to manage its CPU/GPU runtimes here. "
                              u"Missing runtimes can be installed in Settings → Downloads on the "
                              u"host.")
                   .arg(QString::fromUtf8(fv::identity().product));
    }));
}
