#include "ui_history/calendar_glyph_button.h"

#include <chrono>

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include "base/clock.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"

namespace fv {
namespace {

const QString kGlyphStyle = QStringLiteral(R"qss(
            QPushButton#calendarGlyphBtn {
                background-color: transparent;
                border: none;
                border-radius: 14px;
                padding: 0px;
            }
            QPushButton#calendarGlyphBtn:hover {
                background-color: rgba(@overlay-rgb, 0.1);
            }
        )qss");

} // namespace

CalendarGlyphButton::CalendarGlyphButton(QWidget* parent)
    : CalendarGlyphButton(system_clock(), parent) {}

CalendarGlyphButton::CalendarGlyphButton(const Clock& clock, QWidget* parent)
    : QPushButton(parent), clock_(clock) {
    setObjectName(QStringLiteral("calendarGlyphBtn"));
    setFixedSize(28, 28);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(QStringLiteral("History calendar"));
    ui::setLocalStyleSheet(this, kGlyphStyle);
    connect(this, &QPushButton::clicked, this, &CalendarGlyphButton::calendarRequested);
}

int CalendarGlyphButton::dayOfMonth() const {
    const std::chrono::zoned_time local(clock_.zone(), clock_.utc());
    const auto day = std::chrono::floor<std::chrono::days>(local.get_local_time());
    return static_cast<int>(static_cast<unsigned>(std::chrono::year_month_day(day).day()));
}

void CalendarGlyphButton::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = ui::tokenColor(underMouse() ? "text-heading" : "text-secondary");
    const QRectF page(6.5, 7.5, 15, 14);
    painter.setPen(QPen(color, 1.4));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(page, 3, 3);
    QPainterPath band;
    band.addRoundedRect(QRectF(page.left(), page.top(), page.width(), 4.5), 3, 3);
    painter.fillPath(band, color);
    for (const double x : {page.left() + 4, page.right() - 4}) {
        painter.drawLine(QPointF(x, page.top() - 2), QPointF(x, page.top() + 1.5));
    }
    QFont glyphFont(font());
    glyphFont.setPixelSize(8);
    glyphFont.setWeight(QFont::Bold);
    painter.setFont(glyphFont);
    painter.drawText(page.adjusted(0, 4, 0, 0), Qt::AlignCenter, QString::number(dayOfMonth()));
}

} // namespace fv
