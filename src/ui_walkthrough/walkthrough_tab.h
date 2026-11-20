#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QDate>
#include <QFrame>
#include <QMessageBox>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QWidget>

#include "ui_shell/page_fit.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "walkthrough/styles.h"

class QAction;
class QActionGroup;
class QDialog;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace fv {

class ElidingLabel;
class Executor;
class FeatureBadge;
class NoWheelComboBox;
class SettingsStore;
class SettingsSwitch;
class Stage;
class StudioSplit;
class SuccessButton;
class WarningButton;

namespace ui_snap {
class CaptureDelay;
}

// The Walkthrough tab (ui_qt/widgets/walkthrough_tab.py): its settings down the left, the
// walkthrough on the right.
//
// The left column is one form read top to bottom: Start and the two shortcuts, then what happens
// while recording (Auto Mode, the capture delay, Minimize to tray), who writes it up and how (the
// AI write-up, its agent and style, whether it follows spoken notes, and the prompt itself in a
// popover), and what is copied when it finishes. The right side is a stage with one slot: the
// storyboard of the newest walkthrough or the one picked in the side panel, a Recording card while
// one records, or, with none open, how a walkthrough works and the three newest to open. Changing
// a setting never moves the stage, and the stage changing never moves the settings or resizes the
// window. In a window too narrow for both, the column goes above the stage.
namespace walkthrough_tab {

// The style menu's item while the prompt is the user's own words.
inline constexpr std::string_view OWN_PROMPT = "Your own";
// The style menu's last item, which makes a style from scratch.
inline constexpr std::string_view NEW_STYLE = "New style…";
// How long typing in the prompt pauses before it is saved, in ms.
inline constexpr int PROMPT_SAVE_DELAY_MS = 400;

// A copy choice: its walkthrough_copy_format value and what the menu calls it.
struct CopyChoice {
    std::string_view value;
    std::string_view text;
};
// In the order the menu lists them.
inline constexpr std::array<CopyChoice, 5> COPY_CHOICES{{
    {"pdf", "PDF"},
    {"agent_task", "Agent"},
    {"both", "PDF + Markdown"},
    {"agent", "Markdown"},
    {"word", "Word"},
}};

// Sizes at 100% type; each grows with the font scale.
// The values down the column's right edge, so they line up as one form.
inline constexpr int FIELD_WIDTH = 164;
inline constexpr int FIELD_HEIGHT = 34;
inline constexpr int ROW_HEIGHT = 36;
// Under Start: the two shortcut rows, or Cancel while a walkthrough records.
inline constexpr int KEYS_SLOT_HEIGHT = 68;
// How many walkthroughs the empty stage offers to open.
inline constexpr int RECENT_SHOWN = 3;
inline constexpr QSize RECENT_THUMB{76, 48};
// The prompt popover.
inline constexpr int PROMPT_POPOVER_WIDTH = 520;

// Whether Auto Mode and the capture delay are offered on `platform` (Python's sys.platform):
// clicks can be watched only on Windows and macOS.
bool offersAutoMode(std::string_view platform);
// This build's sys.platform: "win32", "darwin" or "linux".
std::string_view currentPlatform();

} // namespace walkthrough_tab

// One value in the form (Field): its text at a fixed width, a menu to change it. The menu is
// built each time it opens, from the choices as they are then, so a scan, a saved style or a
// change in Settings never leaves it stale. The chevron is drawn to match the capture delay's
// own chip, which shares the column. Affinity: GUI thread.
class WalkthroughField : public QPushButton {
    Q_OBJECT
public:
    using MenuBuilder = std::function<QMenu*()>;

    WalkthroughField(MenuBuilder buildMenu, const QString& name);

    // A new menu of the choices as they are now; its parent is the menu's owner.
    QMenu* buildMenu() const { return buildMenu_(); }
    // The whole value, whatever part of it fits.
    const QString& value() const { return value_; }
    // Shows text; the tooltip is `tip`, else the text when it does not fit. `tone` "warning"
    // marks a value that needs attention.
    void setValue(const QString& text, const QString& tip = {}, const QString& tone = {});

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void elide();
    void open();

