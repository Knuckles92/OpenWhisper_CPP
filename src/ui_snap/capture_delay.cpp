#include "ui_snap/capture_delay.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScreen>

#include "pyport/number.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_snap/affinity.h"
#include "ui_snap/chrome.h"
#include "ui_snap/icons.h"
#include "ui_theme/palette.h"

namespace fv::ui_snap {
namespace {

constexpr char16_t kInstant[] = u"Delay · Instant";
constexpr char16_t kTip[] = u"Wait after each click or mark before taking a screenshot. Keep "
                            u"drawing to restart the countdown. Esc while drawing cancels a "
                            u"pending shot.";
// The × of a running countdown: the chip's last 27 px.
constexpr int kCancelWidth = 27;
// The picker closes after the pointer has been away this long.
constexpr std::chrono::milliseconds kAwayClose(600);

int readSeconds(const SettingsStore* settings) {
    const std::int64_t seconds = settings != nullptr
                                     ? settings->get(keys::walkthrough_capture_delay)
                                     : keys::walkthrough_capture_delay.read(Json::object());
    return static_cast<int>(seconds);
}

QString presetText(std::int64_t seconds) {
    return seconds != 0 ? QStringLiteral("%1s").arg(seconds) : QStringLiteral("Instant");
}

} // namespace

std::span<const std::int64_t> captureDelays() {
    return keys::int_choices::walkthrough_capture_delay;
}

QString captureDelayText(int seconds, double remaining) {
    if (remaining > 0) {
        // Python's f"{remaining:.1f}": the shortest correctly rounded form, as std::format.
        return QStringLiteral("Snap in %1s")
            .arg(QString::fromStdString(std::format("{:.1f}", remaining)));
    }
    return seconds != 0 ? QStringLiteral(u"Delay · %1s").arg(seconds)
                        : QString::fromUtf16(kInstant);
}

int captureDelayArc(double remaining, double total) {
    const double share = std::min(1.0, remaining / std::max(0.001, total));
    return -static_cast<int>(py::round_half_even(360 * 16 * share));
}

int steppedCaptureDelay(int seconds, int key) {
    const std::span<const std::int64_t> delays = captureDelays();
    const auto found = std::find(delays.begin(), delays.end(), seconds);
    if (found == delays.end()) {
        throw std::out_of_range(std::to_string(seconds) + " is not in list");
    }
    const int index = static_cast<int>(found - delays.begin());
    const int step = key == Qt::Key_Right || key == Qt::Key_Up ? 1 : -1;
    const int last = static_cast<int>(delays.size()) - 1;
    return static_cast<int>(delays[std::max(0, std::min(last, index + step))]);
}

// ---- CaptureDelay ----

CaptureDelay::CaptureDelay(QWidget* parent, SettingsStore* settings)
    : QWidget(parent), settings_(settings) {
    const QFontMetrics metrics(chromeFont(font(), 12));
    setFixedSize(std::max(148, metrics.horizontalAdvance(QString::fromUtf16(kInstant)) + 61), 34);
    setFocusPolicy(parent != nullptr ? Qt::NoFocus : Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_AlwaysShowToolTips);
    setToolTip(QString::fromUtf16(kTip));
    seconds_ = readSeconds(settings_);
    updateAccessibleName();
}

CaptureDelay::~CaptureDelay() = default;

void CaptureDelay::updateAccessibleName() {
    setAccessibleName(seconds_ != 0 ? QStringLiteral("Capture delay: %1 seconds").arg(seconds_)
                                    : QStringLiteral("Capture delay: instant"));
}

void CaptureDelay::setSeconds(int seconds) {
    seconds_ = seconds;
    updateAccessibleName();
    update();
}

void CaptureDelay::setCountdown(double remaining, double total) {
    remaining_ = std::max(0.0, remaining);
    total_ = total;
    update();
}

QList<QWidget*> CaptureDelay::popupWindows() const {
    if (menu_) {
        return {menu_.data()};
    }
    return {};
}

void CaptureDelay::closePicker() {
    if (DelayPicker* menu = menu_.data()) {
        menu_ = nullptr;
        menu->close();
        menu->deleteLater();
    }
}

void CaptureDelay::hideEvent(QHideEvent* event) {
    closePicker();
    QWidget::hideEvent(event);
}

bool CaptureDelay::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
        update();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void CaptureDelay::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton;
}

void CaptureDelay::mouseReleaseEvent(QMouseEvent* event) {
    const bool pressed = std::exchange(pressed_, false);
    if (!pressed || event->button() != Qt::LeftButton ||
        !rect().contains(event->position().toPoint())) {
        return;
    }
    if (remaining_ != 0 && event->position().x() >= width() - kCancelWidth) {
        Q_EMIT cancelled();
    } else if (menu_ && menu_->isVisible()) {
        closePicker();
    } else {
        closePicker();
        auto* menu = new DelayPicker(this);
        menu_ = menu;
        connect(menu, &DelayPicker::chosen, this, &CaptureDelay::choose);
        menu->open();
    }
}

void CaptureDelay::choose(int seconds) {
    if (settings_ != nullptr) {
        settings_->save_setting(keys::walkthrough_capture_delay.key(), seconds);
    }
    setSeconds(seconds);
    closePicker();
    Q_EMIT changed(seconds);
}

void CaptureDelay::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        closePicker();
    } else if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
               key == Qt::Key_Down) {
        const std::span<const std::int64_t> delays = captureDelays();
        if (std::find(delays.begin(), delays.end(), seconds_) == delays.end()) {
            // Python's handler raises ValueError here and the key does nothing; an exception
            // must not leave a Qt event handler, so the key is ignored instead.
            return;
        }
        choose(steppedCaptureDelay(seconds_, key));
    } else {
        QWidget::keyPressEvent(event);
    }
}

