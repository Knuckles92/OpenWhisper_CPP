#include "ui_overlay/language_menu.h"

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QAction>
#include <QMenu>
#include <QStringList>

#include "base/json.h"
#include "base/log.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "ui_overlay/waveform_overlay.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.language_menu"};

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

QStringList toList(const std::vector<std::string>& codes) {
    QStringList list;
    for (const std::string& code : codes) {
        list.append(fromStd(code));
    }
    return list;
}

void switchLanguage(LanguageMenuUi& ui, const std::function<void(Json&)>& change, bool notice) {
    QStringList choices;
    QString code;
    QString reason;
    QString name;
    try {
        SettingsStore& settings = ui.settings();
        settings.mutate(change);
        const std::shared_ptr<const Json> saved = settings.snapshot();
        choices = toList(speech::language_choices(*saved));
        const std::string current = speech::current_language(*saved);
        code = fromStd(current);
        reason = fromStd(speech::single_language_reason(*saved));
        name = fromStd(speech::label(current));
    } catch (const std::exception& error) {
        kLog.error_with(error, kLanguageSwitchFailed);
        ui.setStatus(QString::fromUtf8(kLanguageSwitchFailed.data(),
                                       static_cast<qsizetype>(kLanguageSwitchFailed.size())));
        return;
    }
    if (choices.size() < 2) {
        ui.setStatus(
            !reason.isEmpty()
                ? reason
                : QStringLiteral(u"Add another language in Settings → Voice model to switch"));
        return;
    }
    WaveformOverlay& overlay = ui.overlay();
    overlay.setLanguage(code, choices);
    // With nothing recording the overlay is hidden; a short notice near the pointer confirms a
    // shortcut press that would otherwise show nothing.
    if (notice &&
        (!overlay.isVisible() || overlay.currentState() == WaveformOverlay::kStateLanguage)) {
        overlay.showLanguageNotice();
    }
    ui.setStatus(QStringLiteral("Dictation language: %1").arg(name));
}

} // namespace

void populateLanguageMenu(QMenu* menu, LanguageMenuUi& ui) {
    menu->clear();
    std::vector<std::string> choices;
    std::string current;
    std::string reason;
    try {
        const std::shared_ptr<const Json> saved = ui.settings().snapshot();
        choices = speech::language_choices(*saved);
        current = speech::current_language(*saved);
        reason = speech::single_language_reason(*saved);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't list the dictation languages");
        choices.clear();
        current.clear();
        reason.clear();
    }
    if (choices.size() > 1) {
        for (const std::string& code : choices) {
            QAction* action = menu->addAction(fromStd(speech::label(code)));
            action->setCheckable(true);
            action->setChecked(code == current);
            const QString chosen = fromStd(code);
            QObject::connect(action, &QAction::triggered, menu,
                             [&ui, chosen] { selectLanguage(ui, chosen); });
        }
    } else if (!current.empty() && reason.empty()) {
        // Nothing to switch to, but still the language the next dictation uses.
        QAction* action = menu->addAction(fromStd(speech::label(current)));
        action->setCheckable(true);
        action->setChecked(true);
        action->setEnabled(false);
    } else {
        menu->addAction(!reason.empty() ? fromStd(reason) : QStringLiteral("No other languages"))
            ->setEnabled(false);
    }
    menu->addSeparator();
    QObject::connect(menu->addAction(QStringLiteral(u"Choose languages…")), &QAction::triggered,
                     menu, [&ui] { ui.openVoiceModelSettings(); });
}

void selectLanguage(LanguageMenuUi& ui, const QString& code) {
    const std::string chosen = code.toStdString();
    switchLanguage(ui, [&chosen](Json& settings) { speech::set_active(settings, chosen); }, false);
}

void cycleLanguage(LanguageMenuUi& ui) {
    switchLanguage(ui, [](Json& settings) { speech::cycle(settings); }, true);
}

} // namespace fv