    MenuBuilder buildMenu_;
    QString name_;
    QString value_;
    QString tone_;
};

// One walkthrough on the empty stage (RecentRow): click, Enter or Space opens it.
// Affinity: GUI thread.
class WalkthroughRecentRow : public QFrame {
    Q_OBJECT
public:
    WalkthroughRecentRow(const past_walkthroughs::Summary& summary,
                         std::chrono::year_month_day today, QWidget* parent = nullptr);

    const std::filesystem::path& folder() const { return folder_; }
    ElidingLabel* titleLabel() const { return titleLabel_; }

Q_SIGNALS:
    void clicked(const QString& folder);

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    std::filesystem::path folder_;
    ElidingLabel* titleLabel_ = nullptr;
};

// The prompt, opened beside the column; a click outside or Esc closes it. Affinity: GUI thread.
class PromptPopover : public QWidget {
    Q_OBJECT
public:
    PromptPopover(QWidget* owner, QWidget* content);

    // Opens to the right of `column`, level with `anchor` where the screen allows.
    void openBeside(QWidget* column, QWidget* anchor);

Q_SIGNALS:
    void closed();

protected:
    void hideEvent(QHideEvent* event) override;
};

// What the tab reads that Python took from its module globals.
struct WalkthroughTabOptions {
    // The captures folder; nullopt: <data root>/captures of this app's paths.
    std::optional<std::filesystem::path> capturesRoot;
    // Where recent walkthroughs are read and agents scanned (null: a thread of their own). It
    // must outlive what it was given.
    Executor* io = nullptr;
    // The date recent walkthroughs are told against; empty: QDate::currentDate.
    std::function<QDate()> today;
    // Python's sys.platform, which decides whether Auto Mode is offered.
    std::string platform{walkthrough_tab::currentPlatform()};
};

// Affinity: GUI thread.
class WalkthroughTab : public QWidget, public FittedPage {
    Q_OBJECT
public:
    // QInputDialog::getText(parent, title, label): the text, or nullopt when cancelled.
    using TextPrompt = std::function<std::optional<QString>(QWidget* parent, const QString& title,
                                                            const QString& label)>;
    // QMessageBox::warning(parent, title, text).
    using Warning = std::function<void(QWidget* parent, const QString& title, const QString& text)>;

    // `settings` holds every choice the column shows; it must outlive the tab. `host` is the
    // storyboard's (WalkthroughReview).
    WalkthroughTab(SettingsStore& settings, ReviewHost host, QWidget* parent = nullptr);
    WalkthroughTab(SettingsStore& settings, ReviewHost host, WalkthroughTabOptions options,
                   QWidget* parent = nullptr);
    ~WalkthroughTab() override;

    // How the tab asks for a style's name, warns, asks yes or no and runs the New style dialog
    // (QInputDialog, QMessageBox and exec unless set).
    void setTextPrompt(TextPrompt prompt);
    void setWarning(Warning warning);
    void setQuestion(WalkthroughReview::Question question);
    void setDialogRunner(WalkthroughReview::DialogRunner runner);

    // Both shortcuts, "" for one not set.
    Q_INVOKABLE void setHotkeys(const QString& walkthroughKey, const QString& screenshotKey);
    // A walkthrough records: Finish and Cancel. A dictation records: wait for it.
    Q_INVOKABLE void setRecordingState(bool isRecording, bool walkthroughActive);
    // Shows `folder`, or the newest walkthrough unless it was closed, as the storyboard. Unsaved
    // edits stay unless `ask` is set and the user lets them go; false when they stayed, or while
    // the storyboard is saving.
    bool showWalkthrough(const std::optional<std::filesystem::path>& folder = std::nullopt,
                         bool ask = false);
    // Something else changed the saved walkthroughs: shows the one open as it is now. Unsaved
    // edits here stay; a walkthrough that was deleted gives way to the newest.
    void reloadWalkthrough();
    // Puts the storyboard away until a newer walkthrough finishes.
    void closeWalkthrough();
    // Shows the saved choices without saving them back. Typing not yet saved is saved first: it
    // is newer than anything here.
    void loadSettings();
    // Opens the prompt beside the column; asking for it turns the write-up on.
    void editPrompt();
    // Saves the prompt now; the default's own words save as "", like an empty box.
    void savePrompt();
    // Sizes the column and its values for the type size in use.
    void applySizes();
    // Labels the agents with the last scan; scans once (on the reader) if none has run.
    void showAgents();

