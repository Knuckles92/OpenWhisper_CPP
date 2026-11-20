#include "ui_walkthrough/walkthrough_ready.h"

#include <format>
#include <system_error>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDrag>
#include <QEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "base/utf8.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_walkthrough/walkthrough_copy_tips.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/file_reveal.h"
#include "walkthrough/document.h"

namespace fv {

FileChip::FileChip(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("walkthroughFileChip"));
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral(u"Click to open · drag into any app to attach it"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(9, 5, 11, 5);
    row->setSpacing(6);
    auto* icon = new QLabel();
    icon->setObjectName(QStringLiteral("walkthroughFileIcon"));
    icon->setPixmap(ui::designIcon(QStringLiteral("file-text-blue.svg")).pixmap(14, 14));
    row->addWidget(icon);
    name_ = new QLabel(QString());
    name_->setObjectName(QStringLiteral("walkthroughFileName"));
    row->addWidget(name_);
}

FileChip::FileChip(const QString& path, QWidget* parent) : FileChip(parent) {
    setPath(path);
}

void FileChip::setPath(const QString& path) {
    path_ = path;
    hasPath_ = true;
    name_->setText(QFileInfo(path).fileName());
}

QMimeData* FileChip::mimeData() const {
    auto* drop = new QMimeData();
    if (hasPath_) {
        drop->setUrls({QUrl::fromLocalFile(path_)});
    }
    return drop;
}

void FileChip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        press_ = event->position().toPoint();
    }
    QFrame::mousePressEvent(event);
}

void FileChip::mouseMoveEvent(QMouseEvent* event) {
    if (!press_ || !hasPath_) {
        return;
    }
    if ((event->position().toPoint() - *press_).manhattanLength() <
        QApplication::startDragDistance()) {
        return;
    }
    press_.reset();
    auto* drag = new QDrag(this);
    drag->setMimeData(mimeData());
    drag->exec(Qt::CopyAction);
}

void FileChip::mouseReleaseEvent(QMouseEvent* event) {
    const std::optional<QPoint> pressed = std::exchange(press_, std::nullopt);
    if (pressed && event->button() == Qt::LeftButton) {
        Q_EMIT clicked();
    }
    QFrame::mouseReleaseEvent(event);
}

namespace {

// The walkthrough_copy_format values the menu copies in.
constexpr std::string_view kPdf = "pdf";
constexpr std::string_view kAgentTask = "agent_task";
constexpr std::string_view kAgent = "agent";
constexpr std::string_view kWord = "word";

QString q(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString fileOf(const std::filesystem::path& path) {
    return QString::fromStdString(path_to_utf8(path));
}

std::string countOf(const walkthrough::WalkthroughResult& result) {
    return std::format("{} screenshot{}", result.shots, result.shots == 1 ? "" : "s");
}

} // namespace

WalkthroughReadyCard::WalkthroughReadyCard()
    : QWidget(nullptr), opener_([](const QUrl& url) { return QDesktopServices::openUrl(url); }) {
    setObjectName(QStringLiteral("walkthroughReady"));
    setWindowTitle(QStringLiteral("Walkthrough"));
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                   Qt::WindowDoesNotAcceptFocus);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &QWidget::hide);
    build();
}

void WalkthroughReadyCard::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* surface = new QFrame();
    surface->setObjectName(QStringLiteral("walkthroughReadySurface"));
    outer->addWidget(surface);
    auto* layout = new QHBoxLayout(surface);
    layout->setContentsMargins(12, 12, 10, 12);
    layout->setSpacing(12);
    thumb_ = new QLabel();
    thumb_->setObjectName(QStringLiteral("walkthroughThumb"));
    thumb_->setFixedSize(kThumbSize);
    layout->addWidget(thumb_, 0, Qt::AlignTop);
    layout->addLayout(details(), 1);
    close_ = new QToolButton();
    close_->setObjectName(QStringLiteral("walkthroughReadyClose"));
    close_->setIcon(ui::designIcon(QStringLiteral("x-gray.svg")));
    close_->setIconSize(QSize(13, 13));
    close_->setCursor(Qt::PointingHandCursor);
    close_->setAccessibleName(QStringLiteral("Close"));
    connect(close_, &QToolButton::clicked, this, &QWidget::hide);
    layout->addWidget(close_, 0, Qt::AlignTop);
}

