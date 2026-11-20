#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QTest>

#include "base/json.h"
#include "hotkeys/conflicts.h"
#include "hotkeys/testing/fake_capture.h"
#include "pyport/errors.h"
#include "settings/hotkeys.h"
#include "settings/store.h"
#include "testkit/testkit.h"
#include "ui_hotkeys/hotkey_bindings.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/hotkey_row.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/wrapped_label.h"

namespace fvtest {

using fv::Json;

// The application styled the way OpenWhisper's QtApplication styles it (ThemeManager, theme,
// font scale), for the duration of one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

// Stands in for Settings around the page (the dialog's message label, _persist and the on_*
// callbacks the OpenWhisper tests set), recording every call.
class TestHost final : public fv::HotkeysPageHost {
public:
    explicit TestHost(fv::SettingsStore& store) : store_(store) {}

    void showMessage(const QString& message) override { message_ = message; }
    void setCaptureSuspended(bool suspended) override { suspends.push_back(suspended); }
    void hotkeysChanged(const Json& hotkeys) override {
        applied.push_back(hotkeys);
        if (saveError) {
            throw std::runtime_error(*saveError);
        }
        // UIController._on_settings_hotkeys_changed without an app: it saves them.
        fv::save_hotkey_settings(store_, hotkeys);
    }
    void triggerModeChanged(const QString& mode) override { modes.push_back(mode); }
    void openDestination(const QString& destination) override {
        destinations.push_back(destination);
    }
    bool persist(std::string_view key, const Json& value) override {
        // settings_dialog._persist: the raw stored value decides whether anything is written.
        if (store_.value(key, Json("<unset>")) == value) {
            return true;
        }
        store_.save_setting(key, value);
        message_.clear();
        return true;
    }

    const QString& message() const { return message_; }

    std::vector<bool> suspends;
    std::vector<Json> applied;
    std::vector<QString> modes;
    std::vector<QString> destinations;
    std::optional<std::string> saveError;

private:
    fv::SettingsStore& store_;
    QString message_;
};

// A profile or transform source over a settings list of {"id", "name", <text>, "hotkey"}, with
// the validation their own editors use: a shortcut something else uses is refused with the
// editor's reason.
class ListSource final : public fv::BindingSource {
public:
    ListSource(std::string kind, std::string key, std::string text)
        : kind_(std::move(kind)), key_(std::move(key)), text_(std::move(text)) {}

    std::vector<fv::hotkey_bindings::Binding> list(const Json& settings) const override {
        std::vector<fv::hotkey_bindings::Binding> out;
        const auto found = settings.find(key_);
        if (found == settings.end() || !found->is_array()) {
            return out;
        }
        for (const auto& item : *found) {
            out.push_back({kind_, item.value("id", ""), item.value("name", ""),
                           item.value("hotkey", ""),
                           fv::hotkey_bindings::clip(item.value(text_, "")), true});
        }
        return out;
    }

    bool save(Json& settings, const std::string& id, const std::string& hotkey) override {
        auto found = settings.find(key_);
        if (found == settings.end() || !found->is_array()) {
            return false;
        }
        for (auto& item : *found) {
            if (item.value("id", "") != id) {
                continue;
            }
            fv::hotkeys::ConflictOptions options;
            options.exclude = {kind_, id};
            if (sources != nullptr) {
                options.bindings = fv::conflictBindings(*sources, settings);
            }
            const auto conflict = fv::hotkeys::hotkey_conflict(hotkey, settings, options);
            if (!conflict.empty()) {
                throw fv::py::ValueError("That shortcut is already used by " + conflict +
                                         ". Choose another.");
            }
            item["hotkey"] = hotkey;
            return true;
        }
        return false;
    }

    void saved() override { ++savedCount; }

