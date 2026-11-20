#pragma once

// Apps & styles, Commands and Profiles inside a real Settings window over fakes (OpenWhisper's
// tests/fakes/personalize.settings_dialogs, as pages_support.h builds it), plus what those pages
// reach outside the window:
// - the Command Mode readiness check (text_rewrite.provider_readiness, which the Python tests
//   monkeypatch): `ready`, answered on an executor that runs each check at once, the answer
//   arriving with the next turn of the event loop (answers());
// - the Hotkeys page's save path for one standard shortcut, as set_standard_hotkey does it
//   (profile_hotkey_conflict over the window's shortcuts, then save_hotkey_settings and
//   "<label> hotkey updated."), because the Hotkeys page here is a stand-in;
// - the window's capture pauses, recorded in `captures` (the on_profile_hotkey_capture mock);
// - the platform answers of Apps & styles (text_reading_supported pinned true, no recent apps);
// - the editors' questions: recorded in `asked` as "title: text" and answered `answer`
//   (Cancel unless a test says otherwise), never a real message box.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QMessageBox>
#include <QStringList>

#include "base/executor.h"
#include "hotkeys/grammar.h"
#include "pages_support.h"
#include "settings/hotkeys.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_personalize/command_shortcut_field.h"
#include "ui_personalize/commands_page.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/profiles_page.h"
#include "ui_personalize/styles_page.h"
#include "ui_personalize/transforms_panel.h"

namespace fvtest {

// Runs every task the moment it is posted, on the posting thread.
class InlineExecutor final : public fv::Executor {
public:
    void post(fv::Task task) override { task(); }
    fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

inline QString display(std::string_view hotkey) {
    return qs(fv::hotkeys::format_hotkey_display(hotkey, fv::hotkeys::platform_vocabulary()));
}

class PersonalizeRig : public PagesRig {
public:
    explicit PersonalizeRig(const fv::Json& saved = fv::Json::object()) : PagesRig(saved) {
        fv::StylesPage::setPlatform({[] { return std::vector<fv::focus::AppIdentity>{}; },
                                     [] {
                                         return true;
                                     }});
        context().service<fv::CommandReadiness>().setCheck(
            [this](const fv::Json&) { return ready; }, &worker_);
    }
    ~PersonalizeRig() { fv::StylesPage::setPlatform({}); }
    PersonalizeRig(const PersonalizeRig&) = delete;
    PersonalizeRig& operator=(const PersonalizeRig&) = delete;

    // The window, with the standard-shortcut path and the capture recorder installed.
    fv::SettingsDialog& window() {
        fv::SettingsDialog& made = dialog();
        if (wired_ != &made) {
            wired_ = &made;
            made.setStandardHotkeyHandler(
                [this, &made](const QString& action, const QString& hotkey) {
                    return setStandardHotkey(made, fv::qt::toUtf8(action), fv::qt::toUtf8(hotkey));
                });
            QObject::connect(&made, &fv::SettingsDialog::hotkeyCaptureSuspended,
                             [this](bool suspended) { captures.push_back(suspended); });
        }
        return made;
    }
    // shownDialog() over window().
    fv::SettingsDialog& shownWindow() {
        window();
        return shownDialog();
    }

    fv::StylesPage& styles() {
        window();
        return page<fv::StylesPage>(dest::styles);
    }
    fv::CommandsPage& commands() {
        window();
        fv::CommandsPage& made = page<fv::CommandsPage>(dest::commands);
        guard(*made.transformsPanel());
        answers();
        return made;
    }
    fv::TransformsPanel& transforms() { return *commands().transformsPanel(); }
    fv::ProfilesPage& profiles() {
        window();
        fv::ProfilesPage& made = page<fv::ProfilesPage>(dest::cleanup_profiles);
        guard(*made.panel());
        return made;
    }
    fv::CleanupProfilesPanel& profilesPanel() { return *profiles().panel(); }
    // The snippet editor with the same questions as the others.
    fv::SnippetsPanel& snippetEditor() {
        window();
        fv::SnippetsPanel& made = *page<fv::SnippetsPage>(dest::snippets).panel();
        guard(made);
        return made;
    }

    template <class Panel> void guard(Panel& panel) {
        panel.question = [this](QWidget*, const QString& title, const QString& text,
                                QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            asked << title + QStringLiteral(": ") + text;
            return answer;
        };
    }

    // Lets readiness answers (and anything else queued) arrive.
    static void answers() {
        for (int i = 0; i < 3; ++i) {
            QApplication::processEvents();
        }
    }

    // The saved standard shortcuts.
    fv::Json savedHotkeys() { return fv::load_hotkey_settings(settings()); }
    std::vector<fv::text::Transform> savedTransforms() {
        return fv::text::load_transforms(*settings().snapshot());
    }
    std::vector<fv::text::CleanupProfile> savedProfiles() {
        return fv::text::load_cleanup_profiles(*settings().snapshot());
    }

    std::optional<bool> ready = true;
    std::vector<bool> captures;
    QStringList asked;
    QMessageBox::StandardButton answer = QMessageBox::Cancel;

private:
    // settings_dialog.py set_standard_hotkey.
    QString setStandardHotkey(fv::SettingsDialog& made, const std::string& action,
                              const std::string& hotkey) {
        fv::Json current = fv::default_hotkeys();
        const fv::Json saved = fv::load_hotkey_settings(settings());
        if (saved.is_object()) {
            for (const auto& [key, value] : saved.items()) {
                current[key] = value;
            }
        }
        fv::Json updated = current;
        updated[action] = hotkey;
        fv::text::ShortcutContext shortcuts;
        shortcuts.standard_hotkeys = updated;
        (*shortcuts.standard_hotkeys)[action] = "";
        const std::string conflict =
            fv::text::profile_hotkey_conflict(hotkey, *settings().snapshot(), {}, shortcuts);
        if (!conflict.empty()) {
            const QString error =
                QStringLiteral("That shortcut is already used by %1.").arg(qs(conflict));
            made.showMessage(error);
            return error;
        }
        fv::save_hotkey_settings(settings(), updated);
        made.showMessage(qs(fv::standard_hotkeys::label(action)) +
                         QStringLiteral(" hotkey updated."));
        made.refreshRailValues();
        return {};
    }

    InlineExecutor worker_;
    fv::SettingsDialog* wired_ = nullptr;
};

} // namespace fvtest
