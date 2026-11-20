#include "ui_widgets/field_help.h"

#include <algorithm>

#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QScreen>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"

namespace fv {
namespace {

constexpr Qt::WindowFlags kCardFlags =
    Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;

constexpr int kShowDelayMs = 450;
constexpr int kDismissPollMs = 200;
// Pointer slop around the card before it counts as outside.
constexpr int kCardSlop = 8;

const char* const kCaptionSheet = "QPushButton { background: transparent; border: none; "
                                  "padding: 0; }";
const char* const kCardSheet =
    "QFrame#fieldHelpCard { background: transparent; border: none; }"
    "QLabel { color: @text-secondary; background: transparent; border: none; }"
    "QLabel#fieldHelpTitle { color: @text; font-weight: 600; }"
    "QPushButton { color: @accent; background: transparent; border: none; "
    "text-align: left; padding: 4px 0 0 0; }"
    "QPushButton:hover, QPushButton:focus { text-decoration: underline; }";

} // namespace

// Named as OpenWhisper names it, so the widget tree reads the same.
namespace field_help_detail {

// Rounded popover painted against a translucent window. QSS border-radius on an opaque tool
// window leaves square backing blocks; translucency without a paintEvent leaves the card
// invisible, which made the engine-field hints unreadable over the recording buttons.
class HelpCard : public QFrame {
    Q_OBJECT
public:
    static constexpr double kRadius = 10;

    explicit HelpCard(QWidget* parent) : QFrame(parent, kCardFlags) {
        setObjectName(QStringLiteral("fieldHelpCard"));
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_StyledBackground, true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(ui::tokenColor("border-hover"), 1.0));
        painter.setBrush(ui::tokenColor("surface-hover"));
        painter.drawRoundedRect(bounds, kRadius, kRadius);
    }
};

} // namespace field_help_detail

using field_help_detail::HelpCard;

FieldHelp::FieldHelp(const QString& caption, const QString& title, const QString& description,
                     const std::vector<std::pair<QString, QString>>& links, QWidget* parent)
    : QPushButton(parent), caption_(caption) {
    setAccessibleName(QStringLiteral("Help for %1").arg(caption));
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setAutoDefault(false);
    setDefault(false);
    setFlat(true);
    setMinimumSize(0, 0);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    ui::setLocalStyleSheet(this, QString::fromUtf8(kCaptionSheet));
    card_ = new HelpCard(this);
    ui::setLocalStyleSheet(card_, QString::fromUtf8(kCardSheet));
    auto* layout = new QVBoxLayout(card_);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("fieldHelpTitle"));
    layout->addWidget(titleLabel);
    body_ = new QLabel(description);
    body_->setWordWrap(true);
    layout->addWidget(body_);
    for (const auto& [text, destination] : links) {
        auto* button = new QPushButton(text);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this,
                [this, destination = destination] { navigate(destination); });
        layout->addWidget(button);
        links_.push_back(button);
        button->installEventFilter(this);
    }
    card_->installEventFilter(this);
    card_->hide();
    showTimer_ = new QTimer(this);
    showTimer_->setSingleShot(true);
    // A coarse timer may fire up to 5 % early (449 ms seen); the card must not open before
    // 450 ms of hover.
    showTimer_->setTimerType(Qt::PreciseTimer);
    showTimer_->setInterval(kShowDelayMs);
    connect(showTimer_, &QTimer::timeout, this, &FieldHelp::showHelp);
    dismissTimer_ = new QTimer(this);
    dismissTimer_->setInterval(kDismissPollMs);
    connect(dismissTimer_, &QTimer::timeout, this, &FieldHelp::dismissIfOutside);
    connect(this, &QPushButton::clicked, this, &FieldHelp::openForKeyboard);
}

FieldHelp::~FieldHelp() {
    delete card_;
}

void FieldHelp::showHelp() {
    if (!isVisible() || card_ == nullptr) {
        return;
    }
    hostCard();
    sizeCard();
    const QPoint point = cardPosition();
    card_->move(point);
    card_->show();
    card_->raise();
    // Windows can ignore the first move() until the native window exists.
    card_->move(point);
    dismissTimer_->start();
}

