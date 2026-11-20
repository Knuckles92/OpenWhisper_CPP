#pragma once

#include <QWidget>

class QPushButton;

namespace fv {

class Button;
class HotkeyHintFilter;

// The main window's footer (object name "footerBar"): Minimize to Tray, Stats, Settings and Quit,
// centred, 10 px apart. MainWindow connects the buttons.
//
// No Q_OBJECT, on purpose: Qt's style sheets paint a background rule only on widgets whose
// meta-object is QWidget's own (QStyleSheetStyle::polish), and the theme gives this bar its
// background and top border that way, as it gives OpenWhisper's plain QWidget footer.
// Affinity: GUI thread.
class FooterBar : public QWidget {
public:
    static constexpr int kHeight = 48;
    static constexpr int kButtonHeight = 34;
    static constexpr int kGap = 10;
    // The Quit button's hover hint: the window's own shortcut for Exit.
    static constexpr char kQuitShortcut[] = "Ctrl+Q";

    explicit FooterBar(QWidget* parent = nullptr);

    Button* trayButton() const { return trayButton_; }
    QPushButton* statsButton() const { return statsButton_; }
    QPushButton* settingsButton() const { return settingsButton_; }
    QPushButton* quitButton() const { return quitButton_; }

    // Shown only where a system tray exists: without one, the button is hidden, disabled and
    // says why (MainWindow.set_tray_available).
    void setTrayAvailable(bool available);
    // The tray button's hover hint (the minimize hotkey's display text); empty hides it.
    void setTrayHotkey(const QString& hotkey);

private:
    Button* trayButton_;
    QPushButton* statsButton_;
    QPushButton* settingsButton_;
    QPushButton* quitButton_;
    HotkeyHintFilter* quitHint_;
};

} // namespace fv