    // The stage takes whatever height the window is given, whatever it shows.
    bool fillsHeight() const { return true; }
    // FittedPage: room for the column beside a storyboard you can read, at any type size.
    int preferredWidth() const override;
    void prepareForWidthChange(int grow) override;
    // What the stage shows: "review", "recording" or "empty".
    QString stageState() const;
    // True while the empty stage's list is being read.
    bool isRecentLoading() const { return recentToken_ != 0; }
    bool isRecording() const { return isRecording_; }
    bool isWalkthroughActive() const { return walkthroughActive_; }
    WalkthroughReview* review() const { return review_; }

    // The parts tests and the presenter reach.
    QScrollArea* scrollArea() const { return scrollArea_; }
    StudioSplit* split() const { return split_; }
    Stage* stage() const { return stage_; }
    QFrame* column() const { return column_; }
    SuccessButton* walkthroughButton() const { return walkthroughButton_; }
    WarningButton* cancelButton() const { return cancelButton_; }
    QWidget* keysSlot() const { return keysSlot_; }
    QWidget* keysBox() const { return keysBox_; }
    FeatureBadge* walkthroughBadge() const { return walkthroughBadge_; }
    QLabel* startKey() const { return startKey_; }
    QLabel* snapKey() const { return snapKey_; }
    SettingsSwitch* autoSwitch() const { return autoSwitch_; }
    QWidget* autoRow() const { return autoRow_; }
    ui_snap::CaptureDelay* captureDelay() const { return captureDelay_; }
    QWidget* delayRow() const { return delayRow_; }
    SettingsSwitch* minimizeSwitch() const { return minimizeSwitch_; }
    SettingsSwitch* writeUpSwitch() const { return writeUpSwitch_; }
    QWidget* writeUpRow() const { return writeUpRow_; }
    QLabel* writeUpRowName() const { return writeUpRowName_; }
    WalkthroughField* agentField() const { return agentField_; }
    WalkthroughField* styleField() const { return styleField_; }
    SettingsSwitch* notesSwitch() const { return notesSwitch_; }
    QPushButton* promptLink() const { return promptLink_; }
    // Written by, Style, Follow my notes and the prompt link: dimmed without a write-up.
    const std::vector<QWidget*>& writeUpRows() const { return writeUpRows_; }
    // Every form row, which takes ROW_HEIGHT at least.
    const std::vector<QWidget*>& formRows() const { return formRows_; }
    WalkthroughField* copyField() const { return copyField_; }
    QPushButton* settingsLink() const { return settingsLink_; }
    PromptPopover* promptPopover() const { return promptPopover_; }
    QWidget* promptArea() const { return promptArea_; }
    NoWheelComboBox* presetCombo() const { return presetCombo_; }
    QPlainTextEdit* promptEdit() const { return promptEdit_; }
    QPushButton* saveStyleButton() const { return saveStyleButton_; }
    QPushButton* deleteStyleButton() const { return deleteStyleButton_; }
    QTimer* promptTimer() const { return promptTimer_; }
    QFrame* recordingPane() const { return recordingPane_; }
    QLabel* recordingKeys() const { return recordingKeys_; }
    QFrame* emptyPane() const { return emptyPane_; }
    QLabel* emptyHint() const { return emptyHint_; }
    QWidget* recentBox() const { return recentBox_; }
    const std::vector<WalkthroughRecentRow*>& recentRows() const { return recentRows_; }

    // The menus the fields open, built from the choices as they are now.
    QMenu* copyMenu();
    QMenu* agentMenu();
    QMenu* styleMenu();

Q_SIGNALS:
    // Start a walkthrough, or finish the one recording.
    void walkthroughRequested();
    // Throw the recording walkthrough away.
    void cancelRequested();
    // Open Settings at this destination.
    void settingsRequested(const QString& destination);
    // The storyboard saved, rewrote or deleted a walkthrough: lists of them are stale.
    void walkthroughsChanged();

protected:
    void changeEvent(QEvent* event) override;

private:
    struct Inbox;
    struct AgentState {
        QString status;
        bool found = true;
        QString detail;
    };

    QFrame* buildColumn();
    void addSection(QVBoxLayout* body, const QString& title);
    QWidget* row(const QString& text, QWidget* control, const QString& tip = {},
                 QLabel** name = nullptr);
    QWidget* keyRow(const QString& text, QLabel* chip, QWidget* badge = nullptr);
    QWidget* buildPromptArea();
    QPushButton* styleLink(const QString& text, const QString& tip, void (WalkthroughTab::*slot)());
    void buildStage();
    QFrame* buildRecordingPane();
    QFrame* buildEmptyPane();
    QAction* choice(QMenu* menu, QActionGroup* group, const QString& text, bool on,
                    std::function<void()> slot);
    QMenu* menu();

