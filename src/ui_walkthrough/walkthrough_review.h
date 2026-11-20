#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QFrame>
#include <QImage>
#include <QMessageBox>
#include <QPixmap>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QTextEdit>
#include <QToolButton>
#include <QUrl>
#include <QWidget>

#include "base/cancel.h"
#include "base/executor.h"
#include "base/json.h"
#include "walkthrough/session.h"
#include "walkthrough/write_up.h"

class QAction;
class QActionGroup;
class QDialog;
class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

// One walkthrough as a storyboard (ui_qt/widgets/walkthrough_review.py): each screenshot beside
// the words that led to it. The Walkthrough tab shows the newest walkthrough here, or the one
// picked in the side panel; the library dialog shows whichever its list selects. A row is a
// section: its screenshot, then its words, edited in place, with a switch to leave the
// screenshot out. Dragging a row's grip (or Alt+Up/Down in its words) moves the section, words
// and screenshot together. A walkthrough an agent wrote up shows the write-up the same way, cut
// at its screenshots, with a switch to the words as spoken. Every write-up is kept; a picker
// beside the date opens any of them. Save rebuilds the documents off the UI thread; copying and
// opening wait until edits are saved. Affinity: GUI thread.
namespace fv {

class SettingsStore;
class SettingsSwitch;
class SegmentedBar;
class Reflow;
class FlowLayout;
class FileChip;
class ReviewRow;

// What copying, recovery and the app's one-operation-at-a-time gate go through: the five host
// callbacks of walkthrough_review.py. An empty member is Python's None.
struct ReviewHost {
    // Puts `files`, `text` and, for one screenshot, `image` on the clipboard; whether it did.
    std::function<bool(const std::vector<std::filesystem::path>& files, const QString& text,
                       const QImage& image)>
        copyFiles;
    // Starts recovering the draft in `folder`.
    std::function<void(const QString& folder)> recover;
    // Takes the app's single library-operation gate: nullopt or "" when taken, else why not
    // (shown as is). Empty: always taken.
    std::function<std::optional<QString>()> acquire;
    // Gives the gate back; called once after every acquire that succeeded.
    std::function<void()> release;
    // Runs a task on the app's executor; false or an exception when it was not accepted. Empty:
    // a thread of its own.
    std::function<bool(Task)> submit;
};

// What a piece of Review's work leaves: the walkthrough to show (nullopt keeps the folder that
// shows, when it is still there), and, for a save whose write-up was asked for and failed, why
// (_SavedOnly).
struct ReviewOutcome {
    std::optional<walkthrough::WalkthroughResult> result;
    std::optional<std::string> savedOnly;
};
// Work run off the UI thread; `cancel` is set by WalkthroughReview::cancel. An exception is the
// failure the notice shows.
using ReviewOperation = std::function<ReviewOutcome(const CancelToken& cancel)>;

// Which version of a folder shows: the words as spoken, or write-up `version` (nullopt: the
// latest).
struct ReviewView {
    bool spoken = true;
    std::optional<int> version;
};

// A row's words: plain text that grows with them instead of scrolling (_Editor).
class ReviewEditor : public QTextEdit {
    Q_OBJECT

public:
    ReviewEditor(const QString& text, int number);

Q_SIGNALS:
    // Alt+Up (-1) or Alt+Down (+1): move this row.
    void nudged(int step);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void fit();
};

// Six dots that drag their row to another place in the storyboard (_Grip).
class ReviewGrip : public QWidget {
    Q_OBJECT

public:
    explicit ReviewGrip(ReviewRow* row);
    ReviewRow* row() const { return row_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    ReviewRow* row_;
    std::optional<QPoint> press_;
};

// The copy button in a screenshot's corner: a dark chip, so it reads on any picture
// (_CopyCorner).
class ReviewCopyCorner : public QToolButton {
    Q_OBJECT

public:
    explicit ReviewCopyCorner(QWidget* parent);

protected:
    void paintEvent(QPaintEvent* event) override;
};

// A row's screenshot, as large as its row has room for; clicking it opens the full picture
// (_Preview). The picture is decoded once at the largest size it shows and drawn smaller as the
// row narrows, faded while the screenshot is left out. The button in its top-right corner copies
// the screenshot instead.
class ReviewPreview : public QWidget {
    Q_OBJECT

public:
    ReviewPreview(std::filesystem::path image, QPixmap picture, bool included,
                  std::function<bool(const QUrl&)> opener);

