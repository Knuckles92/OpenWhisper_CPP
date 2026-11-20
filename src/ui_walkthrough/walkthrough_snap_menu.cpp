#include "ui_walkthrough/walkthrough_snap_menu.h"

#include <algorithm>
#include <chrono>

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScreen>

#include "base/clock.h"
#include "settings/enums.h"
#include "ui_theme/palette.h"
#include "ui_walkthrough/keep_out.h"
#include "ui_walkthrough/walkthrough_icons.h"

namespace fv {
namespace {

constexpr char16_t kHeader[] = u"Snap repeats the one you pick";

} // namespace

QString snapModeLabel(const QString& mode) {
    const auto value = mode.toStdString();
    for (const enums::Entry& entry : enums::labels(enums::EnumId::WalkthroughSnapMode)) {
        if (entry.value.is_string() && entry.value.get<std::string>() == value) {
            return QString::fromStdString(entry.text);
        }
    }
    return mode;
}

SnapMenu::SnapMenu(const Clock& clock, const QStringList& modes, const QString& current,
                   const QString& shortcut)
    : QWidget(nullptr), clock_(clock), modes_(modes), current_(current), shortcut_(shortcut),
      pointer_([] { return QCursor::pos(); }) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAttribute(Qt::WA_QuitOnClose, false);
    setMouseTracking(true);
    watch_.setParent(this);
    watch_.setInterval(kWatchMs);
    connect(&watch_, &QTimer::timeout, this, &SnapMenu::checkPointer);
    const int height = kPad + kHeaderH + kItemH * static_cast<int>(modes.size()) + kPad;
    setFixedSize(QSize(kWidth + 2 * kShadow, height + 2 * kShadow));
}

double SnapMenu::now() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

void SnapMenu::openBeside(const QRect& card, int bottom, std::optional<QRect> nearRect) {
    near_ = nearRect.value_or(card);
    QScreen* screen = QApplication::screenAt(card.center());
    if (screen == nullptr) {
        screen = QApplication::primaryScreen();
    }
    const QRect area = screen != nullptr ? screen->availableGeometry() : QRect();
    int x = card.left() - kGap - kWidth - kShadow;
    if (area.isValid() && x + kShadow < area.left()) {
        x = card.right() + kGap - kShadow;
    }
    int y = bottom - height() + kShadow;
    if (area.isValid()) {
        y = std::max(area.top() - kShadow, y);
    }
    move(x, y);
    show();
    raise();
    // A pick closes it just before the screen is grabbed. Elsewhere the snap waits for the
    // column to leave the screen, and the menu with it.
    setKeepOutOfScreenshots(this, true);
    watch_.start();
}

void SnapMenu::checkPointer() {
    const QPoint pointer = pointer_();
    if (geometry().contains(pointer) || near_.contains(pointer)) {
        awaySince_.reset();
    } else if (!awaySince_) {
        awaySince_ = now();
    } else if (now() - *awaySince_ >= kAwayS) {
        close();
    }
}

QRectF SnapMenu::menuRect() const {
    return QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow);
}

QRectF SnapMenu::item(int index) const {
    const QRectF menu = menuRect();
    return QRectF(menu.left() + kPad, menu.top() + kPad + kHeaderH + index * kItemH,
                  menu.width() - 2 * kPad, kItemH);
}

QString SnapMenu::at(QPointF position) const {
    for (int index = 0; index < modes_.size(); ++index) {
        if (item(index).contains(position)) {
            return modes_[index];
        }
    }
    return QString();
}

void SnapMenu::mouseMoveEvent(QMouseEvent* event) {
    const QString hover = at(event->position());
    if (hover != hover_) {
        hover_ = hover;
        update();
    }
}

void SnapMenu::leaveEvent(QEvent*) {
    hover_.clear();
    update();
}

void SnapMenu::mouseReleaseEvent(QMouseEvent* event) {
    const QString mode = at(event->position());
    if (!mode.isEmpty()) {
        close();
        Q_EMIT chosen(mode);
    }
}

void SnapMenu::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF menu = menuRect();
    painter.setPen(Qt::NoPen);
    for (int i = kShadow; i > 0; i -= 2) {
        const double share = 1.0 - static_cast<double>(i) / kShadow;
        painter.setBrush(QColor(0, 0, 0, static_cast<int>(10 * share + 3)));
        painter.drawRoundedRect(menu.adjusted(-i, -i + 5, i, i + 5), kRadius + i, kRadius + i);
    }
    painter.setBrush(ui::tokenColor("surface"));
    painter.setPen(QPen(ui::tokenColor("border-hover"), 1.0));
    painter.drawRoundedRect(menu.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    QFont small(font());
    small.setPixelSize(11);
    painter.setFont(small);
    painter.setPen(ui::tokenColor("text-secondary"));
    painter.drawText(
        QRectF(menu.left() + kPad + 10, menu.top() + kPad + 2, menu.width(), kHeaderH - 4),
        Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf16(kHeader));

    QFont label(font());
    label.setPixelSize(13);
    // The one under the pointer is lit; with none, the one Snap repeats.
    const QString lit = hover_.isEmpty() ? current_ : hover_;
    for (int index = 0; index < modes_.size(); ++index) {
        const QString& mode = modes_[index];
        const QRectF row = item(index);
        if (mode == lit) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ui::tokenColor("accent-tint-strong"));
            painter.drawRoundedRect(row, 7, 7);
        }
        if (mode == current_) {
            paintWalkthroughIcon(painter, "check", QPointF(row.left() + 18, row.center().y()), 14,
                                 ui::tokenColor("accent"));
        }
        const QRectF text(row.left() + 36, row.top(), row.width() - 46, row.height());
        if (mode == current_ && !shortcut_.isEmpty()) {
            painter.setFont(small);
            painter.setPen(ui::tokenColor("text-secondary"));
            painter.drawText(text, Qt::AlignRight | Qt::AlignVCenter, shortcut_);
        }
        painter.setFont(label);
        painter.setPen(ui::tokenColor("text"));
        painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, snapModeLabel(mode));
    }
}

} // namespace fv
