#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMetaObject>
#include <QPoint>
#include <QString>
#include <QWidget>

#include "base/executor.h"
#include "base/subscription.h"
#include "dictation/scratchpad_port.h"
#include "focus/snapshot.h"

class QFrame;
class QHBoxLayout;
class QMenu;
class QPlainTextEdit;
class QSizeGrip;
class QTimer;
class QToolButton;

namespace fv {

class ElidingLabel;
class MainWindow;
class ScratchpadSaver;
class SettingsStore;
class SystemTray;

namespace dictation {
class HotkeyRuntime;
}

// What the Scratchpad reads and writes outside itself (OpenWhisper reaches the same through its
// settings manager and config.SCRATCHPAD_FILE).
struct ScratchpadEnvironment {
    // scratchpad_always_on_top and scratchpad_geometry. Required.
    SettingsStore* settings = nullptr;
    // The strand the autosave writes on (the context's persistence executor). Required; it
    // outlives the window and the writes queued on it.
    Executor* persistence = nullptr;
    // Where the notes live (paths.user_file("scratchpad.txt")), asked at every read and write.
    std::function<std::filesystem::path()> file;
};

// The floating Scratchpad: a notepad that dictation types into while it has focus
// (ui_qt/widgets/scratchpad.py ScratchpadWindow). A frameless, rounded tool window with its own
// drag header and resize grip; the compositor-managed window of Wayland and Omarchy is the
// other platforms' (OpenWhisper's `compositor` branch). Its text autosaves 700 ms after an
// edit to the notes file and is never written before it was loaded. Never log what it holds:
// counts and lengths only.
// Affinity: GUI thread.
class ScratchpadWindow : public QWidget {
    Q_OBJECT
    // The notes, as the Python window's text() gives them: a tree dump reads it as the text.
    Q_PROPERTY(QString text READ text STORED false)
public:
    // AUTOSAVE_MS, GEOMETRY_SAVE_MS, NOTICE_MS, UNDO_CLEAR_MS.
    static constexpr int kAutosaveMs = 700;
    static constexpr int kGeometrySaveMs = 500;
    static constexpr int kNoticeMs = 2600;
    static constexpr int kUndoClearMs = 8000;

    // Puts text on the user's clipboard; false when it could not.
    using CopyText = std::function<bool(const QString&)>;
    // Fills the Transform menu each time it opens. The default adds the disabled "No
    // transforms yet"; the transforms arrive with the AI cleanup feature.
    using TransformMenuFiller = std::function<void(QMenu*, ScratchpadWindow&)>;

    // OpenWhisper's window takes only copy_text because its settings and notes file are
    // globals; here they come in with the environment.
    ScratchpadWindow(ScratchpadEnvironment environment, CopyText copyText,
                     QWidget* parent = nullptr);
    ~ScratchpadWindow() override;

    // Shows, raises and focuses the Scratchpad, loading its text the first time.
    void present();
    // Shows and raises it without taking focus from the user's app: for an answer that arrives
    // later, while the user may be typing elsewhere.
    void reveal();
    // Reads the saved notes and the window's place the first time; before any edit.
    void ensureLoaded();
    // Writes pending edits now (hiding, quitting).
    void flush();

    QString text() const;
    // Types a finished dictation at the cursor, spaced to fit the text around it, replacing a
    // selection.
    void insertDictation(const QString& text);
    // Adds text after everything else, separated by a blank line; the cursor stays.
    void appendBlock(const QString& text);
    void copyAll();
    // Empties the notes and keeps them for kUndoClearMs behind "Undo clear".
    void clearText();
    void undoClear();
    // Keeps the window above others (saved either way). Changing the window flags recreates
    // the native window, which hides it, so a visible window is shown again.
    void setAlwaysOnTop(bool on);

    // How a dictation is joined to the text around the cursor; focus::join_with_context unless
    // set (a test sees the context through it). A joiner that throws leaves the dictation as
    // spoken.
    using Joiner = std::function<std::string(std::string_view, const focus::TextContext&)>;
    void setDictationJoiner(Joiner joiner) { joiner_ = std::move(joiner); }

    // M4 sets the menu's contents; without one the menu says "No transforms yet".
    void setTransformMenuFiller(TransformMenuFiller filler);
    // Empties the menu and has the filler fill it. A filler that throws is logged and leaves
    // the disabled "No transforms yet".
    void fillTransformMenu();

    // Visible and the active window: where a dictation belongs. activityProbe replaces
    // isActiveWindow (a test window cannot take the desktop's focus).
    bool isActive() const;
    void setActivityProbe(std::function<bool()> probe) { activityProbe_ = std::move(probe); }

    // Whether the notes were loaded from the file (or found to have none).
    bool isLoaded() const { return loaded_; }
    // The cleared notes while "Undo clear" is offered.
    const std::optional<QString>& clearedText() const { return clearedText_; }