QVBoxLayout* WalkthroughReadyCard::details() {
    auto* column = new QVBoxLayout();
    column->setSpacing(3);
    title_ = new QLabel();
    title_->setObjectName(QStringLiteral("walkthroughReadyTitle"));
    column->addWidget(title_);
    subtitle_ = new QLabel();
    subtitle_->setObjectName(QStringLiteral("walkthroughReadySubtitle"));
    column->addWidget(subtitle_);
    // Why the write-up failed, in full: the status line cuts it off.
    reason_ = new QLabel();
    reason_->setObjectName(QStringLiteral("walkthroughReadyReason"));
    reason_->setWordWrap(true);
    reason_->setMaximumWidth(kReasonWidth);
    reason_->setVisible(false);
    column->addWidget(reason_);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 8, 0, 0);
    row->setSpacing(4);
    chip_ = new FileChip(this);
    connect(chip_, &FileChip::clicked, this, [this] { open(); });
    row->addWidget(chip_);
    row->addStretch(1);
    review_ = new QToolButton();
    ui::setStyleProperty(review_, ui::prop::walkthroughAction, true);
    review_->setText(QStringLiteral("Review"));
    review_->setToolTip(
        QStringLiteral("Edit the words, leave screenshots out, or redo the write-up"));
    review_->setCursor(Qt::PointingHandCursor);
    review_->setFocusPolicy(Qt::NoFocus);
    connect(review_, &QToolButton::clicked, this, [this] { review(); });
    row->addWidget(review_);
    row->addWidget(buildMoreButton());
    column->addLayout(row);
    return column;
}

// The "..." button: what a moment after finishing rarely needs.
QToolButton* WalkthroughReadyCard::buildMoreButton() {
    menu_ = contextMenu(this);
    menu_->setToolTipsVisible(true);
    open_ = menuAction(QStringLiteral("Open PDF"), [this] { open(); });
    openWord_ = menuAction(QStringLiteral("Open Word document"), [this] { openWord(); });
    reveal_ = menuAction(QStringLiteral("Show in folder"), [this] { revealPdf(); });
    menu_->addSeparator();
    copyPdf_ = menuAction(QStringLiteral("Copy PDF"), [this] { copy(kPdf); });
    copyAgent_ = menuAction(QStringLiteral("Copy for agent"), [this] { copy(kAgentTask); });
    copyWord_ = menuAction(QStringLiteral("Copy Word document"), [this] { copy(kWord); });
    copyMarkdown_ = menuAction(QStringLiteral("Copy Markdown"), [this] { copy(kAgent); });
    setCopyTip(copyPdf_, kPdf);
    setCopyTip(copyAgent_, kAgentTask);
    setCopyTip(copyWord_, kWord);
    setCopyTip(copyMarkdown_, kAgent);
    // The pointer leaves the card for the menu; don't let the card fade under it.
    connect(menu_, &QMenu::aboutToShow, this, [this] { timer_.stop(); });
    connect(menu_, &QMenu::aboutToHide, this, [this] { linger(); });
    more_ = new QToolButton();
    more_->setObjectName(QStringLiteral("walkthroughReadyMore"));
    more_->setIcon(ui::designIcon(QStringLiteral("dots-gray.svg")));
    more_->setIconSize(QSize(16, 16));
    more_->setCursor(Qt::PointingHandCursor);
    more_->setFocusPolicy(Qt::NoFocus);
    more_->setAccessibleName(QStringLiteral("More"));
    more_->setPopupMode(QToolButton::InstantPopup);
    more_->setMenu(menu_);
    return more_;
}

QAction* WalkthroughReadyCard::menuAction(const QString& text, std::function<void()> slot) {
    QAction* action = menu_->addAction(text);
    connect(action, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    return action;
}

QString WalkthroughReadyCard::titleFor(const walkthrough::WalkthroughResult& result, bool copied) {
    return q(result.name()) + (copied ? QStringLiteral(" copied") : QStringLiteral(" saved"));
}

QString WalkthroughReadyCard::subtitleFor(const walkthrough::WalkthroughResult& result, bool copied,
                                          bool autoPaste, std::string_view copyFormat) {
    if (!copied) {
        return q(countOf(result) + " · copying failed — drag it from here");
    }
    const bool failed = !result.write_up_error.empty();
    std::string note = failed ? "write-up failed, copied as spoken" : "paste it into your agent";
    if (copyFormat == kAgentTask) {
        if (autoPaste) {
            note = failed ? "write-up failed; preparing to paste as-spoken PDF, then instructions"
                          : "preparing to paste PDF, then instructions";
        } else {
            note = failed ? "write-up failed; paste instructions and attach the as-spoken PDF"
                          : "paste instructions, then attach the PDF to your chat";
        }
    }
    return q(countOf(result) + " · " + walkthrough::clock(result.duration_s) + " — " + note);
}

QPixmap WalkthroughReadyCard::thumbnail(const std::optional<std::filesystem::path>& shot) {
    const QImage image = shot ? QImage(fileOf(*shot)) : QImage();
    if (image.isNull()) {
        return {};
    }
    const QImage scaled =
        image.scaled(kThumbSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = (scaled.width() - kThumbSize.width()) / 2;
    const int y = (scaled.height() - kThumbSize.height()) / 2;
    const QImage cropped = scaled.copy(QRect(QPoint(x, y), kThumbSize));
    QPixmap rounded(kThumbSize);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rounded.rect()), 7, 7);
    painter.setClipPath(clip);
    painter.drawImage(0, 0, cropped);
    painter.end();
    return rounded;
}