    // The page's sources, for the conflict check across kinds.
    const fv::BindingSources* sources = nullptr;
    int savedCount = 0;

private:
    std::string kind_;
    std::string key_;
    std::string text_;
};

// A Hotkeys page over a fresh settings file, with fake global captures and answered prompts.
struct PageScene {
    explicit PageScene(const Json& values = Json::object()) {
        store = std::make_unique<fv::SettingsStore>(root.path() / "settings.json");
        store->save_all(values);
        host = std::make_unique<TestHost>(*store);
        page = std::make_unique<fv::HotkeysPage>(*store, *host);
        page->setCaptureFactory(captures.factory());
        page->setPrompts({[this](QWidget*) { return resetAnswer; },
                          [this](QWidget*, const QString& message) {
                              failures.push_back(message);
                          }});
    }
    ~PageScene() {
        page.reset();
        host.reset();
        store.reset();
    }

    // Registers profile and transform sources over transcript_cleanup_profiles and
    // text_transforms, then loads the page again.
    void addListSources() {
        profiles =
            std::make_shared<ListSource>("profile", "transcript_cleanup_profiles", "instructions");
        transforms = std::make_shared<ListSource>("transform", "text_transforms", "instruction");
        page->registerBindingSource("profile", profiles);
        page->registerBindingSource("transform", transforms);
        profiles->sources = &page->bindingShortcuts()->sources();
        transforms->sources = &page->bindingShortcuts()->sources();
        page->loadSettings(*store->snapshot());
    }

    Json savedHotkeys() const { return fv::load_hotkey_settings(*store); }
    std::string saved(std::string_view action) const {
        const Json hotkeys = savedHotkeys();
        const auto found = hotkeys.find(action);
        return found != hotkeys.end() && found->is_string() ? found->get<std::string>() : "";
    }

    // The note under a standard row, "" while hidden.
    std::string note(std::string_view action) const {
        const auto* label = page->rowNote(action);
        return label->isHidden() ? "" : label->text().toStdString();
    }

    fv::hotkeys::FakeCapture* capture() const { return captures.last(); }

    fv::testkit::TempDataRoot root;
    fv::hotkeys::FakeCaptureFactory captures;
    std::unique_ptr<fv::SettingsStore> store;
    std::unique_ptr<TestHost> host;
    std::unique_ptr<fv::HotkeysPage> page;
    std::shared_ptr<ListSource> profiles;
    std::shared_ptr<ListSource> transforms;
    bool resetAnswer = true;
    std::vector<QString> failures;
};

inline QString display(const std::string& hotkey) {
    return QString::fromStdString(
        fv::hotkeys::format_hotkey_display(hotkey, fv::hotkeys::platform_vocabulary()));
}

inline void clickSideButton(QWidget* widget, Qt::MouseButton button = Qt::BackButton,
                            Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::mouseClick(widget, button, modifiers, QPoint(8, 8));
}

// The card a field sits on.
inline fv::ReflowCard* cardOf(QWidget* widget) {
    while (widget != nullptr && qobject_cast<fv::ReflowCard*>(widget) == nullptr) {
        widget = widget->parentWidget();
    }
    return qobject_cast<fv::ReflowCard*>(widget);
}

// The Hotkeys page's field for the profile, transform or action called `name`.
inline fv::ProfileHotkeyInput* bindingField(fv::HotkeysPage& page, const QString& name) {
    fv::ProfileHotkeyInput* match = nullptr;
    int count = 0;
    for (auto* field : page.bindingShortcuts()->findChildren<fv::ProfileHotkeyInput*>()) {
        if (field->accessibleName() == name + QStringLiteral(" shortcut")) {
            match = field;
            ++count;
        }
    }
    return count == 1 ? match : nullptr;
}

inline std::string bindingNote(QWidget* field) {
    auto* note = cardOf(field)->findChild<QLabel*>(QStringLiteral("hotkeyShortcutNote"));
    return note->isHidden() ? "" : note->text().toStdString();
}

inline QPushButton* bindingClear(QWidget* field) {
    return cardOf(field)->findChild<QPushButton*>(QStringLiteral("hotkeyClearButton"));
}

} // namespace fvtest
