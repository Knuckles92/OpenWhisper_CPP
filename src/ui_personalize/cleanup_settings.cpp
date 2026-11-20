#include "ui_personalize/cleanup_settings.h"

#include <algorithm>
#include <exception>
#include <string_view>
#include <typeinfo>
#include <utility>

#include <QListWidget>
#include <QWidget>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_prompts.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/cleanup_page.h"
#include "ui_personalize/cleanup_rules_section.h"
#include "ui_personalize/rules_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_widgets/settings_switch.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

template <class T> void prune(std::vector<QPointer<T>>& items) {
    std::erase_if(items, [](const QPointer<T>& item) { return item.isNull(); });
}

} // namespace

SettingsDialog* settingsDialogOf(QWidget* widget) {
    for (QWidget* at = widget; at != nullptr; at = at->parentWidget()) {
        if (auto* dialog = qobject_cast<SettingsDialog*>(at)) {
            return dialog;
        }
    }
    return nullptr;
}

CleanupSettings::CleanupSettings(AppContext& context, CleanupGates& gates, QObject* parent)
    : QObject(parent), context_(context), gates_(gates) {
    question = [](QWidget* dialogParent, const QString& title, const QString& text,
                  QMessageBox::StandardButtons buttons, QMessageBox::StandardButton fallback) {
        return QMessageBox::question(dialogParent, title, text, buttons, fallback);
    };
    // The gates turn cleanup on as this page's switch does.
    gates_.setTurnOnHandlerWithOrigin([this](QWidget* origin) { turnOnCleanup(origin); });
}

CleanupSettings::~CleanupSettings() {
    gates_.setTurnOnHandlerWithOrigin({});
}

SettingsDialog* CleanupSettings::windowFor(QWidget* origin) const {
    if (SettingsDialog* dialog = settingsDialogOf(origin)) {
        return dialog;
    }
    if (origin == nullptr) {
        if (const SettingsWindow* window = context_.find<SettingsWindow>()) {
            return window->dialog();
        }
    }
    return nullptr;
}

Json CleanupSettings::snapshot(SettingsDialog* dialog) const {
    const Json settings =
        dialog != nullptr ? *dialog->settingsSnapshot() : *context_.settings().snapshot();
    return settings.is_object() ? settings : Json::object();
}

bool CleanupSettings::persist(SettingsDialog* dialog, const Json& updates,
                              std::vector<std::string> drops) {
    if (dialog != nullptr) {
        return dialog->persistMany(updates, std::move(drops));
    }
    try {
        const std::vector<std::string_view> removed(drops.begin(), drops.end());
        context_.settings().update(updates, removed);
        return true;
    } catch (const std::exception& error) {
        kLog.warning("Couldn't save the AI cleanup settings ({})",
                     type_display_name(typeid(error)));
        return false;
    }
}

