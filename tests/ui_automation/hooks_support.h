#pragma once

// Settings > Automate > Hooks sections over fakes (OpenWhisper's
// tests/test_automation_ui_hooks_page.py fixtures `store`, `open_section`, `answers`, `record` and
// `wait_for`):
// - an in-memory settings file, an io pool for the secret calls, and the webhook and program
//   engines scripted (automation/testing/fake_hook_engines.h);
// - the text filter leases over a fake clock, which the sections read as their clock too;
// - `questions`: every confirmation asked, as (title, text); each answered from `answers` in order
//   (Cancel once it is empty), never a real message box;
// - `copied`: what a secret copy put on the clipboard, never the real clipboard.

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include "actions_page_support.h"
#include "automation/catalog.h"
#include "automation/filters.h"
#include "automation/hooks_model.h"
#include "automation/programs.h"
#include "automation/testing/fake_hook_engines.h"
#include "automation/webhooks.h"
#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "settings/testing/memory_store.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/automation_feature.h"
#include "ui_automation/hooks_editors.h"
#include "ui_automation/hooks_page.h"
#include "ui_automation/hooks_panel.h"

namespace fvtest {

namespace cat = fv::automation::catalog;

inline const Json kHook{{"id", "hook01"},
                        {"name", "Home"},
                        {"url", "https://example.com/in"},
                        {"events", Json::array({std::string(cat::TRANSCRIPTION_COMPLETED)})},
                        {"enabled", true}};

inline Json programRow(const std::string& id = "prog01", const std::string& name = "Journal",
                       const std::string& program = "") {
    return Json{
        {"id", id},
        {"name", name},
        {"program",
         program.empty()
             ? QDir::toNativeSeparators(QFileInfo("journal.exe").absoluteFilePath()).toStdString()
             : program},
        {"args", Json::array({"--append"})},
        {"events", Json::array({std::string(cat::HISTORY_SAVED)})},
        {"enabled", true},
        {"timeout_s", 30}};
}

inline const Json kFilter{{"id", "filt01"},
                          {"name", "Names"},
                          {"kind", "http"},
                          {"stage", "after_cleanup"},
                          {"url", "http://127.0.0.1:5005/fix"},
                          {"timeout_ms", 1500},
                          {"enabled", false}};

// Another webhook, as the "answers" tests have them.
inline Json hookRow(const std::string& id, const std::string& name, const std::string& host) {
    return Json{{"id", id},
                {"name", name},
                {"url", "https://" + host + "/hook"},
                {"events", Json::array({std::string(cat::TRANSCRIPTION_COMPLETED)})},
                {"enabled", true}};
}

// The saved settings with one list set.
inline Json settingsWith(const char* key, std::vector<Json> rows) {
    Json settings = Json::object();
    settings[key] = Json(std::move(rows));
    return settings;
}

// record(): a delivery attempt as Settings lists it.
inline fv::automation::DeliveryRecord record(std::optional<int> status = 200, int elapsed = 120,
                                             bool ok = true, std::string error = {},
                                             std::string eventType = "webhook.test",
                                             int attempt = 1) {
    return {"evt_1",
            std::move(eventType),
            "2026-10-10T18:02:11.120Z",
            ok,
            status,
            elapsed,
            std::move(error),
            attempt};
}

inline fv::automation::RunRecord runRecord(std::string eventId, std::string at, bool ok,
                                           std::optional<int> exitCode, int elapsed,
                                           std::string error = {}) {
    return {std::move(eventId), std::move(at), ok, exitCode, elapsed, std::move(error)};
}

inline std::string text(const QString& value) {
    return fv::qt::toUtf8(value);
}

// A webhook engine whose answers a test scripts call by call: what Python's tests do by patching
// `webhooks.send_test`, `ensure_secret` and `rotate_secret`. Anything left unset behaves as
// FakeWebhookControl does. Affinity: thread-safe.
class ScriptedWebhooks final : public fv::automation::WebhookControl {
public:
    // Runs instead of the fake's send_test; may throw (the engine could not even start).
    std::function<void(const std::string&, std::function<void(fv::automation::DeliveryRecord)>)>
        onSendTest;
    std::function<std::string(const std::string&)> onEnsure;
    std::function<std::string(const std::string&)> onRotate;
    fv::automation::FakeWebhookControl fake;