void FieldHelp::hostCard() {
    QWidget* host = window();
    if (card_->parent() == host && card_->isWindow()) {
        return;
    }
    card_->setParent(host, kCardFlags);
    host->installEventFilter(this);
    card_->setAttribute(Qt::WA_ShowWithoutActivating, true);
    card_->setAttribute(Qt::WA_TranslucentBackground, true);
    card_->setAttribute(Qt::WA_StyledBackground, true);
}

void FieldHelp::sizeCard() {
    const QRect screen = this->screen()->availableGeometry();
    const int width = std::min(ui::scaled(360), std::max(160, screen.width()));
    const int inner = std::max(80, width - 28);
    body_->setFixedWidth(inner);
    for (QPushButton* link : links_) {
        link->setMaximumWidth(inner);
    }
    card_->setFixedWidth(width);
    card_->adjustSize();
}

QPoint FieldHelp::cardPosition() const {
    QPoint point = mapToGlobal(QPoint(0, height() + 4));
    const QRect screen = this->screen()->availableGeometry();
    point.setX(std::max(screen.left(), std::min(point.x(), screen.right() - card_->width() + 1)));
    if (point.y() + card_->height() > screen.bottom()) {
        point.setY(mapToGlobal(QPoint(0, 0)).y() - card_->height() - 4);
    }
    point.setY(std::max(screen.top(), point.y()));
    return point;
}

void FieldHelp::openForKeyboard() {
    showHelp();
    if (card_ == nullptr) {
        return;
    }
    card_->activateWindow();
    if (!links_.empty()) {
        links_.front()->setFocus();
    }
}

void FieldHelp::closeCard() {
    if (card_ != nullptr) {
        card_->hide();
    }
    dismissTimer_->stop();
}

void FieldHelp::navigate(const QString& destination) {
    closeCard();
    Q_EMIT destinationRequested(destination);
}

void FieldHelp::dismissIfOutside() {
    if (card_ == nullptr) {
        return;
    }
    const QPoint point = QCursor::pos();
    const bool overCaption = rect().contains(mapFromGlobal(point));
    const bool overCard = card_->rect()
                              .adjusted(-kCardSlop, -kCardSlop, kCardSlop, kCardSlop)
                              .contains(card_->mapFromGlobal(point));
    if (!overCaption && !overCard && !card_->isActiveWindow()) {
        closeCard();
    }
}

QSize FieldHelp::sizeHint() const {
    const QFontMetrics metrics = fontMetrics();
    return {metrics.horizontalAdvance(caption_), metrics.height()};
}

QSize FieldHelp::minimumSizeHint() const {
    return sizeHint();
}

void FieldHelp::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const bool active = underMouse() || hasFocus() || isDown();
    const QColor color = ui::tokenColor(active ? "accent" : "text-secondary");
    const QFontMetrics metrics = fontMetrics();
    const QRectF bounds(rect());
    const int textWidth = metrics.horizontalAdvance(caption_);
    painter.setPen(color);
    painter.drawText(QRectF(bounds.left(), bounds.top(), textWidth, bounds.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, caption_);
}

void FieldHelp::enterEvent(QEnterEvent* event) {
    showTimer_->start();
    update();
    QPushButton::enterEvent(event);
}

void FieldHelp::leaveEvent(QEvent* event) {
    showTimer_->stop();
    update();
    QPushButton::leaveEvent(event);
}

void FieldHelp::focusInEvent(QFocusEvent* event) {
    update();
    QPushButton::focusInEvent(event);
}

void FieldHelp::focusOutEvent(QFocusEvent* event) {
    update();
    QPushButton::focusOutEvent(event);
}

void FieldHelp::hideEvent(QHideEvent* event) {
    showTimer_->stop();
    closeCard();
    QPushButton::hideEvent(event);
}

bool FieldHelp::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        closeCard();
        setFocus();
        return true;
    }
    if (watched == card_ && event->type() == QEvent::WindowDeactivate) {
        closeCard();
    }
    return QPushButton::eventFilter(watched, event);
}

} // namespace fv

#include "field_help.moc"