void WalkthroughReadyCard::present(const walkthrough::WalkthroughResult& result, bool copied,
                                   bool autoPaste, std::string_view copyFormat) {
    result_ = result;
    title_->setText(titleFor(result, copied));
    subtitle_->setText(subtitleFor(result, copied, autoPaste, copyFormat));
    const bool failed = !result.write_up_error.empty();
    reason_->setText(failed ? QStringLiteral("Write-up failed: ") + q(result.write_up_error)
                            : QString());
    reason_->setVisible(failed);
    const QPixmap thumb = thumbnail(result.first_shot);
    thumb_->setVisible(!thumb.isNull());
    if (!thumb.isNull()) {
        thumb_->setPixmap(thumb);
    }
    chip_->setPath(fileOf(result.pdf));
    std::error_code ignored;
    const bool hasWord = std::filesystem::is_regular_file(result.docx(), ignored);
    openWord_->setEnabled(hasWord);
    copyWord_->setEnabled(hasWord);
    adjustSize();
    place();
    show();
    raise();
    timer_.start(kShowMs);
}

void WalkthroughReadyCard::showPasteStatus(const walkthrough::WalkthroughResult& result,
                                           const QString& status) {
    if (!result_ || result_->pdf != result.pdf) {
        return;
    }
    title_->setText(q(result.name()));
    subtitle_->setText(status);
    adjustSize();
    place();
}

void WalkthroughReadyCard::setOpener(std::function<bool(const QUrl&)> opener) {
    if (opener) {
        opener_ = std::move(opener);
    } else {
        opener_ = [](const QUrl& url) {
            return QDesktopServices::openUrl(url);
        };
    }
}

void WalkthroughReadyCard::place() {
    QScreen* screen = QApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect area = screen->availableGeometry();
    const QSize size = sizeHint();
    move(area.right() - size.width() - kScreenMargin + 1,
         area.bottom() - size.height() - kScreenMargin + 1);
}

void WalkthroughReadyCard::enterEvent(QEnterEvent* event) {
    timer_.stop();
    QWidget::enterEvent(event);
}

void WalkthroughReadyCard::leaveEvent(QEvent* event) {
    if (!menu_->isVisible()) {
        linger();
    }
    QWidget::leaveEvent(event);
}

void WalkthroughReadyCard::linger() {
    if (isVisible() && !underMouse()) {
        timer_.start(kLingerMs);
    }
}

void WalkthroughReadyCard::review() {
    if (result_) {
        Q_EMIT reviewRequested(fileOf(result_->folder));
        hide();
    }
}

void WalkthroughReadyCard::copy(std::string_view format) {
    if (result_) {
        Q_EMIT copyRequested(fileOf(result_->folder), q(format));
        timer_.start(kShowMs);
    }
}

void WalkthroughReadyCard::open() {
    if (result_) {
        opener_(QUrl::fromLocalFile(fileOf(result_->pdf)));
    }
    hide();
}

void WalkthroughReadyCard::openWord() {
    if (result_) {
        opener_(QUrl::fromLocalFile(fileOf(result_->docx())));
    }
    hide();
}

void WalkthroughReadyCard::revealPdf() {
    if (result_) {
        revealInFileManager(fileOf(result_->pdf));
    }
    hide();
}

WalkthroughReadyCard* showReady(std::unique_ptr<WalkthroughReadyCard>& card,
                                const walkthrough::WalkthroughResult& result, bool copied,
                                bool autoPaste, std::string_view copyFormat) {
    if (card == nullptr) {
        card = std::make_unique<WalkthroughReadyCard>();
    }
    card->present(result, copied, autoPaste, copyFormat);
    return card.get();
}

} // namespace fv
