#pragma once

#include <string_view>

#include <QString>

class QMenu;

namespace fv {

class SettingsStore;
class WaveformOverlay;

// What the dictation language menu needs of the app (ui_qt/widgets/language_menu.py's `ui`):
// the overlay that shows the language, the status line, the Voice model page, and the settings
// it reads and saves. settings() may throw when the settings cannot be had; the menu then
// reports the failure as it reports a broken language list.
// Affinity: GUI thread.
class LanguageMenuUi {
public:
    virtual ~LanguageMenuUi() = default;

    virtual WaveformOverlay& overlay() = 0;
    virtual void setStatus(const QString& text) = 0;
    // Settings at the Voice model page (settings_destinations.VOICE_MODEL).
    virtual void openVoiceModelSettings() = 0;
    virtual SettingsStore& settings() = 0;
};

// The dictation language menu shared by the tray and the desktop bar, and the one place a
// language switch is announced, so the tray, the shortcut and the overlay chip say the same.
// Affinity: GUI thread. ui must outlive the menu's actions.

// The status and log line when the switch fails.
inline constexpr std::string_view kLanguageSwitchFailed = "Couldn't switch the dictation language";

// Refills menu with the languages dictated in, just before it opens: each choice checkable with
// the active one checked; with one language, the language in effect as a checked disabled row,
// or why the engine takes only one, or "No other languages"; then "Choose languages…", always
// last. A language list that cannot be read still leaves that menu.
void populateLanguageMenu(QMenu* menu, LanguageMenuUi& ui);
// Makes code the language the next dictation uses (the tray's choice); no notice.
void selectLanguage(LanguageMenuUi& ui, const QString& code);
// Switches to the next language dictated in (the shortcut, the chip and the bar); a hidden
// overlay, or one already showing the language notice, shows the notice near the pointer.
void cycleLanguage(LanguageMenuUi& ui);

} // namespace fv