    const std::filesystem::path& image() const { return image_; }
    bool included() const { return included_; }
    // Fades the screenshot while it's left out, so the next save can bring it back.
    void setIncluded(bool on);
    ReviewCopyCorner* copyButton() const { return copyButton_; }
    // The decoded picture.
    const QPixmap& picture() const { return picture_; }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    // The corner button was clicked: copy this screenshot.
    void copyRequested(const QString& image);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void placeCopyButton();

    std::filesystem::path image_;
    QPixmap picture_;
    bool included_;
    double ratio_ = 1.0;
    int widest_ = 0;
    std::function<bool(const QUrl&)> opener_;
    ReviewCopyCorner* copyButton_;
};

// One section: its screenshot beside the words that led to it (_Row). index() is the saved
// section (or write-up part) this row shows, so a reordered storyboard saves in its new order.
class ReviewRow : public QFrame {
    Q_OBJECT

public:
    struct Content {
        int index = 0;
        int number = 1;
        QString text;
        std::optional<std::filesystem::path> image;
        bool included = true;
        QString stamp;
        bool canOmit = false;
        std::string line; // a write-up part's screenshot line, as written
        std::vector<std::string> notes;
    };

    ReviewRow(const Content& content, std::function<bool(const QUrl&)> opener);

    int index() const { return index_; }
    const std::string& line() const { return line_; }
    ReviewGrip* grip() const { return grip_; }
    ReviewPreview* preview() const { return preview_; }
    QLabel* step() const { return step_; }
    SettingsSwitch* check() const { return check_; }
    ReviewEditor* editor() const { return editor_; }
    // The step line, with the Include switch beside it or under it (a Reflow when the
    // screenshot can be left out).
    QWidget* top() const { return top_; }
    // The "{name} is missing" or "No screenshot" placeholder when there is no picture.
    QLabel* placeholder() const { return placeholder_; }

    void setNumber(int number);
    void showIncluded(bool on);

private:
    QWidget* picture(const std::optional<std::filesystem::path>& image, bool included,
                     std::function<bool(const QUrl&)> opener);

    int index_;
    std::string line_;
    ReviewGrip* grip_ = nullptr;
    ReviewPreview* preview_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QLabel* step_ = nullptr;
    SettingsSwitch* check_ = nullptr;
    ReviewEditor* editor_ = nullptr;
    QWidget* top_ = nullptr;
};

// The rows in order; a row dropped on it moves to where it lands (_Board).
class ReviewBoard : public QWidget {
    Q_OBJECT

public:
    ReviewBoard();

    std::vector<ReviewRow*>& rows() { return rows_; }
    QVBoxLayout* column() const { return column_; }
    QFrame* dropMarker() const { return marker_; }
    // Where a row dropped at `y` goes: the number of rows above it.
    int slot(double y) const;

Q_SIGNALS:
    // A row moved from one place to another (row positions, not saved indexes).
    void moved(int start, int end);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    ReviewRow* dragged(QDropEvent* event) const;

    std::vector<ReviewRow*> rows_;
    QVBoxLayout* column_;
    QFrame* marker_;
};

class WalkthroughReview : public QWidget {
    Q_OBJECT

public:
    // The largest a screenshot shows beside its words, in logical pixels; it grows toward this
    // with the room its row has.
    static constexpr QSize kPreviewSize{360, 225};
    // The narrowest a screenshot gets, so a narrow window keeps it beside its words.
    static constexpr int kPreviewMinWidth = 140;
    // How a row's width splits between its screenshot and its words.
    static constexpr int kPreviewStretch = 3;
    static constexpr int kWordsStretch = 4;
    static constexpr int kVersionBarWidth = 250;
    // The copy button in a screenshot's corner: its size, and its inset from the corner.
    static constexpr int kShotCopySize = 28;
    static constexpr int kShotCopyInset = 6;
    // A dragged row's MIME type, "application/x-<product in lower case>-walkthrough-row"; its
    // payload is empty, the row is the drag's source.
    static QString rowMime();