void CaptureDelay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& palette = ui::currentPalette();
    const bool active = remaining_ > 0;
    const QColor ink = palette.color(active || seconds_ != 0 ? "accent" : "text-secondary");
    painter.setOpacity(isEnabled() ? 1.0 : 0.4);
    painter.setPen(QPen(palette.color(active || hasFocus() ? "accent-tint-border" : "border"), 1));
    painter.setBrush(palette.color(active         ? "accent-tint"
                                   : underMouse() ? "surface-hover"
                                                  : "surface"));
    painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 10, 10);
    const QRectF ring(11, 9, 16, 16);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(active ? palette.color("border-strong") : ink, 1.6));
    painter.drawEllipse(ring);
    painter.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap));
    if (active) {
        painter.drawArc(ring, 90 * 16, captureDelayArc(remaining_, total_));
    } else {
        painter.drawLine(QPointF(19, 12), QPointF(19, 17));
        painter.drawLine(QPointF(19, 17), QPointF(22, 19));
    }
    painter.setFont(chromeFont(font(), 12));
    painter.setPen(palette.color("text"));
    painter.drawText(QRectF(35, 0, width() - 61, height()), Qt::AlignVCenter,
                     captureDelayText(seconds_, remaining_));
    if (active) {
        paintSnapIcon(painter, "drop", QRectF(width() - 23, 10, 14, 14), ink);
    } else {
        painter.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap));
        const int x = width() - 17;
        painter.drawLine(QPointF(x - 3, 15), QPointF(x, 18));
        painter.drawLine(QPointF(x, 18), QPointF(x + 3, 15));
    }
}

// ---- DelayPicker ----

DelayPicker::DelayPicker(CaptureDelay* chip)
    : QWidget(chip, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                        Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
      chip_(chip) {
    setFixedSize(264, 170);
    setWindowTitle(QStringLiteral("Automatic screenshot delay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    setMouseTracking(true);
    watch_.setInterval(100);
    connect(&watch_, &QTimer::timeout, this, &DelayPicker::checkPointer);
}

void DelayPicker::open() {
    const QPoint anchor = chip_->mapToGlobal(chip_->rect().bottomLeft());
    QScreen* screen = QGuiApplication::screenAt(anchor);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect area = screen != nullptr ? screen->availableGeometry() : QRect();
    const int x = std::max(area.left(), std::min(anchor.x(), area.right() - width() + 1));
    int y = anchor.y() + 7;
    if (y + height() > area.bottom()) {
        y = anchor.y() - chip_->height() - height() - 7;
    }
    move(x, std::max(area.top(), y));
    show();
    keepOutOfScreenshots(this, true);
    raise();
    watch_.start();
}

void DelayPicker::checkPointer() {
    const QPoint pointer = QCursor::pos();
    if (geometry().contains(pointer) || chip_->window()->geometry().contains(pointer)) {
        away_.reset();
    } else if (!away_) {
        away_ = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - *away_ > kAwayClose) {
        close();
    }
}

void DelayPicker::closeEvent(QCloseEvent* event) {
    watch_.stop();
    QWidget::closeEvent(event);
}

QRectF DelayPicker::option(int index) {
    return QRectF(14 + index % 3 * 80, 68 + index / 3 * 42, 76, 36);
}

int DelayPicker::at(QPointF position) {
    const int count = static_cast<int>(captureDelays().size());
    for (int index = 0; index < count; ++index) {
        if (option(index).contains(position)) {
            return index;
        }
    }
    return -1;
}

void DelayPicker::mouseMoveEvent(QMouseEvent* event) {
    hover_ = at(event->position());
    setCursor(hover_ >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void DelayPicker::leaveEvent(QEvent*) {
    hover_ = -1;
    update();
}

void DelayPicker::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton ? at(event->position()) : -1;
}

void DelayPicker::mouseReleaseEvent(QMouseEvent* event) {
    const int index = std::exchange(pressed_, -1);
    if (event->button() == Qt::LeftButton && index >= 0 && index == at(event->position())) {
        Q_EMIT chosen(static_cast<int>(captureDelays()[index]));
    }
}

void DelayPicker::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& palette = ui::currentPalette();
    paintPanel(painter, QRectF(rect()).adjusted(2, 2, -2, -2), 14, palette, 0.0);
    painter.setFont(chromeFont(font(), 13));
    painter.setPen(palette.color("text"));
    painter.drawText(QRectF(16, 13, 232, 22), Qt::AlignVCenter, QStringLiteral("Screenshot delay"));
    painter.setFont(chromeFont(font(), 11));
    painter.setPen(palette.color("text-secondary"));
    painter.drawText(QRectF(16, 38, 232, 18), Qt::AlignVCenter,
                     QStringLiteral("Restarts after each click or mark"));
    const std::span<const std::int64_t> delays = captureDelays();
    for (int index = 0; index < static_cast<int>(delays.size()); ++index) {
        const bool selected = delays[index] == chip_->seconds();
        const QRectF rect = option(index);
        painter.setPen(QPen(palette.color(selected ? "accent-border" : "border"), 1));
        painter.setBrush(palette.color(selected          ? "accent-tint-strong"
                                       : index == hover_ ? "surface-hover"
                                                         : "surface"));
        painter.drawRoundedRect(rect, 8, 8);
        painter.setPen(palette.color(selected ? "accent" : "text"));
        painter.setFont(chromeFont(font(), 12));
        painter.drawText(rect, Qt::AlignCenter, presetText(delays[index]));
    }
}

} // namespace fv::ui_snap