    std::vector<fv::automation::DeliveryRecord> deliveries(std::string_view id) const override {
        return fake.deliveries(id);
    }
    void send_test(std::string id,
                   std::function<void(fv::automation::DeliveryRecord)> done) override {
        if (onSendTest) {
            onSendTest(id, std::move(done));
            return;
        }
        fake.send_test(std::move(id), std::move(done));
    }
    std::string ensure_secret(std::string_view id) override {
        return onEnsure ? onEnsure(std::string(id)) : fake.ensure_secret(id);
    }
    std::string rotate_secret(std::string_view id) override {
        return onRotate ? onRotate(std::string(id)) : fake.rotate_secret(id);
    }
    void delete_secret(std::string_view id) override { fake.delete_secret(id); }
    void reload() override { fake.reload(); }
};

// The same for program hooks.
class ScriptedPrograms final : public fv::automation::ProgramControl {
public:
    std::function<void(const std::string&, std::function<void(fv::automation::RunRecord)>)>
        onRunTest;
    fv::automation::FakeProgramControl fake;

    std::vector<fv::automation::RunRecord> runs(std::string_view id) const override {
        return fake.runs(id);
    }
    void run_test(std::string id, std::function<void(fv::automation::RunRecord)> done) override {
        if (onRunTest) {
            onRunTest(id, std::move(done));
            return;
        }
        fake.run_test(std::move(id), std::move(done));
    }
    void reload() override { fake.reload(); }
};

// A widget a test made: deleted when the test ends unless a parent got there first (a column
// that holds it, say).
template <class Widget> class Held {
public:
    explicit Held(Widget* widget) : widget_(widget) {}
    ~Held() { delete widget_.data(); }
    Held(Held&&) = default;
    Held& operator=(Held&& other) noexcept {
        if (this != &other) {
            delete widget_.data();
            widget_ = std::move(other.widget_);
        }
        return *this;
    }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    Widget* get() const { return widget_.data(); }
    Widget* operator->() const { return widget_.data(); }
    Widget& operator*() const { return *widget_.data(); }

private:
    QPointer<Widget> widget_;
};

// Everything a section needs, owned for the length of a test case.
class HooksRig {
public:
    explicit HooksRig(const Json& saved = Json::object(), double leaseTtlSeconds = 60.0)
        : store_(saved), leases_(clock_, leaseTtlSeconds), io_("hooks-test-io", 2) {
        previousQuestion_ = fv::action_prompts::question();
        previousCopy_ = fv::action_prompts::copyText();
        fv::action_prompts::question() = [this](QWidget*, const QString& title, const QString& text,
                                                QMessageBox::StandardButtons,
                                                QMessageBox::StandardButton) {
            questions.emplace_back(title, text);
            if (answers.empty()) {
                return QMessageBox::Cancel;
            }
            const QMessageBox::StandardButton next = answers.front();
            answers.pop_front();
            return next;
        };
        fv::action_prompts::copyText() = [this](const QString& copiedText) {
            copied << copiedText;
        };
    }
    ~HooksRig() {
        io_.shutdown(std::chrono::seconds(5));
        fv::action_prompts::question() = previousQuestion_;
        fv::action_prompts::copyText() = previousCopy_;
    }
    HooksRig(const HooksRig&) = delete;
    HooksRig& operator=(const HooksRig&) = delete;

    // What the sections reach: both engines, unless a test leaves one out.
    fv::HookServices services(bool withWebhooks = true, bool withPrograms = true) {
        fv::HookServices services;
        services.store = &store_;
        services.io = &io_;
        services.webhooks = withWebhooks ? &webhooks : nullptr;
        services.programs = withPrograms ? &programs : nullptr;
        services.leases = &leases_;
        services.clock = &clock_;
        return services;
    }

    template <class Section> Held<Section> section() {
        return Held<Section>(new Section(services()));
    }
    template <class Section> Held<Section> section(const fv::HookServices& custom) {
        return Held<Section>(new Section(custom));
    }

    fv::MemorySettingsStore& store() { return store_; }
    fv::FakeClock& clock() { return clock_; }
    fv::automation::FilterLeases& leases() { return leases_; }
    fv::ThreadPool& io() { return io_; }

    // The saved settings now.
    Json stored() { return *store_.snapshot(); }
    // The one item list `key` holds, loaded as the app loads it.
    std::vector<fv::automation::hooks::Webhook> savedWebhooks() {
        return fv::automation::hooks::load_webhooks(stored());
    }
    std::vector<fv::automation::hooks::Program> savedPrograms() {
        return fv::automation::hooks::load_programs(stored());
    }
    std::vector<fv::automation::hooks::TextFilter> savedFilters() {
        return fv::automation::hooks::load_filters(stored());
    }

