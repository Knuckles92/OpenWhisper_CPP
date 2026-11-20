#pragma once

// What the Remote engine page tests share: the page over the remote fakes with a real io pool, a
// pinned clock, answered prompts and a recorded URL opener, and a pump that waits for the page's
// io jobs to come back.

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "remote/testing/fake_record_sync.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/remote_engine_page.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fvtest {

// The application styled as OpenWhisper's QtApplication styles it, for one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

// Processes events until `done` holds or `timeout` passes; whether it held.
inline bool pumpUntil(const std::function<bool()>& done,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents();
        // Rows the page replaced go when control returns to an event loop; tests run none.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (done()) {
            // Widgets added meanwhile are shown by a queued call, and replaced ones deleted.
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents();
    return done();
}

// Processes events for `period`.
inline void pumpFor(std::chrono::milliseconds period) {
    pumpUntil([] { return false; }, period);
}

// 2026-11-14 10:00 UTC, in Pacific time: the clock the references were drawn with.
inline std::chrono::system_clock::time_point referenceTime() {
    using namespace std::chrono;
    return sys_days{year{2026} / November / 14} + hours{10};
}

// The page over FakeRemoteEngine and FakeRecordSync. Members are destroyed in reverse: the page
// first, then the pool (which waits for the jobs still running), then the fakes they call.
template <class Engine = fv::remote::testing::FakeRemoteEngine> struct BasicPageHarness {
    struct Options {
        bool bound = true;
        bool records = true;
        // Fills the fakes before the page is built.
        std::function<void(Engine&, fv::remote::testing::FakeRecordSync&)> script;
        std::function<void(fv::RemoteEnginePageServices&)> customize;
    };

    BasicPageHarness() : BasicPageHarness(Options{}) {}
    explicit BasicPageHarness(Options options) : clock(referenceTime(), "America/Los_Angeles") {
        records.status_value.location = "local";
        if (options.script) {
            options.script(engine, records);
        }
        fv::RemoteEnginePageServices services;
        if (options.bound) {
            services.service = &engine;
            services.io = &pool;
        }
        if (options.records) {
            services.records = &records;
        }
        services.clock = &clock;
        services.setRailValue = [this](const QString& text) {
            rail = text;
        };
        services.openUrl = [this](const QUrl& url) {
            opened.push_back(url.toString());
        };
        services.prompts.askName = [this](QWidget*, const QString& title, const QString&,
                                          const QString& current, const QString& own) {
            asked.push_back({title, current, own});
            return nameAnswer;
        };
        services.prompts.question = [this](QWidget*, const QString& title, const QString& text) {
            questions.push_back(title + QStringLiteral(": ") + text);
            return questionAnswer;
        };
        services.prompts.warning = [this](QWidget*, const QString& title, const QString& text) {
            warnings.push_back(title + QStringLiteral(": ") + text);
        };
        services.prompts.information = [this](QWidget*, const QString& title, const QString& text) {
            informations.push_back(title + QStringLiteral(": ") + text);
        };
        services.prompts.removeDevice = [this](QWidget*, const QString& name,
                                               const QString& stored) {
            questions.push_back(
                QStringLiteral("Remove paired computer: %1 keeps %2 here.").arg(name, stored));
            return removeAnswer;
        };
        if (options.customize) {
            options.customize(services);
        }
        page = std::make_unique<fv::RemoteEnginePage>(container, std::move(services));
        page->resize(900, 1200);
    }
    ~BasicPageHarness() { page.reset(); }
    BasicPageHarness(const BasicPageHarness&) = delete;
    BasicPageHarness& operator=(const BasicPageHarness&) = delete;

    // Waits until the newest state read has been drawn and no search is running.
    bool settle() {
        return pumpUntil([this] { return !page->refreshing() && !page->scanBusy(); });
    }
    // Shows the page and waits for its first look around.
    bool show() {
        page->show();
        page->onShown();
        return pumpUntil([this] {
            return !page->refreshing() && !page->scanBusy() &&
                   (page->hasScan() || !page->findTile()->isVisibleTo(page.get()) ||
                    engine.client_pairing().has_value());
        });
    }

    // The texts of the found computers' rows.
    QStringList rows() const {
        QStringList texts;
        for (fv::WrappedLabel* label : page->findList()->findChildren<fv::WrappedLabel*>()) {
            if (label->objectName() == QStringLiteral("remoteFindLabel") && !label->isHidden()) {
                texts.append(label->text());
            }
        }
        return texts;
    }
    // Every label the find list shows (rows and notes).
    QStringList findTexts() const {
        QStringList texts;
        for (fv::WrappedLabel* label : page->findList()->findChildren<fv::WrappedLabel*>()) {
            if (!label->isHidden()) {
                texts.append(label->text());
            }
        }
        return texts;
    }
    // A visible button under `root` with this text, object name or buttonId.
    static QPushButton* button(QWidget* root, const QString& key) {
        for (QPushButton* candidate : root->findChildren<QPushButton*>()) {
            if (candidate->isHidden()) {
                continue;
            }
            if (candidate->text() == key || candidate->objectName() == key ||
                candidate->property(fv::ui::prop::buttonId).toString() == key) {
                return candidate;
            }
        }
        return nullptr;
    }
    bool called(const std::string& call) const {
        const auto calls = engine.calls();
        return std::find(calls.begin(), calls.end(), call) != calls.end();
    }
    bool recordsCalled(const std::string& call) const {
        const auto calls = records.calls();
        return std::find(calls.begin(), calls.end(), call) != calls.end();
    }

    Engine engine;
    fv::remote::testing::FakeRecordSync records;
    fv::FakeClock clock;
    fv::ThreadPool pool{"remote-page-test-io", 4};
    fv::SettingsPage container;
    std::unique_ptr<fv::RemoteEnginePage> page;

    QString rail;
    std::vector<QString> opened;
    struct Asked {
        QString title;
        QString current;
        QString own;
    };
    std::vector<Asked> asked;
    std::optional<QString> nameAnswer;
    std::vector<QString> questions;
    bool questionAnswer = true;
    std::vector<QString> warnings;
    std::vector<QString> informations;
    fv::RemoteEnginePrompts::RemoveChoice removeAnswer =
        fv::RemoteEnginePrompts::RemoveChoice::KeepRecords;
};

using PageHarness = BasicPageHarness<>;

// A paired host, as the fake's pairing.
inline fv::remote::ClientPairing pairedWith(std::string host = "192.168.1.20",
                                            std::string name = "devbox") {
    fv::remote::ClientPairing pairing;
    pairing.host = std::move(host);
    pairing.port = 47821;
    pairing.fingerprint = std::string(64, 'A');
    pairing.host_name = std::move(name);
    pairing.device_id = "this-device";
    pairing.paired_at = "2026-11-14T09:00:00+00:00";
    pairing.via = "code";
    return pairing;
}

// An engine as describe() reports one.
inline fv::Json fakeEngine(bool available = true) {
    return fv::Json{{"family", "parakeet"},
                    {"model", "parakeet-v3"},
                    {"label", "Fake Parakeet"},
                    {"device", "cuda"},
                    {"streaming", false},
                    {"available", available},
                    {"status", available ? "" : "The host is loading its speech engine."}};
}

} // namespace fvtest
