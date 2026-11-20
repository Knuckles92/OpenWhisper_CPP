#include "ui_walkthrough/walkthrough_review.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTextDocument>
#include <QThread>
#include <QVBoxLayout>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_theme/icons.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_walkthrough/review_text.h"
#include "ui_walkthrough/walkthrough_copy_tips.h"
#include "ui_walkthrough/walkthrough_icons.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_walkthrough/write_up_prompt_dialog.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/file_reveal.h"
#include "ui_widgets/flow_layout.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "walkthrough/document.h"
#include "walkthrough/files.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"
#include "walkthrough/styles.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.walkthrough_review"};

namespace text = walkthrough_review;

QString qs(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString qpath(const std::filesystem::path& path) {
    return qs(path_to_utf8(path));
}

std::string utf8(const QString& value) {
    return value.toStdString();
}

// Python's round(): halves go to the even neighbour.
int pyRound(double value) {
    return static_cast<int>(std::nearbyint(value));
}

std::filesystem::path named(const std::filesystem::path& folder, std::string_view name) {
    return folder / path_from_utf8(name);
}

// str(Path): the platform's separators.
QString shownPath(const std::filesystem::path& path) {
    auto preferred = path;
    preferred.make_preferred();
    return qpath(preferred);
}

// Whether `text` is UTF-8 Python's strict codec accepts.
bool validUtf8(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        int more = 0;
        unsigned int code = 0;
        if (lead < 0x80) {
            ++at;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
            code = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            code = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            code = lead & 0x07;
        } else {
            return false;
        }
        if (at + static_cast<std::size_t>(more) >= text.size()) {
            return false;
        }
        for (int i = 1; i <= more; ++i) {
            const auto next = static_cast<unsigned char>(text[at + i]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if ((more == 2 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) ||
            (more == 3 && (code < 0x10000 || code > 0x10FFFF))) {
            return false;
        }
        at += static_cast<std::size_t>(more) + 1;
    }
    return true;
}

// Path.read_text(encoding="utf-8"): strict UTF-8, universal newlines.
std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::filesystem::filesystem_error(
            "No such file or directory", path,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!validUtf8(bytes)) {
        throw std::invalid_argument("'utf-8' codec can't decode the write-up");
    }
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t at = 0; at < bytes.size(); ++at) {
        if (bytes[at] == '\r') {
            out.push_back('\n');
            if (at + 1 < bytes.size() && bytes[at + 1] == '\n') {
                ++at;
            }
        } else {
            out.push_back(bytes[at]);
        }
    }
    return out;
}

QString reviewSheet() {
    QFile file(QStringLiteral(":/ui_walkthrough/walkthrough_review.qss"));
    if (!file.open(QIODevice::ReadOnly)) {
        kLog.warning("The storyboard's stylesheet is missing from the resources");
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

// The screenshot decoded at the largest size it shows (_preview).
std::optional<QPixmap> decodePreview(const std::filesystem::path& path, double ratio) {
    QImageReader reader(qpath(path));
    const QSize size = reader.size();
    if (!size.isValid()) {
        return std::nullopt;
    }
    const QSize box = WalkthroughReview::kPreviewSize;
    const QSize shown = size.width() > box.width() || size.height() > box.height()
                            ? size.scaled(box, Qt::KeepAspectRatio)
                            : size;
    reader.setScaledSize(shown * ratio);
    const QImage image = reader.read();
    if (image.isNull()) {
        return std::nullopt;
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

QWidget* rowLine(std::initializer_list<QWidget*> widgets, bool stretch) {
    auto* line = new QWidget();
    line->setObjectName(QStringLiteral("wlRowLine"));
    auto* row = new QHBoxLayout(line);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    for (QWidget* widget : widgets) {
        row->addWidget(widget);
    }
    if (stretch) {
        row->addStretch(1);
    }
    return line;
}

// QScrollArea recomputes its scroll range when it resizes the board. Editors can
// change their height during that resize. Let the outer calculation finish before
// another resize of the same board starts a second scrollbar calculation.
class ReviewScrollArea final : public QScrollArea {
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == widget() && event->type() == QEvent::Resize) {
            if (sizing_) {
                return false;
            }
            QScopedValueRollback guard(sizing_, true);
            return QScrollArea::eventFilter(watched, event);
        }
        return QScrollArea::eventFilter(watched, event);
    }

private:
    bool sizing_ = false;
};

// The copy buttons and the file: one line while they fit, wrapping once they don't
// (_CopyGroup).
class CopyGroup : public QWidget {
public:
    explicit CopyGroup(std::initializer_list<QWidget*> widgets) {
        setObjectName(QStringLiteral("wlActionGroup"));
        flow_ = new FlowLayout(this, 8);
        for (QWidget* widget : widgets) {
            flow_->addWidget(widget);
        }
    }

    // One line's worth, so Open drops below before the copy buttons wrap.
    QSize sizeHint() const override {
        const int width = flow_->lineWidth();
        return {width, flow_->heightForWidth(width)};
    }

private:
    FlowLayout* flow_;
};

QWidget* actionGroup(std::initializer_list<QWidget*> widgets) {
    auto* group = new QWidget();
    group->setObjectName(QStringLiteral("wlActionGroup"));
    auto* row = new QHBoxLayout(group);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    for (QWidget* widget : widgets) {
        row->addWidget(widget);
    }
    row->addStretch(1);
    return group;
}

} // namespace

// ---- rows ----

ReviewEditor::ReviewEditor(const QString& text, int number) {
    setObjectName(QStringLiteral("wlEditor"));
    setAcceptRichText(false);
    setTabChangesFocus(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setPlaceholderText(QStringLiteral("Nothing was said here"));
    setPlainText(text);
    setAccessibleName(QStringLiteral("Section %1 transcript").arg(number));
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this] { fit(); });
}

void ReviewEditor::fit() {
    const QMargins margins = contentsMargins();
    const int height = pyRound(document()->size().height()) + margins.top() + margins.bottom() + 2;
    setFixedHeight(std::max(46, height));
}

void ReviewEditor::resizeEvent(QResizeEvent* event) {
    QTextEdit::resizeEvent(event);
    fit();
}

void ReviewEditor::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() == Qt::AltModifier &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        Q_EMIT nudged(event->key() == Qt::Key_Up ? -1 : 1);
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

ReviewGrip::ReviewGrip(ReviewRow* row) : QWidget(row), row_(row) {
    setFixedWidth(16);
    setCursor(Qt::OpenHandCursor);
    setToolTip(
        QStringLiteral("Drag to move this screenshot and its words (or Alt+Up/Down in the words)"));
}

void ReviewGrip::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ui::tokenColor("text-muted"));
    const double x = width() / 2.0;
    const double y = height() / 2.0;
    for (const double dx : {-3.0, 3.0}) {
        for (const double dy : {-6.0, 0.0, 6.0}) {
            painter.drawEllipse(QPointF(x + dx, y + dy), 1.6, 1.6);
        }
    }
}

void ReviewGrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        press_ = event->position().toPoint();
    }
}

void ReviewGrip::mouseMoveEvent(QMouseEvent* event) {
    if (!press_) {
        return;
    }
    if ((event->position().toPoint() - *press_).manhattanLength() <
        QApplication::startDragDistance()) {
        return;
    }
    press_.reset();
    auto* mime = new QMimeData();
    mime->setData(WalkthroughReview::rowMime(), QByteArray());
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap picture = row_->grab();
    drag->setPixmap(
        picture.scaledToWidth(std::min(picture.width(), 420), Qt::SmoothTransformation));
    drag->exec(Qt::MoveAction);
}

void ReviewGrip::mouseReleaseEvent(QMouseEvent* /*event*/) {
    press_.reset();
}

ReviewCopyCorner::ReviewCopyCorner(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("wlShotCopy"));
    const int side = WalkthroughReview::kShotCopySize;
    setFixedSize(side, side);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    // Repaint on hover, so the chip darkens under the pointer.
    setAttribute(Qt::WA_Hover);
    setToolTip(QStringLiteral("Copy this screenshot"));
    setAccessibleName(QStringLiteral("Copy screenshot"));
}

void ReviewCopyCorner::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool hot = underMouse() || isDown() || hasFocus();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, hot ? 190 : 120));
    painter.drawRoundedRect(QRectF(rect()), 7, 7);
    paintWalkthroughIcon(painter, "copy", QRectF(rect()).center(), 16,
                         QColor(255, 255, 255, hot ? 255 : 215));
}

ReviewPreview::ReviewPreview(std::filesystem::path image, QPixmap picture, bool included,
                             std::function<bool(const QUrl&)> opener)
    : image_(std::move(image)), picture_(std::move(picture)), included_(included),
      opener_(std::move(opener)) {
    setObjectName(QStringLiteral("wlPreview"));
    setAccessibleName(QStringLiteral("Preview of %1").arg(qpath(image_.filename())));
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Open the full screenshot"));
    const QSizeF size = picture_.deviceIndependentSize();
    ratio_ = size.height() / size.width();
    // Never drawn larger than decoded: a small region stays its own size.
    widest_ = pyRound(size.width());
    setMinimumWidth(std::min(WalkthroughReview::kPreviewMinWidth, widest_));
    setMaximumWidth(widest_);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    copyButton_ = new ReviewCopyCorner(this);
    connect(copyButton_, &QToolButton::clicked, this,
            [this] { Q_EMIT copyRequested(shownPath(image_)); });
    placeCopyButton();
}

void ReviewPreview::placeCopyButton() {
    // In the corner of the picture as drawn, which stops at its decoded width.
    const int shown = std::min(width(), widest_);
    copyButton_->move(
        std::max(0, shown - WalkthroughReview::kShotCopySize - WalkthroughReview::kShotCopyInset),
        WalkthroughReview::kShotCopyInset);
}

void ReviewPreview::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeCopyButton();
}

void ReviewPreview::setIncluded(bool on) {
    included_ = on;
    update();
}

int ReviewPreview::heightForWidth(int width) const {
    return pyRound(std::min(width, widest_) * ratio_);
}

QSize ReviewPreview::sizeHint() const {
    // The row shares out the rest by stretch, so every row's screenshot is as wide.
    return minimumSizeHint();
}

QSize ReviewPreview::minimumSizeHint() const {
    const int width = minimumWidth();
    return {width, heightForWidth(width)};
}

void ReviewPreview::paintEvent(QPaintEvent* /*event*/) {
    const int width = std::min(this->width(), widest_);
    const QRectF shown(0, 0, width, heightForWidth(width));
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath corners;
    corners.addRoundedRect(shown, 8, 8);
    painter.setClipPath(corners);
    if (!included_) {
        painter.setOpacity(0.28);
    }
    painter.drawPixmap(shown, picture_, QRectF(picture_.rect()));
}

void ReviewPreview::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && opener_) {
        opener_(QUrl::fromLocalFile(shownPath(image_)));
    }
}