    QPlainTextEdit* editor() const { return editor_; }
    QFrame* surface() const { return surface_; }
    QFrame* header() const { return header_; }
    QToolButton* onTopButton() const { return onTopButton_; }
    QToolButton* closeButton() const { return closeButton_; }
    ElidingLabel* statusLabel() const { return statusLabel_; }
    QToolButton* transformButton() const { return transformButton_; }
    QMenu* transformMenu() const { return transformMenu_; }
    QToolButton* copyButton() const { return copyButton_; }
    QToolButton* clearButton() const { return clearButton_; }
    QSizeGrip* sizeGrip() const { return sizeGrip_; }
    // The four single-shot timers, for tests to expire without waiting.
    QTimer* autosaveTimer() const { return autosaveTimer_; }
    QTimer* geometryTimer() const { return geometryTimer_; }
    QTimer* noticeTimer() const { return noticeTimer_; }
    QTimer* undoTimer() const { return undoTimer_; }
    ScratchpadSaver& saver() const { return *saver_; }

protected:
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    // Accepted: QApplication::quit stops closing windows at the first refusal and then cancels
    // the quit. Without WA_DeleteOnClose an accepted close only hides, and hideEvent saves the
    // notes.
    void closeEvent(QCloseEvent* event) override;

private:
    class Header;

    void applyWindowFlags(bool onTop);
    void build();
    QToolButton* makeAction(const QString& text, const QString& icon);
    QRect defaultRect(const QRect& available) const;
    void restoreWindowPlace();
    void saveWindowPlace();
    void load();
    void onTextChanged();
    void saveSoon();
    void showWordCount();
    void notice(const QString& text, int ms = kNoticeMs);
    void clearOrUndo();
    void forgetCleared();
    void openTransformMenu();

    ScratchpadEnvironment environment_;
    CopyText copyText_;
    TransformMenuFiller transformMenuFiller_;
    Joiner joiner_;
    std::function<bool()> activityProbe_;
    std::unique_ptr<ScratchpadSaver> saver_;
    bool loaded_ = false;
    bool suppressSave_ = false;
    std::optional<QString> clearedText_;

    QFrame* surface_ = nullptr;
    QFrame* header_ = nullptr;
    QToolButton* onTopButton_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QHBoxLayout* footerRow_ = nullptr;
    ElidingLabel* statusLabel_ = nullptr;
    QToolButton* transformButton_ = nullptr;
    QMenu* transformMenu_ = nullptr;
    QToolButton* copyButton_ = nullptr;
    QToolButton* clearButton_ = nullptr;
    QSizeGrip* sizeGrip_ = nullptr;
    QTimer* autosaveTimer_ = nullptr;
    QTimer* geometryTimer_ = nullptr;
    QTimer* noticeTimer_ = nullptr;
    QTimer* undoTimer_ = nullptr;
};

// What dictation, the hotkey, the tray and the View menu reach the Scratchpad through (Python
// keeps the window on the UI controller as `ui._scratchpad`): it builds the window on first
// use, keeps it, and is the dictation runtime's ScratchpadPort. Provided to the context by
// RecordFeature. Affinity: GUI thread.
class ScratchpadService final : public dictation::ScratchpadPort {
public:
    // `copyText` is the application's clipboard copy (UIController.copy_to_clipboard).
    ScratchpadService(ScratchpadEnvironment environment, ScratchpadWindow::CopyText copyText);
    // Writes pending notes and deletes the window.
    ~ScratchpadService() override;
    ScratchpadService(const ScratchpadService&) = delete;
    ScratchpadService& operator=(const ScratchpadService&) = delete;

    // The window, once something has used it; null before.
    ScratchpadWindow* window() const { return window_.get(); }
    // The window, built when it does not exist yet.
    ScratchpadWindow& ensureWindow();

    // Sets how a new window fills its Transform menu (applies to the window now and later).
    void setTransformMenuFiller(ScratchpadWindow::TransformMenuFiller filler);

    // ScratchpadPort: visible and active; takes the text as a dictation.
    bool is_focused() const override;
    bool insert(std::string text) override;

    // Run first when the service goes: whoever was handed the port lets go of it.
    void setReleaseHook(std::function<void()> hook) { releaseHook_ = std::move(hook); }

    // What shows or hides the Scratchpad (toggleScratchpad), each connected for the life of the
    // service: the View menu's item, the tray's item and the Scratchpad hotkey (its signal is
    // delivered on `ui`). RecordFeature connects the hotkey; the composition connects the window
    // and the tray. Connecting a source again does nothing, so a press never toggles twice
    // whoever else calls these.
    void connectWindow(MainWindow& window);
    void connectTray(SystemTray& tray);
    void connectHotkey(dictation::HotkeyRuntime& hotkeys, Executor& ui);

private:
    // Records `source` as connected; false when it already was.
    bool firstConnection(const void* source);

    ScratchpadEnvironment environment_;
    ScratchpadWindow::CopyText copyText_;
    ScratchpadWindow::TransformMenuFiller filler_;
    std::function<void()> releaseHook_;
    std::vector<const void*> sources_;
    std::vector<QMetaObject::Connection> connections_;
    std::vector<Subscription> links_;
    std::unique_ptr<ScratchpadWindow> window_;
};

// scratchpad.toggle: shows and focuses the Scratchpad, or hides it when it is visible and the
// active window (the hotkey, the tray and the View menu).
void toggleScratchpad(ScratchpadService& service);
// scratchpad.show_text: shows the Scratchpad with text added at the end, on a line of its own.
// For answers that arrive later (an agent action's reply), not dictations: the user's cursor,
// selection and focus stay where they were. The saved notes load first, so loading them cannot
// replace the answer. Empty text does nothing.
void showScratchpadText(ScratchpadService& service, const QString& text);
// scratchpad.insert: types a finished dictation into the focused Scratchpad. True only when the
// Scratchpad took the text, so the caller skips the paste; false whenever the text should go to
// the app as usual (no window, empty text, hidden or not active).
bool insertIntoScratchpad(ScratchpadService& service, const QString& text);

} // namespace fv
