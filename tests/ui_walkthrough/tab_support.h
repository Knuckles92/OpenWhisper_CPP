#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDate>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QMenu>
#include <QMessageBox>
#include <QString>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "review_support.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/agent_picker.h"

// What the Walkthrough tab's tests share: an in-memory settings file, a captures folder of the
// test's own, a ManualExecutor for the reads and scans the tab starts, the storyboard's host
// (review_support.h), and the modal dialogs answered by the test (Python monkeypatches
// QInputDialog, QMessageBox and NewStyleDialog.exec).
namespace fvtest {

// The scan seam put back once a test that replaced it is done.
class ScopedScanSeam {
public:
    ScopedScanSeam() = default;
    ~ScopedScanSeam() { fv::agent_picker::resetScanSeam(); }
    ScopedScanSeam(const ScopedScanSeam&) = delete;
    ScopedScanSeam& operator=(const ScopedScanSeam&) = delete;
};

// Runs the event loop for `ms` milliseconds.
inline void spin(int ms = 0) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    } while (timer.elapsed() < ms);
}

struct TabRig {
    struct Options {
        fv::Json settings = fv::Json::object();
        // The captures folder; empty: <scratch>/captures, which does not exist.
        std::filesystem::path root;
        std::string platform = "win32";
        QDate today = QDate(2026, 10, 21);
    };

    fv::testkit::TempDataRoot scratch;
    std::unique_ptr<fv::SettingsStore> settings;
    fv::ManualExecutor io;
    Host host;
    std::unique_ptr<fv::WalkthroughTab> tab;
    // What the tab's dialogs are answered with, and what they were asked.
    std::optional<QString> styleName;
    std::vector<QString> warnings;
    QMessageBox::StandardButton answer = QMessageBox::Yes;
    int questions = 0;
    std::function<int(QDialog&)> dialog = [](QDialog&) {
        return int(QDialog::Rejected);
    };
    std::vector<QDialog*> dialogs;
    QMessageBox::StandardButton reviewAnswer = QMessageBox::Yes;

    TabRig() : TabRig(Options{}) {}
    explicit TabRig(Options options)
        : settings(
              std::make_unique<fv::SettingsStore>(std::make_unique<fv::MemorySettingsFile>())) {
        settings->save_all(options.settings);
        fv::WalkthroughTabOptions made;
        made.capturesRoot =
            options.root.empty() ? scratch.path() / "captures" : std::move(options.root);
        made.io = &io;
        made.today = [today = options.today] {
            return today;
        };
        made.platform = options.platform;
        tab = std::make_unique<fv::WalkthroughTab>(*settings, host.callbacks(), std::move(made));
        tab->setTextPrompt([this](QWidget*, const QString&, const QString&) { return styleName; });
        tab->setWarning(
            [this](QWidget*, const QString&, const QString& text) { warnings.push_back(text); });
        tab->setQuestion([this](QWidget*, const QString&, const QString&,
                                QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            ++questions;
            return answer;
        });
        tab->setDialogRunner([this](QDialog& shown) {
            dialogs.push_back(&shown);
            return dialog(shown);
        });
        tab->review()->setQuestion([this](QWidget*, const QString&, const QString&,
                                          QMessageBox::StandardButtons,
                                          QMessageBox::StandardButton) { return reviewAnswer; });
        tab->review()->setDialogRunner([](QDialog&) { return 0; });
        tab->review()->setOpener([](const QUrl&) { return true; });
    }
    ~TabRig() {
        // Nothing asks about the edits a test leaves behind.
        tab->review()->setDirty(false);
        tab->promptPopover()->hide();
        tab->close();
        tab.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    TabRig(const TabRig&) = delete;
    TabRig& operator=(const TabRig&) = delete;

    fv::WalkthroughTab* operator->() const { return tab.get(); }
    fv::WalkthroughTab& operator*() const { return *tab; }

    // Runs what the tab handed its reader and brings the answers to the GUI thread
    // (_recent_loaded).
    void settle() {
        for (int round = 0; round < 5; ++round) {
            io.run_all();
            QCoreApplication::sendPostedEvents();
            QApplication::processEvents();
        }
    }
    // The storyboard's work (the host's executor), run to its end.
    void wait() { host.executor.run_all(); }

    fv::Json saved() const { return *settings->snapshot(); }
    bool has(const char* key) const { return saved().contains(key); }
};

// Chooses the item starting with `prefix` from `field`'s menu, as a click would (_pick).
inline void pick(fv::WalkthroughField* field, const QString& prefix) {
    std::unique_ptr<QMenu> menu(field->buildMenu());
    for (QAction* action : menu->actions()) {
        if (action->text().startsWith(prefix)) {
            action->trigger();
            return;
        }
    }
    FAIL("no menu item starts with " << prefix.toStdString());
}

// The texts of a fresh menu's actions.
inline QStringList menuTexts(fv::WalkthroughField* field, bool checkedOnly = false) {
    std::unique_ptr<QMenu> menu(field->buildMenu());
    QStringList texts;
    for (QAction* action : menu->actions()) {
        if (!checkedOnly || action->isChecked()) {
            texts << action->text();
        }
    }
    return texts;
}

} // namespace fvtest