ReviewRow::ReviewRow(const Content& content, std::function<bool(const QUrl&)> opener)
    : index_(content.index), line_(content.line) {
    setObjectName(QStringLiteral("wlSection"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 12, 14, 12);
    row->setSpacing(10);
    grip_ = new ReviewGrip(this);
    row->addWidget(grip_);
    row->addWidget(picture(content.image, content.included, std::move(opener)),
                   WalkthroughReview::kPreviewStretch, Qt::AlignTop);
    auto* column = new QVBoxLayout();
    column->setSpacing(8);
    step_ = new QLabel(QString::number(content.number));
    step_->setObjectName(QStringLiteral("wlStep"));
    step_->setFixedSize(22, 22);
    step_->setAlignment(Qt::AlignCenter);
    QLabel* stamp = nullptr;
    if (!content.stamp.isEmpty()) {
        stamp = new QLabel(content.stamp);
        stamp->setObjectName(QStringLiteral("wlStamp"));
    }
    check_ = new SettingsSwitch();
    check_->setChecked(content.included);
    check_->setAccessibleName(
        content.image ? QStringLiteral("Include %1").arg(qpath(content.image->filename()))
                      : QStringLiteral("No screenshot"));
    const auto lead = [&] {
        return stamp != nullptr ? rowLine({step_, stamp}, true) : rowLine({step_}, true);
    };
    if (content.image && content.canOmit) {
        auto* include = new QLabel(QStringLiteral("Include screenshot"));
        include->setObjectName(QStringLiteral("wlInclude"));
        // The switch drops under the step once the words are too narrow for both, so a narrow
        // storyboard never runs past its right edge.
        top_ = new Reflow(lead(), rowLine({include, check_}, false), std::nullopt, nullptr,
                          Qt::AlignLeft);
    } else {
        check_->setEnabled(false);
        check_->hide();
        top_ = lead();
    }
    column->addWidget(top_);
    editor_ = new ReviewEditor(content.text, content.number);
    column->addWidget(editor_);
    for (const auto& note : content.notes) {
        // Held for the agent: shown so Retry's input is visible, never edited.
        auto* label =
            new QLabel(QStringLiteral(u"Note for the agent · not in the document\n") + qs(note));
        label->setObjectName(QStringLiteral("wlNote"));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        column->addWidget(label);
    }
    column->addStretch(1);
    row->addLayout(column, WalkthroughReview::kWordsStretch);
}

QWidget* ReviewRow::picture(const std::optional<std::filesystem::path>& image, bool included,
                            std::function<bool(const QUrl&)> opener) {
    std::optional<QPixmap> shown;
    if (image) {
        shown = decodePreview(*image, devicePixelRatioF());
    }
    if (shown) {
        preview_ = new ReviewPreview(*image, *shown, included, std::move(opener));
        return preview_;
    }
    // As wide as the screenshots in the other rows, so the words line up.
    placeholder_ = new QLabel(image ? QStringLiteral("%1 is missing").arg(qpath(image->filename()))
                                    : QStringLiteral("No screenshot"));
    placeholder_->setObjectName(QStringLiteral("wlNoShot"));
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setWordWrap(true);
    placeholder_->setMinimumWidth(WalkthroughReview::kPreviewMinWidth);
    placeholder_->setMaximumWidth(WalkthroughReview::kPreviewSize.width());
    placeholder_->setFixedHeight(64);
    return placeholder_;
}

void ReviewRow::setNumber(int number) {
    step_->setText(QString::number(number));
    editor_->setAccessibleName(QStringLiteral("Section %1 transcript").arg(number));
}

void ReviewRow::showIncluded(bool on) {
    if (preview_ != nullptr) {
        preview_->setIncluded(on);
    }
}

ReviewBoard::ReviewBoard() {
    setObjectName(QStringLiteral("wlBody"));
    setAcceptDrops(true);
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(0, 0, 6, 12);
    column_->setSpacing(12);
    marker_ = new QFrame(this);
    marker_->setObjectName(QStringLiteral("wlDropMarker"));
    marker_->setFixedHeight(3);
    marker_->hide();
}

ReviewRow* ReviewBoard::dragged(QDropEvent* event) const {
    auto* grip = qobject_cast<ReviewGrip*>(event->source());
    if (event->mimeData()->hasFormat(WalkthroughReview::rowMime()) && grip != nullptr &&
        std::ranges::find(rows_, grip->row()) != rows_.end()) {
        return grip->row();
    }
    return nullptr;
}

int ReviewBoard::slot(double y) const {
    return static_cast<int>(std::ranges::count_if(
        rows_, [y](const ReviewRow* row) { return row->geometry().center().y() < y; }));
}

void ReviewBoard::dragEnterEvent(QDragEnterEvent* event) {
    if (dragged(event) != nullptr) {
        event->acceptProposedAction();
    }
}

void ReviewBoard::dragMoveEvent(QDragMoveEvent* event) {
    if (dragged(event) == nullptr) {
        return;
    }
    event->acceptProposedAction();
    const int at = slot(event->position().y());
    const double gap = column_->spacing() / 2.0;
    const double y = at < static_cast<int>(rows_.size())
                         ? rows_[static_cast<std::size_t>(at)]->geometry().top() - gap
                         : rows_.back()->geometry().bottom() + gap;
    marker_->setGeometry(0, pyRound(y) - 1, width() - 6, 3);
    marker_->show();
    marker_->raise();
}

void ReviewBoard::dragLeaveEvent(QDragLeaveEvent* /*event*/) {
    marker_->hide();
}

void ReviewBoard::dropEvent(QDropEvent* event) {
    marker_->hide();
    ReviewRow* row = dragged(event);
    if (row == nullptr) {
        return;
    }
    event->acceptProposedAction();
    const int start = static_cast<int>(std::ranges::find(rows_, row) - rows_.begin());
    const int at = slot(event->position().y());
    const int end = at > start ? at - 1 : at;
    if (end != start) {
        Q_EMIT moved(start, end);
    }
}

// ---- the review ----

// Where work off the UI thread hands its result back: the review while it lives. A worker that
// saw it alive posts under the lock, which the destructor takes before the review goes, so a
// result never reaches a deleted widget.
// A review destroyed while busy leaves its gate release here: the library stays held until the
// work it started has ended, then is released on the review's thread.
struct WalkthroughReview::Inbox {
    std::mutex mutex;
    WalkthroughReview* review = nullptr;
    QThread* thread = nullptr;
    std::function<void()> orphanRelease;

    static void deliver(const std::shared_ptr<Inbox>& inbox, std::optional<ReviewOutcome> outcome,
                        std::string error) {
        if (QThread::currentThread() == inbox->thread) {
            // On the review's own thread, as a Qt signal emitted there would be: at once.
            if (inbox->review != nullptr) {
                inbox->review->done(std::move(outcome), error);
            } else if (auto release = std::exchange(inbox->orphanRelease, {})) {
                release();
            }
            return;
        }
        std::lock_guard lock(inbox->mutex);
        if (inbox->review == nullptr) {
            if (auto release = std::exchange(inbox->orphanRelease, {})) {
                QMetaObject::invokeMethod(QCoreApplication::instance(), std::move(release),
                                          Qt::QueuedConnection);
            }
            return;
        }
        WalkthroughReview* review = inbox->review;
        QMetaObject::invokeMethod(
            review,
            [review, outcome = std::move(outcome), error = std::move(error)]() mutable {
                review->done(std::move(outcome), error);
            },
            Qt::QueuedConnection);
    }
};

void WalkthroughReview::Control::setEnabled(bool on) const {
    if (widget != nullptr) {
        widget->setEnabled(on);
    } else if (action != nullptr) {
        action->setEnabled(on);
    }
}

bool WalkthroughReview::Control::isEnabled() const {
    return widget != nullptr ? widget->isEnabled() : action != nullptr && action->isEnabled();
}

WalkthroughReview::WalkthroughReview(SettingsStore& settings, ReviewHost host, bool closable,
                                     QWidget* parent, Today today)
    : QWidget(parent), settings_(settings), today_(today ? std::move(today) : text::today),
      opener_([](const QUrl& url) { return QDesktopServices::openUrl(url); }),
      question_([](QWidget* parent_, const QString& title, const QString& body,
                   QMessageBox::StandardButtons buttons, QMessageBox::StandardButton fallback) {
          return QMessageBox::question(parent_, title, body, buttons, fallback);
      }),
      dialogRunner_([](QDialog& dialog) { return dialog.exec(); }),
      inbox_(std::make_shared<Inbox>()) {
    inbox_->review = this;
    inbox_->thread = thread();
    setObjectName(QStringLiteral("walkthroughReview"));
    ui::setLocalStyleSheet(this, reviewSheet());
    bind(std::move(host));
    // One button, moved into whichever draft's banner shows; parked on the widget in between so
    // a replaced body never takes it along.
    recoverButton_ =
        button(QStringLiteral("Recover draft"), "wlPrimary", [this] { recoverDraft(); });
    recoverButton_->hide();
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);
    column->addLayout(buildHeader());
    closeButton_->setVisible(closable);
    column->addWidget(buildActions());
    auto* status = new QHBoxLayout();
    notice_ = new QLabel(qs(text::SPOKEN_HINT));
    notice_->setObjectName(QStringLiteral("wlNotice"));
    notice_->setWordWrap(true);
    status->addWidget(notice_, 1);
    cancelButton_ = button(QStringLiteral("Cancel"), "wlQuiet", [this] { cancel(); });
    cancelButton_->hide();
    status->addWidget(cancelButton_);
    column->addLayout(status);
    column->addSpacing(6);
    scroll_ = new ReviewScrollArea();
    scroll_->setObjectName(QStringLiteral("wlScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    column->addWidget(scroll_, 1);
    column->addWidget(buildSaveBar());
    actions_ = {{saveButton_},
                {saveRedoButton_},
                {discardButton_},
                {pdfButton_},
                {markdownButton_},
                {copyWordButton_},
                {openButton_},
                {nullptr, openWordAction_},
                {nullptr, folderAction_},
                {nullptr, copyAgentAction_},
                {nullptr, retryAction_},
                {nullptr, styleAction_},
                {nullptr, promptAction_},
                {nullptr, deleteAction_},
                {recoverButton_},
                {closeButton_},
                {versionBar_},
                {versionPicker_}};
    showFolder(std::nullopt);
}

QString WalkthroughReview::rowMime() {
    static const QString mime = QStringLiteral("application/x-") +
                                qs(py::lower(identity().product)) +
                                QStringLiteral("-walkthrough-row");
    return mime;
}

WalkthroughReview::~WalkthroughReview() {
    std::lock_guard lock(inbox_->mutex);
    inbox_->review = nullptr;
    if (busy_) {
        // Nobody is left to show the result: stop the work, and release the gate when it ends.
        cancel_.cancel();
        inbox_->orphanRelease = host_.release;
    }
}

void WalkthroughReview::bind(ReviewHost host) {
    host_ = std::move(host);
}

void WalkthroughReview::setWriteUpRunner(walkthrough::write_up::AgentTaskRunner runner) {
    runner_ = std::move(runner);
}

void WalkthroughReview::setOpener(std::function<bool(const QUrl&)> opener) {
    opener_ = std::move(opener);
}

void WalkthroughReview::setQuestion(Question question) {
    question_ = std::move(question);
}

void WalkthroughReview::setDialogRunner(DialogRunner runner) {
    dialogRunner_ = std::move(runner);
}

// ---- building ----

QLayout* WalkthroughReview::buildHeader() {
    auto* head = new QHBoxLayout();
    head->setSpacing(10);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(2);
    title_ = new QLabel(QString());
    title_->setObjectName(QStringLiteral("wlTitle"));
    title_->setWordWrap(true);
    titles->addWidget(title_);
    auto* meta = new QHBoxLayout();
    meta->setSpacing(8);
    meta_ = new QLabel(QString());
    meta_->setObjectName(QStringLiteral("wlMeta"));
    meta->addWidget(meta_);
    kind_ = new QLabel(QString());
    kind_->setObjectName(QStringLiteral("wlPill"));
    meta->addWidget(kind_);
    // Once it has been written up more than once: which of them shows.
    versionPicker_ = new QToolButton();
    versionPicker_->setObjectName(QStringLiteral("wlVersionPicker"));
    versionPicker_->setCursor(Qt::PointingHandCursor);
    versionPicker_->setPopupMode(QToolButton::InstantPopup);
    versionPicker_->setAccessibleName(QStringLiteral("Which write-up shows"));
    versionPicker_->setToolTip(
        QStringLiteral("Each time it's written up again, the one before is kept here"));
    versionMenu_ = contextMenu(this);
    versionPicker_->setMenu(versionMenu_);
    meta->addWidget(versionPicker_);
    meta->addStretch(1);
    titles->addLayout(meta);
    head->addLayout(titles, 1);
    std::vector<std::pair<QString, QString>> choices;
    for (const auto& choice : text::VERSIONS) {
        choices.emplace_back(qs(choice.title), QString());
    }
    versionBar_ = new SegmentedBar(choices, true);
    for (std::size_t i = 0; i < std::size(text::VERSIONS); ++i) {
        versionBar_->buttons()[i]->setToolTip(qs(text::VERSIONS[i].tip));
    }
    // The bar asks for no width of its own beside the title; give it one row's worth.
    versionBar_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    versionBar_->setFixedWidth(kVersionBarWidth);
    versionBar_->setAccessibleName(QStringLiteral("Which version of the walkthrough shows"));
    connect(versionBar_, &SegmentedBar::activated, this,
            [this](int index) { chooseVersion(index); });
    head->addWidget(versionBar_, 0, Qt::AlignTop);
    closeButton_ = iconButton(QStringLiteral("x-gray.svg"), QStringLiteral("Close walkthrough"));
    closeButton_->setToolTip(
        QStringLiteral("Close. It stays under Walkthroughs in the side panel."));
    connect(closeButton_, &QToolButton::clicked, this, [this] { Q_EMIT closeRequested(); });
    head->addWidget(closeButton_, 0, Qt::AlignTop);
    return head;
}

QWidget* WalkthroughReview::buildActions() {
    pdfButton_ = button(QStringLiteral("Copy PDF"), "wlPrimary", [this] { copy("pdf"); });
    setCopyTip(pdfButton_, "pdf");
    markdownButton_ =
        button(QStringLiteral("Copy Markdown"), "wlSecondary", [this] { copy("agent"); });
    markdownButton_->setIcon(ui::designIcon(QStringLiteral("copy-gray.svg")));
    setCopyTip(markdownButton_, "agent");
    copyWordButton_ = button(QStringLiteral("Copy Word"), "wlSecondary", [this] { copy("word"); });
    setCopyTip(copyWordButton_, "word");
    chip_ = new FileChip(this);
    connect(chip_, &FileChip::clicked, this, [this] { open(); });
    openButton_ = button(QStringLiteral("Open PDF"), "wlSecondary", [this] { open(); });
    openButton_->setIcon(ui::designIcon(QStringLiteral("external-link-gray.svg")));
    moreButton_ = iconButton(QStringLiteral("dots-gray.svg"), QStringLiteral("More"));
    moreButton_->setPopupMode(QToolButton::InstantPopup);
    menu_ = contextMenu(this);
    menu_->setToolTipsVisible(true);
    copyAgentAction_ = menuAction(QStringLiteral("Copy for agent"), [this] { copy("agent_task"); });
    setCopyTip(copyAgentAction_, "agent_task");
    menu_->addSeparator();
    openWordAction_ = menuAction(QStringLiteral("Open Word document"), [this] { openWord(); });
    folderAction_ = menuAction(QStringLiteral("Show in folder"), [this] { reveal(); });
    menu_->addSeparator();
    retryAction_ = menuAction(QStringLiteral("Redo AI write-up"), [this] { retry(); });
    // Filled as it opens, so styles saved anywhere since show.
    styleMenu_ = contextMenu(menu_);
    connect(styleMenu_, &QMenu::aboutToShow, this, [this] { fillStyleMenu(); });
    styleAction_ = menu_->addMenu(styleMenu_);
    promptAction_ =
        menuAction(QStringLiteral(u"Redo with a new prompt…"), [this] { retryWithPrompt(); });
    menu_->addSeparator();
    deleteAction_ = menuAction(QStringLiteral(u"Delete…"), [this] { deleteFolder(); });
    moreButton_->setMenu(menu_);
    copyGroup_ = new CopyGroup({pdfButton_, markdownButton_, copyWordButton_, chip_});
    actionRow_ = new Reflow(copyGroup_, actionGroup({openButton_, moreButton_}));
    return actionRow_;
}

QFrame* WalkthroughReview::buildSaveBar() {
    saveBar_ = new QFrame();
    saveBar_->setObjectName(QStringLiteral("wlSaveBar"));
    auto* row = new QHBoxLayout(saveBar_);
    row->setContentsMargins(16, 8, 8, 8);
    saveText_ = new QLabel(QString());
    saveText_->setObjectName(QStringLiteral("wlSaveText"));
    saveText_->setWordWrap(true);
    row->addWidget(saveText_, 1);
    discardButton_ = button(QStringLiteral("Discard"), "wlQuiet", [this] { discard(); });
    row->addWidget(discardButton_);
    saveButton_ = button(QStringLiteral("Save changes"), "wlPrimary", [this] { save(false); });
    row->addWidget(saveButton_);
    saveRedoButton_ =
        button(QStringLiteral("Save and redo write-up"), "wlPrimary", [this] { save(true); });
    row->addWidget(saveRedoButton_);
    saveBar_->hide();
    return saveBar_;
}

QSize WalkthroughReview::minimumSizeHint() const {
    // The row of actions ignores its own width so Open can drop below Copy and the copy buttons
    // can wrap, but no one of them ever squashes.
    return QWidget::minimumSizeHint().expandedTo(QSize(copyGroup_->minimumSizeHint().width(), 0));
}

QPushButton* WalkthroughReview::button(const QString& label, const char* name,
                                       std::function<void()> slot) {
    auto* made = new QPushButton(label);
    made->setObjectName(QString::fromLatin1(name));
    made->setCursor(Qt::PointingHandCursor);
    // Enter belongs to the editors, never to whichever button is first.
    made->setAutoDefault(false);
    connect(made, &QPushButton::clicked, this, [slot = std::move(slot)] { slot(); });
    return made;
}

QToolButton* WalkthroughReview::iconButton(const QString& icon, const QString& name) {
    auto* made = new QToolButton();
    made->setObjectName(QStringLiteral("wlIconButton"));
    made->setIcon(ui::designIcon(icon));
    made->setIconSize(QSize(18, 18));
    made->setCursor(Qt::PointingHandCursor);
    made->setAccessibleName(name);
    made->setToolTip(name);
    return made;
}

QAction* WalkthroughReview::menuAction(const QString& label, std::function<void()> slot) {
    QAction* action = menu_->addAction(label);
    connect(action, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    return action;
}

// ---- the walkthrough ----

std::vector<ReviewRow*> WalkthroughReview::rows() const {
    return board_ != nullptr ? board_->rows() : std::vector<ReviewRow*>{};
}

bool WalkthroughReview::discardChanges() {
    return !dirty_ || ask(QStringLiteral("Unsaved review"),
                          QStringLiteral("Discard your unsaved review changes?"));
}

bool WalkthroughReview::ask(const QString& title, const QString& body) {
    return question_(this, title, body, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
           QMessageBox::Yes;
}

bool WalkthroughReview::showFolder(const std::optional<std::filesystem::path>& folder,
                                   std::optional<bool> spoken, std::optional<int> version) {
    const auto then = std::exchange(next_, std::nullopt);
    if (then && !spoken && !version && folder && *folder == then->first) {
        // Saved here: stay on the version that was saved.
        spoken = then->second.spoken;
        version = then->second.version;
    }
    setDirty(false);
    folder_ = folder && !folder->empty() ? folder : std::nullopt;
    written_ = folder_ && walkthrough::written_up(*folder_);
    spoken_ = !written_ || spoken.value_or(false);
    earlier_ = written_ ? walkthrough::earlier_write_ups(*folder_) : std::vector<int>{};
    version_ = version && std::ranges::find(earlier_, *version) != earlier_.end() && !spoken_
                   ? version
                   : std::nullopt;
    recoverButton_->hide();
    recoverButton_->setParent(this);
    auto* board = new ReviewBoard();
    connect(board, &ReviewBoard::moved, this, [this](int start, int end) { moveRow(start, end); });
    bool ready = false;
    bool draft = false;
    std::optional<walkthrough::Walkthrough> document;
    if (folder_) {
        std::error_code error;
        draft = std::filesystem::exists(named(*folder_, walkthrough::DRAFT_NAME), error);
        if (draft) {
            board->column()->addWidget(draftBanner());
        }
        try {
            document = walkthrough::load_document(*folder_);
        } catch (const std::exception& failure) {
            if (!draft) {
                board->column()->addWidget(emptyBody(
                    QStringLiteral("This walkthrough can't be opened"), qs(failure.what())));
            }
        }
        if (document) {
            std::optional<std::vector<ReviewRow::Content>> contents;
            try {
                contents = spoken_ ? spokenRows(*document) : writeUpRows();
            } catch (const std::exception& failure) {
                board->column()->addWidget(emptyBody(
                    QStringLiteral("This walkthrough can't be opened"), qs(failure.what())));
            }
            if (contents) {
                ready = true;
                for (const auto& content : *contents) {
                    addRow(board, new ReviewRow(content, opener_));
                }
            }
        }
    } else {
        board->column()->addWidget(emptyBody(
            QStringLiteral("No walkthroughs yet"),
            QStringLiteral("Start one from the Walkthrough tab or the tray. Each one you finish is "
                           "kept here, ready to review and copy again.")));
    }
    board->column()->addStretch(1);
    board_ = board;
    if (QWidget* old = scroll_->takeWidget()) {
        old->deleteLater();
    }
    scroll_->setWidget(board);
    showHeader(document ? &*document : nullptr, draft);
    showActions(ready, draft);
    return ready;
}

std::vector<ReviewRow::Content>
WalkthroughReview::spokenRows(const walkthrough::Walkthrough& document) const {
    std::vector<ReviewRow::Content> contents;
    int index = 0;
    for (const auto& section : document.sections) {
        ReviewRow::Content content;
        content.index = index;
        content.number = index + 1;
        content.text = qs(section.text);
        content.image = section.shot ? section.shot : section.omitted;
        if (section.kept_at_s) {
            const QString at = qs(walkthrough::clock(*section.kept_at_s));
            content.stamp = content.image ? QStringLiteral("Screenshot at ") + at
                                          : QStringLiteral("Until ") + at;
        }
        content.included = section.shot.has_value();
        content.canOmit = true;
        content.notes = section.notes;
        contents.push_back(std::move(content));
        ++index;
    }
    return contents;
}

std::vector<ReviewRow::Content> WalkthroughReview::writeUpRows() const {
    const auto name = walkthrough::write_up_names(version_)[0];
    const std::string written = readText(walkthrough::local_file(*folder_, name));
    std::vector<ReviewRow::Content> contents;
    int index = 0;
    for (const auto& [words, line] : walkthrough::write_up_parts(written)) {
        ReviewRow::Content content;
        content.index = index;
        content.number = index + 1;
        content.text = qs(words);
        if (!line.empty()) {
            content.image = walkthrough::write_up_shot(*folder_, line);
        }
        content.included = true;
        content.canOmit = false;
        content.line = line;
        contents.push_back(std::move(content));
        ++index;
    }
    return contents;
}

void WalkthroughReview::addRow(ReviewBoard* board, ReviewRow* row) {
    board->rows().push_back(row);
    board->column()->addWidget(row);
    connect(row->editor(), &QTextEdit::textChanged, this, [this] { edited(); });
    connect(row->editor(), &ReviewEditor::nudged, this,
            [this, row](int step) { nudge(row, step); });
    connect(row->check(), &QAbstractButton::toggled, row, &ReviewRow::showIncluded);
    connect(row->check(), &QAbstractButton::toggled, this, [this] { edited(); });
    if (row->preview() != nullptr) {
        connect(row->preview(), &ReviewPreview::copyRequested, this,
                [this](const QString& path) { copyScreenshot(path); });
    }
}

void WalkthroughReview::showHeader(const walkthrough::Walkthrough* document, bool draft) {
    text::HeaderInput input;
    input.folder = folder_;
    if (folder_) {
        input.started = text::started(*folder_, document);
    }
    if (document != nullptr) {
        std::vector<bool> sections;
        for (const auto& section : document->sections) {
            sections.push_back(section.shot.has_value());
        }
        input.sections = std::move(sections);
        input.duration_s = document->duration_s;
    }
    input.draft = draft;
    input.written = written_;
    input.spoken = spoken_;
    const auto shown = text::header(input);
    title_->setText(qs(shown.title));
    meta_->setText(qs(shown.meta));
    kind_->setText(qs(shown.pill));
    ui::setStyleProperty(kind_, ui::prop::tone, qs(shown.pill_tone));
    kind_->setVisible(shown.pill_shown);
    versionBar_->setVisible(shown.version_bar_shown);
    versionBar_->setCurrentIndex(shown.version_bar_index);
    showVersions(document != nullptr);
    retryAction_->setText(qs(shown.retry));
    styleMenu_->setTitle(qs(shown.style_menu));
    promptAction_->setText(qs(shown.prompt));
    title_->setVisible(shown.title_shown);
    if (!busy_) {
        notice_->setText(qs(shown.notice));
    }
}

void WalkthroughReview::showVersions(bool loaded) {
    versionMenu_->clear();
    delete std::exchange(versionGroup_, nullptr);
    const auto folder = folder_;
    const auto today = today_();
    const auto shown = text::versions(
        earlier_, version_, spoken_, loaded, [&folder, &today](std::optional<int> version) {
            if (!folder) {
                return std::string();
            }
            const auto name = walkthrough::write_up_names(version)[0];
            return text::written_at(text::modified_at(named(*folder, name)), today);
        });
    versionPicker_->setVisible(shown.shown);
    if (!shown.shown) {
        return;
    }
    versionPicker_->setText(qs(shown.text));
    versionGroup_ = new QActionGroup(versionMenu_);
    for (const auto& row : shown.rows) {
        QAction* action = versionMenu_->addAction(qs(row.label));
        action->setCheckable(true);
        action->setChecked(row.checked);
        versionGroup_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, chosen = row.version] { chooseWriteUp(chosen); });
    }
}

void WalkthroughReview::showActions(bool ready, bool draft) {
    // Work off the UI thread turned everything off; what follows turns back off only what this
    // walkthrough can't use.
    for (const auto& control : actions_) {
        control.setEnabled(true);
    }
    for (QPushButton* each :
         {saveButton_, discardButton_, pdfButton_, markdownButton_, openButton_}) {
        each->setEnabled(ready);
    }
    for (QAction* each : {copyAgentAction_, retryAction_, styleAction_, promptAction_}) {
        each->setEnabled(ready);
    }
    // A draft has no document yet: nothing to copy, open or edit.
    for (QWidget* each : std::initializer_list<QWidget*>{pdfButton_, markdownButton_,
                                                         copyWordButton_, chip_, openButton_}) {
        each->setVisible(ready);
    }
    if (!ready && !busy_) {
        // No editing hint, but the line stays to say what Recover and Delete are doing or why
        // they can't.
        notice_->setText(QString());
    }
    // Walkthroughs from before Word documents have none until Review saves them.
    std::error_code error;
    const bool word = ready && std::filesystem::is_regular_file(resultDocx(), error);
    copyWordButton_->setEnabled(word);
    openWordAction_->setEnabled(word);
    if (ready) {
        chip_->setPath(shownPath(resultPdf()));
    }
    recoverButton_->setEnabled(
        draft && host_.recover && folder_ &&
        std::filesystem::is_regular_file(named(*folder_, walkthrough::AUDIO_NAME), error));
    folderAction_->setEnabled(folder_.has_value());
    deleteAction_->setEnabled(folder_.has_value());
    moreButton_->setVisible(folder_.has_value());
}

std::filesystem::path WalkthroughReview::resultPdf() const {
    if (spoken_) {
        return named(*folder_, walkthrough::PDF_NAME);
    }
    return named(*folder_, walkthrough::write_up_names(version_)[1]);
}

std::filesystem::path WalkthroughReview::resultDocx() const {
    return std::filesystem::path(resultPdf()).replace_extension(".docx");
}

QFrame* WalkthroughReview::draftBanner() {
    auto* banner = new QFrame();
    banner->setObjectName(QStringLiteral("wlBanner"));
    auto* row = new QHBoxLayout(banner);
    row->setContentsMargins(16, 14, 14, 14);
    row->setSpacing(12);
    auto* words = new QVBoxLayout();
    words->setSpacing(3);
    auto* title = new QLabel(QStringLiteral("This walkthrough was interrupted"));
    title->setObjectName(QStringLiteral("wlBannerTitle"));
    words->addWidget(title);
    std::error_code error;
    const bool hasAudio =
        std::filesystem::is_regular_file(named(*folder_, walkthrough::AUDIO_NAME), error);
    auto* body = new QLabel(
        hasAudio ? QStringLiteral("Its audio and screenshots were saved. Recover it to transcribe "
                                  "them into a document.")
                 : QStringLiteral("Its screenshots are kept. The audio hasn't been recovered yet; "
                                  "restarting %1 finishes that.")
                       .arg(qs(identity().product)));
    body->setObjectName(QStringLiteral("wlBannerBody"));
    body->setWordWrap(true);
    words->addWidget(body);
    row->addLayout(words, 1);
    row->addWidget(recoverButton_, 0, Qt::AlignVCenter);
    recoverButton_->show();
    return banner;
}

QWidget* WalkthroughReview::emptyBody(const QString& heading, const QString& body) {
    auto* widget = new QWidget();
    auto* column = new QVBoxLayout(widget);
    column->setContentsMargins(0, 60, 0, 0);
    column->setSpacing(6);
    auto* title = new QLabel(heading);
    title->setObjectName(QStringLiteral("wlEmptyTitle"));
    title->setAlignment(Qt::AlignHCenter);
    column->addWidget(title);
    auto* words = new QLabel(body);
    words->setObjectName(QStringLiteral("wlEmptyBody"));
    words->setWordWrap(true);
    words->setAlignment(Qt::AlignHCenter);
    column->addWidget(words);
    column->addStretch(1);
    return widget;
}

// ---- editing ----

void WalkthroughReview::edited() {
    setDirty(true);
}

void WalkthroughReview::setDirty(bool dirty) {
    dirty_ = dirty;
    // The write-ups came from the old words: they stay, and redoing adds one.
    const bool redo = dirty && spoken_ && written_;
    saveText_->setText(redo ? QStringLiteral("Saving rebuilds the PDF and Word document. Your AI "
                                             "write-ups stay as they are; redo it to write up the "
                                             "new words as well.")
                            : QStringLiteral("You have unsaved edits. Saving rebuilds the PDF and "
                                             "Word document."));
    saveButton_->setObjectName(redo ? QStringLiteral("wlSecondary") : QStringLiteral("wlPrimary"));
    ui::repolish(saveButton_);
    saveRedoButton_->setVisible(redo);
    saveBar_->setVisible(dirty);
}

void WalkthroughReview::discard() {
    showFolder(folder_, spoken_, version_);
}

void WalkthroughReview::moveRow(int start, int end) {
    auto& rows = board_->rows();
    // Rows sit together after any banner, from the first row's place on.
    const int base = board_->column()->indexOf(rows.front());
    ReviewRow* row = rows[static_cast<std::size_t>(start)];
    rows.erase(rows.begin() + start);
    rows.insert(rows.begin() + end, row);
    board_->column()->removeWidget(row);
    board_->column()->insertWidget(base + end, row);
    int number = 1;
    for (ReviewRow* each : rows) {
        each->setNumber(number++);
    }
    edited();
}

void WalkthroughReview::nudge(ReviewRow* row, int step) {
    const auto& rows = board_->rows();
    const int start = static_cast<int>(std::ranges::find(rows, row) - rows.begin());
    const int end = start + step;
    if (end >= 0 && end < static_cast<int>(rows.size())) {
        moveRow(start, end);
        row->editor()->setFocus();
        scroll_->ensureWidgetVisible(row);
    }
}

void WalkthroughReview::chooseVersion(int index) {
    const bool spoken = index == 1;
    if (spoken == spoken_) {
        return;
    }
    if (!discardChanges()) {
        versionBar_->setCurrentIndex(spoken_ ? 1 : 0);
        return;
    }
    showFolder(folder_, spoken);
}

void WalkthroughReview::chooseWriteUp(std::optional<int> version) {
    if (version == version_ && !spoken_) {
        return;
    }
    if (!discardChanges()) {
        showVersions(true);
        return;
    }
    showFolder(folder_, false, version);
}

// ---- work off the UI thread ----

void WalkthroughReview::run(ReviewOperation operation, std::optional<ReviewView> then,
                            const std::optional<QString>& done_) {
    if (busy_) {
        return;
    }
    doneText_ = done_.value_or(qs(text::SAVED));
    then_ = then && folder_ ? std::optional(std::pair{*folder_, *then}) : std::nullopt;
    if (host_.acquire) {
        if (const auto error = host_.acquire(); error && !error->isEmpty()) {
            notice_->setText(*error);
            return;
        }
    }
    busy_ = true;
    Q_EMIT busyChanged(true);
    enabled_.clear();
    for (const auto& control : actions_) {
        enabled_.push_back(control.isEnabled());
    }
    cancel_ = CancelSource();
    scroll_->setEnabled(false);
    for (const auto& control : actions_) {
        control.setEnabled(false);
    }
    cancelButton_->show();
    notice_->setText(
        QStringLiteral(u"Working… Your saved documents stay as they are if this fails."));
    Task work = [operation = std::move(operation), token = cancel_.token(), inbox = inbox_] {
        // The library blocks on disk and on the agent: never on the GUI thread.
        assert_not_gui_thread();
        std::optional<ReviewOutcome> outcome;
        std::string error;
        try {
            if (token.cancelled()) {
                throw std::runtime_error("canceled");
            }
            outcome = operation(token);
        } catch (const std::exception& failure) {
            if (!token.cancelled()) {
                kLog.warning("Walkthrough review operation failed: {}", failure.what());
            }
            error = failure.what();
        } catch (...) {
            // Python's `except Exception` catches every failure; one that escaped here would end
            // the process with the gate still taken.
            kLog.warning("Walkthrough review operation failed with an unknown error");
            error = "unknown error";
        }
        Inbox::deliver(inbox, std::move(outcome), std::move(error));
    };
    try {
        bool accepted = true;
        if (host_.submit) {
            accepted = host_.submit(std::move(work));
        } else {
            std::thread(std::move(work)).detach();
        }
        if (!accepted) {
            // What Python's executor says once it has shut down.
            throw std::runtime_error("cannot schedule new futures after shutdown");
        }
    } catch (const std::exception& failure) {
        kLog.warning("Could not start the walkthrough review operation: {}", failure.what());
        done(std::nullopt, failure.what());
    } catch (...) {
        kLog.warning("Could not start the walkthrough review operation");
        done(std::nullopt, "unknown error");
    }
}

void WalkthroughReview::done(std::optional<ReviewOutcome> outcome, const std::string& error) {
    busy_ = false;
    if (host_.release) {
        host_.release();
    }
    scroll_->setEnabled(true);
    cancelButton_->hide();
    Q_EMIT busyChanged(false);
    if (!error.empty()) {
        then_.reset();
        // Preserve unsaved editor contents after a failed regeneration.
        for (std::size_t i = 0; i < actions_.size() && i < enabled_.size(); ++i) {
            actions_[i].setEnabled(enabled_[i]);
        }
        notice_->setText(QStringLiteral("Couldn't finish: ") + qs(error));
        return;
    }
    QString shownDone = doneText_;
    std::optional<walkthrough::WalkthroughResult> result;
    if (outcome) {
        result = std::move(outcome->result);
        if (outcome->savedOnly) {
            // The save is on disk: reload it so a second save can't apply the same reordering
            // twice, and say the write-up is as it was.
            shownDone =
                QStringLiteral("Saved, but the write-up wasn't redone: ") + qs(*outcome->savedOnly);
            if (folder_) {
                then_ = std::pair{*folder_, ReviewView{true, std::nullopt}};
            }
        }
    }
    const std::optional<std::filesystem::path> folder =
        result ? std::optional(result->folder) : folder_;
    std::error_code ignored;
    const bool there = folder && std::filesystem::is_directory(*folder, ignored);
    next_ = std::exchange(then_, std::nullopt);
    setDirty(false);
    // The host shows what to see next: this folder, or another once it's gone.
    Q_EMIT changed(there ? qpath(*folder) : QString());
    notice_->setText(shownDone);
}

void WalkthroughReview::cancel() {
    if (busy_) {
        cancel_.cancel();
        notice_->setText(QStringLiteral(u"Stopping safely…"));
    }
}

// ---- actions ----

Json WalkthroughReview::settingsSnapshot() const {
    return *settings_.snapshot();
}

void WalkthroughReview::save(bool redo) {
    if (!folder_) {
        return;
    }
    const auto folder = *folder_;
    const Json settings = settingsSnapshot();
    if (!spoken_) {
        std::vector<std::pair<std::string, std::string>> parts;
        for (ReviewRow* row : rows()) {
            parts.emplace_back(utf8(row->editor()->toPlainText()), row->line());
        }
        const auto version = version_;
        run(
            [folder, parts = std::move(parts), version, settings](const CancelToken&) {
                return ReviewOutcome{walkthrough::save_write_up(folder,
                                                                walkthrough::join_write_up(parts),
                                                                version, settings),
                                     std::nullopt};
            },
            ReviewView{false, version});
        return;
    }
    std::vector<std::string> texts;
    std::vector<bool> keep;
    std::vector<int> order;
    for (ReviewRow* row : rows()) {
        texts.push_back(utf8(row->editor()->toPlainText()));
        keep.push_back(row->check()->isChecked());
        order.push_back(row->index());
    }
    auto save_ = [folder, texts = std::move(texts), keep = std::move(keep),
                  order = std::move(order), settings, redo,
                  runner = runner_](const CancelToken& cancel) -> ReviewOutcome {
        auto result = walkthrough::save_review(folder, texts, keep, order, settings);
        if (redo) {
            try {
                return {walkthrough::retry_write_up(folder, std::nullopt, settings, cancel, runner),
                        std::nullopt};
            } catch (const std::exception& failure) {
                if (!cancel.cancelled()) {
                    kLog.warning("Walkthrough write-up redo failed after saving: {}",
                                 failure.what());
                }
                return {std::move(result), std::string(failure.what())};
            }
        }
        return {std::move(result), std::nullopt};
    };
    // Redone, the new write-up shows; otherwise the words just saved stay.
    run(std::move(save_), redo ? std::nullopt : std::optional(ReviewView{true, std::nullopt}));
}

void WalkthroughReview::copy(std::string_view format) {
    if (busy_ || !folder_ || !host_.copyFiles) {
        return;
    }
    if (dirty_) {
        notice_->setText(QStringLiteral("Save your changes before copying."));
        return;
    }
    try {
        const auto result = walkthrough::load_result(*folder_, spoken_, version_);
        const auto [files, clipboard] = result.clipboard(format);
        if (host_.copyFiles(files, qs(clipboard), QImage())) {
            notice_->setText(qs(text::copied(format).value_or("")));
        } else {
            notice_->setText(
                QStringLiteral("Copying failed. Drag the file instead, or show it in its folder."));
        }
    } catch (const std::exception& failure) {
        notice_->setText(qs(failure.what()));
    }
}

void WalkthroughReview::copyScreenshot(const QString& path) {
    // Screenshots are never rewritten by a save, so unsaved edits don't hold this up.
    const QImage image(path);
    if (image.isNull()) {
        notice_->setText(
            QStringLiteral("Couldn't read %1.").arg(qpath(path_from_utf8(utf8(path)).filename())));
        return;
    }
    bool ok = true;
    if (host_.copyFiles) {
        ok = host_.copyFiles({path_from_utf8(utf8(path))}, path, image);
    } else {
        auto* mime = new QMimeData();
        mime->setUrls({QUrl::fromLocalFile(path)});
        mime->setText(path);
        mime->setImageData(image);
        QApplication::clipboard()->setMimeData(mime);
    }
    notice_->setText(ok ? QStringLiteral("Screenshot copied.")
                        : QStringLiteral("Couldn't copy the screenshot."));
}

void WalkthroughReview::retry() {
    redo(std::nullopt);
}

void WalkthroughReview::redo(std::optional<std::string> instructions) {
    if (folder_ && !dirty_) {
        run([folder = *folder_, instructions = std::move(instructions),
             settings = settingsSnapshot(), runner = runner_](const CancelToken& cancel) {
            return ReviewOutcome{
                walkthrough::retry_write_up(folder, instructions, settings, cancel, runner),
                std::nullopt};
        });
    } else if (dirty_) {
        notice_->setText(QStringLiteral("Save your changes before writing it up."));
    }
}

void WalkthroughReview::fillStyleMenu() {
    styleMenu_->clear();
    for (const auto& style : walkthrough::load_styles(settingsSnapshot())) {
        QAction* action = styleMenu_->addAction(qs(style.name));
        action->setToolTip(qs(style.instructions));
        connect(action, &QAction::triggered, this,
                [this, words = style.instructions] { redo(words); });
    }
    styleMenu_->setToolTipsVisible(true);
}

void WalkthroughReview::retryWithPrompt() {
    if (dirty_) {
        notice_->setText(QStringLiteral("Save your changes before writing it up."));
        return;
    }
    if (!folder_) {
        return;
    }
    std::string prompt = settings_.get(keys::walkthrough_write_up_instructions);
    if (prompt.empty()) {
        prompt = std::string(walkthrough::write_up::DEFAULT_INSTRUCTIONS);
    }
    WriteUpPromptDialog dialog(settings_, qs(prompt), written_, this);
    if (dialogRunner_(dialog) != 0) {
        redo(utf8(dialog.text()));
    }
}

void WalkthroughReview::recoverDraft() {
    if (folder_ && host_.recover) {
        host_.recover(shownPath(*folder_));
        // One click starts it; a second would only be refused.
        recoverButton_->setEnabled(false);
        notice_->setText(QStringLiteral("Recovering. It appears here once it's transcribed."));
    }
}

void WalkthroughReview::open() {
    if (folder_) {
        opener_(QUrl::fromLocalFile(shownPath(resultPdf())));
    }
}

void WalkthroughReview::openWord() {
    if (folder_) {
        opener_(QUrl::fromLocalFile(shownPath(resultDocx())));
    }
}

void WalkthroughReview::reveal() {
    if (folder_) {
        revealInFileManager(shownPath(resultPdf()));
    }
}

void WalkthroughReview::deleteFolder() {
    if (folder_ && ask(QStringLiteral("Delete walkthrough"),
                       QStringLiteral("Permanently delete this walkthrough, its audio and "
                                      "screenshots?"))) {
        run(
            [folder = *folder_](const CancelToken&) {
                walkthrough::delete_entry(folder);
                return ReviewOutcome{};
            },
            std::nullopt, QStringLiteral("Deleted."));
    }
}

} // namespace fv
