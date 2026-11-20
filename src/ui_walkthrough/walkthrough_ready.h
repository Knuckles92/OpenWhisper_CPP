#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <QFrame>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include "walkthrough/session.h"

class QAction;
class QEnterEvent;
class QLabel;
class QMenu;
class QMimeData;
class QToolButton;
class QVBoxLayout;

// The card that says a walkthrough is ready (ui_qt/widgets/walkthrough_ready.py): the file chip
// and the card. Affinity: GUI thread.
namespace fv {

// The document to drag into any app that takes files, or to click and open.
class FileChip : public QFrame {
    Q_OBJECT

public:
    explicit FileChip(QWidget* parent = nullptr);
    FileChip(const QString& path, QWidget* parent);

    void setPath(const QString& path);
    QString path() const { return path_; }
    QLabel* nameLabel() const { return name_; }

    // The file as a drop: a file URL, which apps attach and terminals type as a path. Empty
    // until a path is set.
    QMimeData* mimeData() const;

Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QLabel* name_ = nullptr;
    QString path_;
    bool hasPath_ = false;
    std::optional<QPoint> press_;
};

// A corner card for the newest walkthrough, reused for the next one. It sits in the corner of
// the screen under the pointer without taking focus, so the user can paste straight into the app
// they were in. Dragging the file chip works in every app that takes files; clicking it opens the
// document. Copying again and showing the folder wait behind the "..." button.
//
// It stays 14 s when the pointer never visits it, then 4 s after the pointer leaves; an open menu
// and the pointer on the card hold it. Review, Open PDF, Open Word document and Show in folder
// hide it; a copy keeps it and starts the 14 s again.
class WalkthroughReadyCard : public QWidget {
    Q_OBJECT

public:
    // How long the card stays when the pointer never visits it, and once it leaves, in ms.
    static constexpr int kShowMs = 14'000;
    static constexpr int kLingerMs = 4'000;
    static constexpr QSize kThumbSize{76, 48};
    // The gap to the screen's available area.
    static constexpr int kScreenMargin = 18;
    // Wraps a write-up failure's reason instead of widening the card.
    static constexpr int kReasonWidth = 300;

    WalkthroughReadyCard();

    // What the title says: "<name> copied", or "<name> saved" when copying failed.
    static QString titleFor(const walkthrough::WalkthroughResult& result, bool copied);
    // The line under it: "<n> screenshot(s) · <m:ss> — <note>", or "<n> screenshot(s) · copying
    // failed — drag it from here". `copyFormat` is the resolved walkthrough_copy_format and
    // `autoPaste` the auto_paste setting; both matter only for a copy that worked.
    static QString subtitleFor(const walkthrough::WalkthroughResult& result, bool copied,
                               bool autoPaste, std::string_view copyFormat);
    // The first screenshot cropped to the thumbnail's shape, with rounded corners; null when
    // there is none or it cannot be read.
    static QPixmap thumbnail(const std::optional<std::filesystem::path>& shot);

    // Fills the card for `result` and shows it in the corner of the pointer's screen.
    void present(const walkthrough::WalkthroughResult& result, bool copied, bool autoPaste,
                 std::string_view copyFormat);
    // Replaces manual-copy guidance, only when `result` is the card being pasted (its PDF is the
    // card's): the title becomes the result's name and the subtitle `status`.
    void showPasteStatus(const walkthrough::WalkthroughResult& result, const QString& status);

    // How "Open PDF" and "Open Word document" open a file: QDesktopServices::openUrl unless set.
    void setOpener(std::function<bool(const QUrl&)> opener);

    QLabel* titleLabel() const { return title_; }
    QLabel* subtitleLabel() const { return subtitle_; }
    // Why the write-up failed, in full; hidden without a failure.
    QLabel* reasonLabel() const { return reason_; }
    QLabel* thumbLabel() const { return thumb_; }
    FileChip* chip() const { return chip_; }
    QToolButton* reviewButton() const { return review_; }
    QToolButton* moreButton() const { return more_; }
    QToolButton* closeButton() const { return close_; }
    QMenu* menu() const { return menu_; }
    QAction* openAction() const { return open_; }
    QAction* openWordAction() const { return openWord_; }
    QAction* revealAction() const { return reveal_; }
    QAction* copyPdfAction() const { return copyPdf_; }
    QAction* copyAgentAction() const { return copyAgent_; }
    QAction* copyWordAction() const { return copyWord_; }
    QAction* copyMarkdownAction() const { return copyMarkdown_; }
    // The result the card shows now.
    const std::optional<walkthrough::WalkthroughResult>& result() const { return result_; }
    // The hide timer: running, and for how long it was last started.
    bool hideTimerActive() const { return timer_.isActive(); }
    int hideTimerInterval() const { return timer_.interval(); }

Q_SIGNALS:
    // The folder of the walkthrough to review.
    void reviewRequested(const QString& folder);
    // The folder of the walkthrough and the walkthrough_copy_format to copy it in.
    void copyRequested(const QString& folder, const QString& format);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void build();
    QVBoxLayout* details();
    QToolButton* buildMoreButton();
    QAction* menuAction(const QString& text, std::function<void()> slot);
    void place();
    void linger();
    void review();
    void copy(std::string_view format);
    void open();
    void openWord();
    void revealPdf();

    std::optional<walkthrough::WalkthroughResult> result_;
    std::function<bool(const QUrl&)> opener_;
    QTimer timer_{this};
    QLabel* thumb_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* reason_ = nullptr;
    FileChip* chip_ = nullptr;
    QToolButton* review_ = nullptr;
    QToolButton* more_ = nullptr;
    QToolButton* close_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction* open_ = nullptr;
    QAction* openWord_ = nullptr;
    QAction* reveal_ = nullptr;
    QAction* copyPdf_ = nullptr;
    QAction* copyAgent_ = nullptr;
    QAction* copyWord_ = nullptr;
    QAction* copyMarkdown_ = nullptr;
};

// show_ready: shows `card` (made the first time) for `result`; the card. A desktop whose
// compositor places windows would get none (OpenWhisper's Wayland rule); FioriVoce runs on
// Windows only, where there is always a place.
WalkthroughReadyCard* showReady(std::unique_ptr<WalkthroughReadyCard>& card,
                                const walkthrough::WalkthroughResult& result, bool copied,
                                bool autoPaste, std::string_view copyFormat);

} // namespace fv
