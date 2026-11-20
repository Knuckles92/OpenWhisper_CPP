#pragma once

#include <vector>

#include <QSize>
#include <QString>
#include <QWidget>

class QBoxLayout;
class QPushButton;

namespace fv {

class Button;
class PersonalizeHost;
class ProfileHotkeyInput;
class WrappedLabel;

// The Command Mode shortcut field (ui_qt/widgets/command_settings.py CommandShortcutField, tile
// id "commandShortcutField"), on Settings > Commands and on Home: a capture box saved through the
// Settings window's standard-shortcut path (setStandardHotkey("command_mode", ...)), so the
// Hotkeys page keeps it and the app registers it like any other, with an optional "Change"
// (Home) and "Clear". A refused shortcut shows why under the box when `inlineErrors` is set
// (Commands); on Home the window's message line says it. Capturing pauses the app's hotkeys.
//
// Settings pages ignore minimum widths, so a narrow tile would draw the buttons over the box: the
// buttons go under the box when the field is narrower than the box's 160 px floor (at the font
// scale) beside them. Its size hints are the same whichever way it is arranged, so a layout
// never flips it back and forth.
// Affinity: GUI thread.
class CommandShortcutField : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kAction = "command_mode";
    // The box's narrowest width beside its buttons at 100 %: room for the whole "Click to set
    // hotkey" placeholder.
    static constexpr int kInputMinWidth = 160;
    static constexpr int kInputMaxWidth = 240;

    explicit CommandShortcutField(PersonalizeHost& host, bool changeButton = false,
                                  bool clearButton = true, bool inlineErrors = true,
                                  QWidget* parent = nullptr);

    ProfileHotkeyInput* input() const { return input_; }
    // Null when the field was built without them.
    QPushButton* changeButton() const { return change_; }
    Button* clearButton() const { return clear_; }
    WrappedLabel* message() const { return message_; }
    QBoxLayout* row() const { return row_; }

    // The saved shortcut: the window's current standard shortcuts, else the saved ones.
    QString hotkey() const;
    // Shows the saved shortcut (unless a capture is under way) and enables Clear only with one.
    void sync();
    void beginCapture();
    void cancelCapture();
    void showError(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void save(const QString& hotkey);
    std::vector<QPushButton*> buttons() const;
    int sideBySideWidth() const;

    PersonalizeHost& host_;
    bool inlineErrors_;
    QBoxLayout* row_;
    ProfileHotkeyInput* input_;
    QPushButton* change_ = nullptr;
    Button* clear_ = nullptr;
    WrappedLabel* message_;
};

} // namespace fv