    void onPromptClosed();
    void loadRecent();
    void recentLoaded(std::uint64_t token, std::vector<past_walkthroughs::Summary> summaries);
    void showStage();
    AgentState agentState(const std::string& agent) const;
    void onWriteUpToggled(bool on);
    void chooseAgent(const std::string& agent);
    void onCopyChosen(std::size_t index);
    void syncCopy();
    void syncWriteUp();
    void onPromptEdited();
    std::optional<walkthrough::WriteUpStyle> currentStyle() const;
    void showPreset();
    void useStyle(const QString& styleId);
    void onPresetChosen(int index);
    void newStyle();
    void saveAsStyle();
    void deleteStyle();
    void reloadStyles();
    std::optional<std::filesystem::path> newest() const;
    bool showOnStage(const std::optional<std::filesystem::path>& folder, bool ask);
    void onReviewChanged(const QString& folder);
    std::chrono::year_month_day today() const;

    SettingsStore& settings_;
    std::filesystem::path capturesRoot_;
    Executor* io_ = nullptr;
    std::function<QDate()> today_;
    std::shared_ptr<Inbox> inbox_;
    TextPrompt textPrompt_;
    Warning warning_;
    WalkthroughReview::Question question_;
    WalkthroughReview::DialogRunner dialogRunner_;

    bool built_ = false;
    bool isRecording_ = false;
    bool walkthroughActive_ = false;
    bool scanStarted_ = false;
    std::uint64_t recentToken_ = 0;
    std::uint64_t recentIssued_ = 0;
    std::optional<QString> stageState_;
    std::string agent_;
    std::size_t copyIndex_ = 0;
    // The user's own prompt, kept while a style is tried in its place.
    QString ownPrompt_;
    std::vector<walkthrough::WriteUpStyle> styles_;

    QScrollArea* scrollArea_ = nullptr;
    Stage* stage_ = nullptr;
    StudioSplit* split_ = nullptr;
    QFrame* column_ = nullptr;
    SuccessButton* walkthroughButton_ = nullptr;
    QWidget* keysSlot_ = nullptr;
    WarningButton* cancelButton_ = nullptr;
    QWidget* keysBox_ = nullptr;
    FeatureBadge* walkthroughBadge_ = nullptr;
    QLabel* startKey_ = nullptr;
    QLabel* snapKey_ = nullptr;
    SettingsSwitch* autoSwitch_ = nullptr;
    QWidget* autoRow_ = nullptr;
    ui_snap::CaptureDelay* captureDelay_ = nullptr;
    QWidget* delayRow_ = nullptr;
    SettingsSwitch* minimizeSwitch_ = nullptr;
    SettingsSwitch* writeUpSwitch_ = nullptr;
    QWidget* writeUpRow_ = nullptr;
    QLabel* writeUpRowName_ = nullptr;
    WalkthroughField* agentField_ = nullptr;
    WalkthroughField* styleField_ = nullptr;
    SettingsSwitch* notesSwitch_ = nullptr;
    QPushButton* promptLink_ = nullptr;
    std::vector<QWidget*> writeUpRows_;
    std::vector<QWidget*> formRows_;
    WalkthroughField* copyField_ = nullptr;
    QPushButton* settingsLink_ = nullptr;
    PromptPopover* promptPopover_ = nullptr;
    QWidget* promptArea_ = nullptr;
    NoWheelComboBox* presetCombo_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    QPushButton* saveStyleButton_ = nullptr;
    QPushButton* deleteStyleButton_ = nullptr;
    QTimer* promptTimer_ = nullptr;
    WalkthroughReview* review_ = nullptr;
    QFrame* recordingPane_ = nullptr;
    QLabel* recordingKeys_ = nullptr;
    QFrame* emptyPane_ = nullptr;
    QLabel* emptyHint_ = nullptr;
    QWidget* recentBox_ = nullptr;
    QVBoxLayout* recentLayout_ = nullptr;
    std::vector<WalkthroughRecentRow*> recentRows_;
};

} // namespace fv