    // QMessageBox::question(parent, title, text, buttons, defaultButton): the button chosen.
    using Question = std::function<QMessageBox::StandardButton(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;
    // Runs a modal dialog (QDialog::exec) and returns its result code.
    using DialogRunner = std::function<int(QDialog& dialog)>;
    using Today = std::function<std::chrono::year_month_day()>;

    // `settings` gives the documents' format, the write-up's agent and prompt, and the styles;
    // it must outlive the widget. `closable` shows the Close button (the tab's, not the
    // library's).
    explicit WalkthroughReview(SettingsStore& settings, ReviewHost host = {}, bool closable = false,
                               QWidget* parent = nullptr, Today today = {});
    ~WalkthroughReview() override;

    // Re-binds the host callbacks after construction.
    void bind(ReviewHost host);
    // The agent runs of a write-up go through `runner` (empty: the installed agent itself).
    void setWriteUpRunner(walkthrough::write_up::AgentTaskRunner runner);
    // How files open (QDesktopServices::openUrl unless set), questions are asked
    // (QMessageBox::question unless set) and the prompt dialog runs (exec unless set).
    void setOpener(std::function<bool(const QUrl&)> opener);
    void setQuestion(Question question);
    void setDialogRunner(DialogRunner runner);

    // Shows the walkthrough in `folder` (nullopt: none), dropping unsaved edits. A written-up
    // walkthrough opens on its latest write-up, which is what was copied, unless `spoken` asks
    // for the words as spoken or `version` for an earlier write-up. After a save, the same
    // folder opens on the version that was saved. Whether its rows show.
    bool showFolder(const std::optional<std::filesystem::path>& folder,
                    std::optional<bool> spoken = std::nullopt,
                    std::optional<int> version = std::nullopt);
    // Whether there is nothing unsaved, or the user agrees to lose it.
    bool discardChanges();
    // Runs `operation` on a worker behind the app's gate, then shows what it left: `then` of
    // this folder afterwards (nullopt: what it opens on); `done` is what the notice says after.
    void run(ReviewOperation operation, std::optional<ReviewView> then = std::nullopt,
             const std::optional<QString>& done = std::nullopt);
    // Stops the work in progress as soon as it safely can.
    void cancel();
    // Whether cancel() was asked of the work started last.
    bool cancelRequested() const { return cancel_.cancelled(); }

    const std::optional<std::filesystem::path>& folder() const { return folder_; }
    bool isBusy() const { return busy_; }
    bool isDirty() const { return dirty_; }
    // Whether the words as spoken show, and the earlier write-up that shows (nullopt: latest).
    bool isSpoken() const { return spoken_; }
    std::optional<int> version() const { return version_; }
    // The rows as they show now.
    std::vector<ReviewRow*> rows() const;
    ReviewBoard* board() const { return board_; }
    // Marks the review edited or not, as an edit does.
    void setDirty(bool dirty);
    // Drops the edits: the folder again, on the same version.
    void discard();
    // Moves the row at `start` to `end` (row positions), renumbering the rows.
    void moveRow(int start, int end);
    // The switch between the write-up (0) and the words as spoken (1) was used.
    void chooseVersion(int index);
    // Write-up `version` (nullopt: the latest) was picked in the version menu.
    void chooseWriteUp(std::optional<int> version);
    // Save changes, or Save and redo write-up.
    void save(bool redo = false);
    // Delete…: asks, then deletes the folder.
    void deleteFolder();
    // A Yes/No question (default No) through setQuestion's seam; whether Yes was chosen. The
    // library dialog asks its retention question here too.
    bool ask(const QString& title, const QString& text);

    QLabel* titleLabel() const { return title_; }
    QLabel* metaLabel() const { return meta_; }
    QLabel* kindPill() const { return kind_; }
    QToolButton* versionPicker() const { return versionPicker_; }
    QMenu* versionMenu() const { return versionMenu_; }
    SegmentedBar* versionBar() const { return versionBar_; }
    QToolButton* closeButton() const { return closeButton_; }
    QPushButton* pdfButton() const { return pdfButton_; }
    QPushButton* markdownButton() const { return markdownButton_; }
    QPushButton* copyWordButton() const { return copyWordButton_; }
    FileChip* chip() const { return chip_; }
    QPushButton* openButton() const { return openButton_; }
    QToolButton* moreButton() const { return moreButton_; }
    QMenu* menu() const { return menu_; }
    QAction* copyAgentAction() const { return copyAgentAction_; }
    QAction* openWordAction() const { return openWordAction_; }
    QAction* folderAction() const { return folderAction_; }
    QAction* retryAction() const { return retryAction_; }
    QMenu* styleMenu() const { return styleMenu_; }
    QAction* styleAction() const { return styleAction_; }
    QAction* promptAction() const { return promptAction_; }
    QAction* deleteAction() const { return deleteAction_; }
    QWidget* copyGroup() const { return copyGroup_; }
    Reflow* actionRow() const { return actionRow_; }
    QLabel* notice() const { return notice_; }
    QPushButton* cancelButton() const { return cancelButton_; }
    QScrollArea* scroll() const { return scroll_; }
    QFrame* saveBar() const { return saveBar_; }
    QLabel* saveText() const { return saveText_; }
    QPushButton* discardButton() const { return discardButton_; }
    QPushButton* saveButton() const { return saveButton_; }
    QPushButton* saveRedoButton() const { return saveRedoButton_; }
    QPushButton* recoverButton() const { return recoverButton_; }

    QSize minimumSizeHint() const override;

Q_SIGNALS:
    // Saved, written up again, recovered or deleted: the folder to show now, or "" when it is
    // gone.
    void changed(const QString& folder);
    // Work started or ended off the UI thread; hosts hold their own controls meanwhile.
    void busyChanged(bool busy);
    // The user is done with this walkthrough here; it stays in the library.
    void closeRequested();

private:
    // One of the controls work off the UI thread turns off: a widget or a menu action.
    struct Control {
        QWidget* widget = nullptr;
        QAction* action = nullptr;
        void setEnabled(bool on) const;
        bool isEnabled() const;
    };
    struct Inbox;

    QLayout* buildHeader();
    QWidget* buildActions();
    QFrame* buildSaveBar();
    QPushButton* button(const QString& text, const char* name, std::function<void()> slot);
    QToolButton* iconButton(const QString& icon, const QString& name);
    QAction* menuAction(const QString& text, std::function<void()> slot);

    std::vector<ReviewRow::Content> spokenRows(const walkthrough::Walkthrough& walkthrough) const;
    std::vector<ReviewRow::Content> writeUpRows() const;
    void addRow(ReviewBoard* board, ReviewRow* row);
    void showHeader(const walkthrough::Walkthrough* walkthrough, bool draft);
    void showVersions(bool loaded);
    void showActions(bool ready, bool draft);
    std::filesystem::path resultPdf() const;
    std::filesystem::path resultDocx() const;
    QFrame* draftBanner();
    QWidget* emptyBody(const QString& heading, const QString& text);
    void edited();
    void nudge(ReviewRow* row, int step);
    void done(std::optional<ReviewOutcome> outcome, const std::string& error);
    void copy(std::string_view format);
    void copyScreenshot(const QString& path);
    void retry();
    void redo(std::optional<std::string> instructions);
    void fillStyleMenu();
    void retryWithPrompt();
    void recoverDraft();
    void open();
    void openWord();
    void reveal();
    Json settingsSnapshot() const;

    SettingsStore& settings_;
    Today today_;
    ReviewHost host_;
    walkthrough::write_up::AgentTaskRunner runner_;
    std::function<bool(const QUrl&)> opener_;
    Question question_;
    DialogRunner dialogRunner_;
    std::shared_ptr<Inbox> inbox_;

    bool busy_ = false;
    bool dirty_ = false;
    std::optional<std::filesystem::path> folder_;
    bool written_ = false;
    bool spoken_ = true;
    std::optional<int> version_;
    std::vector<int> earlier_;
    // What to show of a folder once the work in progress is done, as (folder, view), or nullopt
    // for what it opens on; it moves to next_ when the work succeeds, for the host's showFolder.
    std::optional<std::pair<std::filesystem::path, ReviewView>> then_;
    std::optional<std::pair<std::filesystem::path, ReviewView>> next_;
    CancelSource cancel_;
    QString doneText_;
    std::vector<bool> enabled_;
    std::vector<Control> actions_;

    ReviewBoard* board_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* meta_ = nullptr;
    QLabel* kind_ = nullptr;
    QToolButton* versionPicker_ = nullptr;
    QMenu* versionMenu_ = nullptr;
    QActionGroup* versionGroup_ = nullptr;
    SegmentedBar* versionBar_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    QPushButton* pdfButton_ = nullptr;
    QPushButton* markdownButton_ = nullptr;
    QPushButton* copyWordButton_ = nullptr;
    FileChip* chip_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QToolButton* moreButton_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction* copyAgentAction_ = nullptr;
    QAction* openWordAction_ = nullptr;
    QAction* folderAction_ = nullptr;
    QAction* retryAction_ = nullptr;
    QMenu* styleMenu_ = nullptr;
    QAction* styleAction_ = nullptr;
    QAction* promptAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QWidget* copyGroup_ = nullptr;
    Reflow* actionRow_ = nullptr;
    QLabel* notice_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QFrame* saveBar_ = nullptr;
    QLabel* saveText_ = nullptr;
    QPushButton* discardButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveRedoButton_ = nullptr;
    QPushButton* recoverButton_ = nullptr;
};

} // namespace fv