    fv::automation::FakeWebhookControl webhooks;
    fv::automation::FakeProgramControl programs;
    std::vector<std::pair<QString, QString>> questions;
    std::deque<QMessageBox::StandardButton> answers;
    QStringList copied;

private:
    fv::MemorySettingsStore store_;
    fv::FakeClock clock_;
    fv::automation::FilterLeases leases_;
    fv::ThreadPool io_;
    fv::action_prompts::Question previousQuestion_;
    fv::action_prompts::CopyText previousCopy_;
};

// The Hooks page inside a real Settings window, as the app builds it, with the clock pinned to
// 2026-03-15 15:00 UTC (the zone too) and the engines scripted: what the reference screenshots
// need. Without ActionsRig's stand-ins, since only the Hooks page is built.
class HooksPageRig {
public:
    // `leaseTtl`: how long a lease lives; one made now ends that many seconds from the pinned now.
    explicit HooksPageRig(const Json& saved = Json::object(), double leaseTtl = 42.0)
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), settings_(saved), ui_(qApp),
          clock_(std::chrono::sys_days{std::chrono::year{2026} / std::chrono::March / 15} +
                 std::chrono::hours{15}),
          leases(clock_, leaseTtl), context_(services()), io_("hooks-page-io", 2) {
        const auto lend = [](auto& object) {
            using T = std::remove_reference_t<decltype(object)>;
            return std::shared_ptr<T>(&object, [](T*) {});
        };
        context_.provide<fv::automation::WebhookControl>(
            std::shared_ptr<fv::automation::WebhookControl>(&webhooks, [](auto*) {}));
        context_.provide<fv::automation::ProgramControl>(
            std::shared_ptr<fv::automation::ProgramControl>(&programs, [](auto*) {}));
        context_.provide<fv::automation::FilterLeases>(lend(leases));
        feature_.provide(context_);
        feature_.install(context_);
    }
    ~HooksPageRig() {
        for (auto& dialog : dialogs_) {
            dialog->close();
        }
        dialogs_.clear();
        pump(3);
        io_.shutdown(std::chrono::seconds(5));
    }
    HooksPageRig(const HooksPageRig&) = delete;
    HooksPageRig& operator=(const HooksPageRig&) = delete;

    // The window, shown, on the Hooks page unless `select` is false.
    fv::SettingsDialog& dialog(bool select = true) {
        auto made = std::make_unique<fv::SettingsDialog>(context_);
        made->availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        fv::SettingsDialog& window = *made;
        dialogs_.push_back(std::move(made));
        if (select) {
            window.selectDestination(qs(dest::hooks));
        }
        window.show();
        pump();
        return window;
    }
    fv::HooksPage& page() {
        auto* content = dynamic_cast<fv::HooksPage*>(dialogs_.back()->pageContent(dest::hooks));
        REQUIRE(content != nullptr);
        return *content;
    }
    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    fv::FakeClock& clock() { return clock_; }

    fv::automation::FakeWebhookControl webhooks;
    fv::automation::FakeProgramControl programs;

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.clock = &clock_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.io = &io_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::FakeClock clock_;

public:
    fv::automation::FilterLeases leases;

private:
    fv::AppContext context_;
    // After the context: destroyed first, so no io task outlives what it reaches.
    fv::ThreadPool io_;
    fv::AutomationFeature feature_;
    std::vector<std::unique_ptr<fv::SettingsDialog>> dialogs_;
};

// close(): a widget gone, deferred deletes included.
inline void closeWidget(QWidget& widget) {
    widget.close();
    widget.deleteLater();
    pump();
}

// A section laid out in a column of the given width inside a scroll area, as the page lays it.
struct Column {
    std::unique_ptr<QScrollArea> scroll;
    QWidget* host = nullptr;

    Column(QWidget& section, int width, int height = 900)
        : scroll(std::make_unique<QScrollArea>()) {
        scroll->setWidgetResizable(true);
        host = new QWidget;
        auto* column = new QVBoxLayout(host);
        column->addWidget(&section);
        column->addStretch();
        scroll->setWidget(host);
        scroll->resize(width, height);
        scroll->show();
        pump(8);
    }
    void resize(int width, int height = 900) {
        scroll->resize(width, height);
        pump(8);
    }
};

} // namespace fvtest