bool CleanupSettings::confirmCustomPromptRemoval(const QString& level, QWidget* parent) {
    const auto parsed = text::parse_cleanup_level(qt::toUtf8(level));
    const QString name =
        qt::toQString(text::level_name(parsed.value_or(text::CleanupLevel::Medium)));
    const QMessageBox::StandardButton reply =
        question ? question(parent, QStringLiteral("Remove your custom prompt?"),
                            QStringLiteral("AI cleanup will go back to the %1 level's "
                                           "instructions. Your learned rules stay.")
                                .arg(name),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                 : QMessageBox::No;
    return reply == QMessageBox::Yes;
}

bool CleanupSettings::chooseCleanupLevel(const QString& level, QWidget* origin) {
    SettingsDialog* dialog = windowFor(origin);
    std::vector<std::string> drops;
    if (!text::custom_prompt(snapshot(dialog)).empty()) {
        QWidget* parent = origin != nullptr ? origin->window() : dialog;
        if (!confirmCustomPromptRemoval(level, parent)) {
            prune(pages_);
            for (const QPointer<CleanupPage>& page : pages_) {
                page->updateCleanupLevelUi();
            }
            return false;
        }
        drops.emplace_back(keys::transcript_cleanup_prompt.key());
    }
    Json updates = Json::object();
    updates[std::string(keys::transcript_cleanup_level.key())] = qt::toUtf8(level);
    const bool saved = persist(dialog, updates, drops);
    if (saved && !drops.empty()) {
        forgetCustomPrompt();
    }
    updateCleanupUi(origin);
    if (saved) {
        announce(dialog);
    }
    return saved;
}

void CleanupSettings::setCleanupEnabled(bool enabled, QWidget* origin) {
    SettingsDialog* dialog = windowFor(origin);
    bool saved = false;
    if (dialog != nullptr) {
        saved = dialog->persist(keys::transcript_cleanup_enabled.key(), enabled);
    } else {
        Json updates = Json::object();
        updates[std::string(keys::transcript_cleanup_enabled.key())] = enabled;
        saved = persist(nullptr, updates, {});
    }
    // After saving: without the switch built, the saved value is the state.
    updateCleanupUi(origin);
    if (saved) {
        announce(dialog);
    }
}

void CleanupSettings::announce(SettingsDialog* dialog) {
    // The window says "cleanup" changed too, as the gates do when no page handles them.
    if (dialog != nullptr) {
        dialog->notifyChanged(QStringLiteral("cleanup"));
    }
    Q_EMIT cleanupChanged();
}

void CleanupSettings::turnOnCleanup(QWidget* origin) {
    if (SettingsDialog* dialog = windowFor(origin)) {
        if (auto* page = dynamic_cast<CleanupPage*>(dialog->pageContent(destinations::cleanup))) {
            page->cleanupCheck()->setChecked(true);
            return;
        }
    }
    setCleanupEnabled(true, origin);
}

void CleanupSettings::refreshCleanupGates() {
    gates_.refresh(snapshot(windowFor(nullptr)));
}

InfoTile* CleanupSettings::cleanupGateTile(QString description, std::function<bool()> needed,
                                           QWidget* parent) {
    return gates_.gateTile(description, std::move(needed), parent);
}

void CleanupSettings::focusCleanupModel(QWidget* origin) {
    if (SettingsDialog* dialog = windowFor(origin)) {
        dialog->focusCleanupModel();
    }
}

void CleanupSettings::forgetCustomPrompt() {
    prune(pages_);
    for (const QPointer<CleanupPage>& page : pages_) {
        page->forgetCustomPrompt();
    }
}

void CleanupSettings::updateCleanupUi(QWidget* origin) {
    prune(pages_);
    prune(rulesPages_);
    std::vector<SettingsDialog*> windows;
    const auto addWindow = [&windows](SettingsDialog* dialog) {
        if (dialog != nullptr && std::ranges::find(windows, dialog) == windows.end()) {
            windows.push_back(dialog);
        }
    };
    SettingsDialog* own = windowFor(origin);
    addWindow(own);
    for (const QPointer<CleanupPage>& page : pages_) {
        page->updateCleanupPromptUi();
        addWindow(settingsDialogOf(page.data()));
    }
    for (const QPointer<RulesPage>& page : rulesPages_) {
        addWindow(settingsDialogOf(page.data()));
    }
    gates_.refresh(snapshot(own));
    for (SettingsDialog* dialog : windows) {
        // Styles lock while cleanup is off, also when it changes on their page.
        if (dialog->isBuilt(destinations::styles)) {
            dialog->refreshPage(destinations::styles);
        }
    }
    for (const QPointer<RulesPage>& page : rulesPages_) {
        page->updateCleanupUi();
    }
}

void CleanupSettings::addPage(CleanupPage* page) {
    prune(pages_);
    pages_.emplace_back(page);
}

void CleanupSettings::addRulesPage(RulesPage* page) {
    prune(rulesPages_);
    rulesPages_.emplace_back(page);
}

void CleanupSettings::attach(SettingsDialog& dialog) {
    prune(attached_);
    if (std::ranges::any_of(attached_, [&dialog](const QPointer<SettingsDialog>& known) {
            return known.data() == &dialog;
        })) {
        return;
    }
    attached_.emplace_back(&dialog);
    connect(&dialog, &SettingsDialog::settingsChanged, this, [this](const QString& kind) {
        if (kind == QStringLiteral("cleanup_profiles")) {
            Q_EMIT cleanupProfilesChanged();
        }
    });
    connect(&dialog, &SettingsDialog::hotkeyCaptureSuspended, this,
            [this](bool capturing) { Q_EMIT profileHotkeyCapture(capturing); });
}

std::size_t CleanupSettings::ruleCount(const Json& settings) const {
    for (auto it = rulesPages_.rbegin(); it != rulesPages_.rend(); ++it) {
        if (!it->isNull()) {
            return static_cast<std::size_t>((*it)->section()->rulesList()->count());
        }
    }
    return text::resolve_transcript_cleanup_rules(settings).size();
}

} // namespace fv
