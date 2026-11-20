#include "ui_snap/toolbar.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QToolTip>

#include "ui_snap/chrome.h"
#include "ui_snap/desktop.h"
#include "ui_snap/icons.h"

namespace fv::ui_snap {
namespace {

struct ToolTip {
    SnapTool tool;
    const char16_t* tip;
};

constexpr std::array<ToolTip, 8> kTools{{
    {SnapTool::Arrow, u"Arrow (A) · Shift to align to 45° angles"},
    {SnapTool::Box, u"Box (B) · Shift for a square"},
    {SnapTool::Ellipse, u"Ellipse (O) · Shift for a circle"},
    {SnapTool::Pen, u"Pen (P)"},
    {SnapTool::Highlighter, u"Highlighter (H)"},
    {SnapTool::Text, u"Text (T)"},
    {SnapTool::Step, u"Step number (N)"},
    {SnapTool::Redact, u"Redact (R) · Drag an opaque cover over private details"},
}};

constexpr char16_t kInfillTip[] = u"Fill new boxes and ellipses with a light tint; off leaves only "
                                  u"the outline. Remembered for future screenshots.";
constexpr char16_t kRegionTip[] = u"Select an area for automatic screenshots; close its × to "
                                  u"capture the whole screen";
constexpr char16_t kRedactSwatchTip[] = u"Redactions always use an opaque cover.";
constexpr char16_t kNothingToDropTip[] =
    u"No tool is selected, so there is nothing to put down. Cancel or Done on the walkthrough "
    u"column ends the walkthrough.";

QString str16(const char16_t* value) {
    return QString::fromUtf16(value);
}

// The one-row layout of _layout, built control by control.
class Builder {
public:
    explicit Builder(const QFontMetrics& metrics) : metrics_(metrics) {}

    void add(const QString& kind, const QString& value, const QString& tip,
             int width = kToolbarButton, const QString& label = QString()) {
        if (!label.isEmpty()) {
            width = kToolbarIcon + kToolbarLabelGap + metrics_.horizontalAdvance(label) +
                    2 * kToolbarLabelPad + 4;
        }
        layout_.buttons.push_back(
            {kind, value, QRect(x_, kToolbarPad, width, kToolbarButton), tip, label});
        x_ += width;
    }

    void separate() {
        layout_.separators.emplace_back(x_ + kToolbarSeparator / 2, kToolbarPad);
        x_ += kToolbarSeparator;
    }

    ToolbarLayout take() { return std::move(layout_); }

private:
    const QFontMetrics& metrics_;
    ToolbarLayout layout_;
    int x_ = kToolbarPad;
};

} // namespace

std::optional<SnapTool> toolFromName(const QString& name) {
    for (const ToolTip& entry : kTools) {
        if (name == QLatin1String(toolName(entry.tool))) {
            return entry.tool;
        }
    }
    return std::nullopt;
}

ToolbarLayout toolbarLayout(bool live, const QFontMetrics& labelMetrics,
                            const MarkShortcuts& markShortcuts) {
    Builder build(labelMetrics);
    const QString pin = systemPlatform() == "darwin" ? QStringLiteral(u" · Hold Cmd to pin")
                                                     : QStringLiteral(u" · Hold Ctrl to pin");
    for (const ToolTip& entry : kTools) {
        QString tip = str16(entry.tip);
        if (live) {
            tip += pin;
        }
        build.add(QStringLiteral("tool"), QLatin1String(toolName(entry.tool)), tip);
    }
    build.separate();
    if (!live) {
        build.add(QStringLiteral("undo"), QStringLiteral("undo"), QStringLiteral("Undo (Ctrl+Z)"));
        build.add(QStringLiteral("redo"), QStringLiteral("redo"),
                  QStringLiteral("Redo (Ctrl+Shift+Z)"));
        build.separate();
    }
    for (int index = 0; index < static_cast<int>(kSwatches.size()); ++index) {
        build.add(QStringLiteral("color"), QString::number(index),
                  QStringLiteral("%1 (%2)").arg(str16(kSwatches[index].name)).arg(index + 1),
                  kToolbarSwatch);
    }
    build.separate();
    build.add(QStringLiteral("infill"), QStringLiteral("infill"), str16(kInfillTip), kToolbarButton,
              QStringLiteral("Infill"));
    build.add(QStringLiteral("snap"), QStringLiteral("snap"),
              QStringLiteral("Snap to corners and window edges (S)"));
    if (live) {
        build.add(QStringLiteral("region"), QStringLiteral("region"), str16(kRegionTip),
                  kToolbarButton, QStringLiteral("Region"));
        build.separate();
        build.add(
            QStringLiteral("delay"), QStringLiteral("delay"), QStringLiteral("Screenshot delay"),
            std::max(148, labelMetrics.horizontalAdvance(QStringLiteral(u"Delay · Instant")) + 61));
        build.separate();
        const QString keepKey =
            markShortcuts.value(QStringLiteral("keep_marks"), QStringLiteral("K while drawing"));
        const QString clearKey =
            markShortcuts.value(QStringLiteral("clear"), QStringLiteral("C while drawing"));
        build.add(QStringLiteral("keep_marks"), QStringLiteral("keep_marks"),
                  QStringLiteral("Keep marks (%1)").arg(keepKey), kToolbarButton,
                  QStringLiteral("Keep marks"));
        build.add(QStringLiteral("clear"), QStringLiteral("clear"),
                  QStringLiteral("Clear marks (%1)").arg(clearKey), kToolbarButton,
                  QStringLiteral("Clear marks"));
        build.add(QStringLiteral("drop"), QStringLiteral("drop"),
                  QStringLiteral("Put the tool down and return to the desktop (Esc)"));
    } else {
        build.add(QStringLiteral("drop"), QStringLiteral("drop"), QStringLiteral("Cancel (Esc)"));
        build.add(QStringLiteral("keep"), QStringLiteral("keep"), QStringLiteral("Keep (Enter)"));
    }
    return build.take();
}

ToolbarLayout wrapToolbar(const std::vector<ToolbarButton>& buttons, int width) {
    // A gap of a separator's width or more between two buttons starts a new group.
    std::vector<std::vector<ToolbarButton>> groups(1);
    for (const ToolbarButton& button : buttons) {
        const std::vector<ToolbarButton>& previous = groups.back();
        if (!previous.empty() &&
            button.rect.left() - previous.back().rect.right() - 1 >= kToolbarSeparator) {
            groups.emplace_back();
        }
        groups.back().push_back(button);
    }
    ToolbarLayout wrapped;
    int x = kToolbarPad;
    int top = kToolbarPad;
    for (const std::vector<ToolbarButton>& group : groups) {
        const int groupWidth = group.back().rect.right() + 1 - group.front().rect.left();
        int gap = x > kToolbarPad ? kToolbarSeparator : 0;
        if (x > kToolbarPad && x + gap + groupWidth + kToolbarPad > width) {
            x = kToolbarPad;
            top += kToolbarButton + 4;
            gap = 0;
        }
        if (gap != 0) {
            wrapped.separators.emplace_back(x + kToolbarSeparator / 2, top);
        }
        x += gap;
        const QPoint offset(x - group.front().rect.left(), top - kToolbarPad);
        for (ToolbarButton button : group) {
            button.rect.translate(offset);
            wrapped.buttons.push_back(std::move(button));
        }
        x += groupWidth;
    }
    return wrapped;
}

SnapToolbar::SnapToolbar(QWidget* parent, const ui::Palette& palette, Variant variant,
                         Qt::WindowFlags flags, MarkShortcuts markShortcuts)
    : QWidget(parent, flags), palette_(palette), variant_(variant),
      markShortcuts_(std::move(markShortcuts)) {
    if (variant_ == Variant::Live) {
        setAttribute(Qt::WA_TranslucentBackground);
    }
    setObjectName(QStringLiteral("snapToolbar"));
    labelFont_ = chromeFont(font(), 12);
    layout_ = toolbarLayout(variant_ == Variant::Live, QFontMetrics(labelFont_), markShortcuts_);
    setFixedSize(layout_.buttons.back().rect.right() + 1 + kToolbarPad,
                 kToolbarButton + 2 * kToolbarPad);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

void SnapToolbar::fitToWidth(int width) {
    ToolbarLayout layout =
        toolbarLayout(variant_ == Variant::Live, QFontMetrics(labelFont_), markShortcuts_);
    if (layout.buttons.back().rect.right() + 1 + kToolbarPad > width) {
        layout = wrapToolbar(layout.buttons, width);
    }
    layout_ = std::move(layout);
    hover_ = -1;
    pressed_ = -1;
    int right = 0;
    int bottom = 0;
    for (const ToolbarButton& button : layout_.buttons) {
        right = std::max(right, button.rect.right());
        bottom = std::max(bottom, button.rect.bottom());
    }
    setFixedSize(right + 1 + kToolbarPad, bottom + 1 + kToolbarPad);
    placeCaptureDelay();
}

void SnapToolbar::setState(std::optional<SnapTool> tool, int color, bool canUndo, bool canRedo) {
    if (tool == tool_ && color == color_ && canUndo == canUndo_ && canRedo == canRedo_) {
        return;
    }
    tool_ = tool;
    color_ = color;
    canUndo_ = canUndo;
    canRedo_ = canRedo;
    update();
}

void SnapToolbar::setSnapEnabled(bool enabled) {
    snapEnabled_ = enabled;
    update();
}

void SnapToolbar::setBoxInfill(bool enabled) {
    boxInfill_ = enabled;
    update();
}

void SnapToolbar::setKeepMarks(bool enabled) {
    keepMarks_ = enabled;
    update();
}

void SnapToolbar::setRegionActive(bool active) {
    regionActive_ = active;
    update();
}

void SnapToolbar::setDropEnabled(bool enabled) {
    if (enabled == dropEnabled_) {
        return;
    }
    dropEnabled_ = enabled;
    if (!enabled && hover_ >= 0 && layout_.buttons[hover_].kind == QLatin1String("drop")) {
        hover_ = -1;
        setCursor(Qt::ArrowCursor);
    }
    update();
}

QRect SnapToolbar::buttonRect(const QString& kind, const QString& value) const {
    for (const ToolbarButton& button : layout_.buttons) {
        if (button.kind == kind && (value.isEmpty() || button.value == value)) {
            return button.rect;
        }
    }
    return QRect();
}

QRect SnapToolbar::buttonRect(SnapTool tool) const {
    return buttonRect(QStringLiteral("tool"), QLatin1String(toolName(tool)));
}

void SnapToolbar::setCaptureDelay(QWidget* chip) {
    if (variant_ != Variant::Live || chip == nullptr) {
        return;
    }
    captureDelay_ = chip;
    chip->setParent(this);
    placeCaptureDelay();
    chip->show();
}

void SnapToolbar::placeCaptureDelay() {
    if (captureDelay_) {
        captureDelay_->move(buttonRect(QStringLiteral("delay")).topLeft());
    }
}

bool SnapToolbar::enabled(const ToolbarButton& button) const {
    if (button.kind == QLatin1String("undo")) {
        return canUndo_;
    }
    if (button.kind == QLatin1String("redo")) {
        return canRedo_;
    }
    if (button.kind == QLatin1String("infill")) {
        return tool_ == SnapTool::Box || tool_ == SnapTool::Ellipse;
    }
    if (button.kind == QLatin1String("color")) {
        return tool_ != SnapTool::Redact;
    }
    if (button.kind == QLatin1String("drop")) {
        return dropEnabled_;
    }
    return true;
}

int SnapToolbar::buttonAt(QPoint pos, bool enabledOnly) const {
    for (int index = 0; index < static_cast<int>(layout_.buttons.size()); ++index) {
        const ToolbarButton& button = layout_.buttons[index];
        if (button.rect.contains(pos) && (!enabledOnly || enabled(button))) {
            return index;
        }
    }
    return -1;
}

QString SnapToolbar::tipFor(const ToolbarButton& button) const {
    if (button.kind == QLatin1String("color") && tool_ == SnapTool::Redact) {
        return str16(kRedactSwatchTip);
    }
    if (button.kind == QLatin1String("drop") && !dropEnabled_) {
        return str16(kNothingToDropTip);
    }
    return button.tip;
}

// ---- input ----

void SnapToolbar::mouseMoveEvent(QMouseEvent* event) {
    const int hover = buttonAt(event->position().toPoint());
    if (hover != hover_) {
        hover_ = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    event->accept();
}

void SnapToolbar::leaveEvent(QEvent* event) {
    hover_ = -1;
    update();
    QWidget::leaveEvent(event);
}

void SnapToolbar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed_ = buttonAt(event->position().toPoint());
    }
    event->accept();
}

void SnapToolbar::mouseReleaseEvent(QMouseEvent* event) {
    const int pressed = std::exchange(pressed_, -1);
    if (pressed >= 0 && pressed == buttonAt(event->position().toPoint())) {
        // A copy: a slot may lay the toolbar out again.
        const ToolbarButton button = layout_.buttons[pressed];
        activate(button);
    }
    event->accept();
}

void SnapToolbar::mouseDoubleClickEvent(QMouseEvent* event) {
    // A quick second click (two undos) is still a click, not a double-click.
    mousePressEvent(event);
}

void SnapToolbar::activate(const ToolbarButton& button) {
    const QString& kind = button.kind;
    if (kind == QLatin1String("tool")) {
        if (const std::optional<SnapTool> tool = toolFromName(button.value)) {
            Q_EMIT toolChosen(*tool);
        }
    } else if (kind == QLatin1String("color")) {
        Q_EMIT colorChosen(button.value.toInt());
    } else if (kind == QLatin1String("undo")) {
        Q_EMIT undoClicked();
    } else if (kind == QLatin1String("redo")) {
        Q_EMIT redoClicked();
    } else if (kind == QLatin1String("drop")) {
        Q_EMIT dropClicked();
    } else if (kind == QLatin1String("keep")) {
        Q_EMIT keepClicked();
    } else if (kind == QLatin1String("snap")) {
        Q_EMIT snapClicked();
    } else if (kind == QLatin1String("infill")) {
        Q_EMIT infillClicked();
    } else if (kind == QLatin1String("keep_marks")) {
        Q_EMIT keepMarksClicked();
    } else if (kind == QLatin1String("clear")) {
        Q_EMIT clearClicked();
    } else if (kind == QLatin1String("region")) {
        Q_EMIT regionClicked();
    }
}

bool SnapToolbar::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const int index = buttonAt(help->pos(), false);
        if (index < 0) {
            QToolTip::hideText();
        } else {
            const ToolbarButton& button = layout_.buttons[index];
            QToolTip::showText(help->globalPos(), tipFor(button), this, button.rect);
        }
        return true;
    }
    return QWidget::event(event);
}

// ---- painting ----

void SnapToolbar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paintPanel(painter, QRectF(rect()), kToolbarRadius, palette_, 0.0);
    paintSeparators(painter);
    for (int index = 0; index < static_cast<int>(layout_.buttons.size()); ++index) {
        paintButton(painter, index);
    }
}

void SnapToolbar::paintSeparators(QPainter& painter) const {
    painter.setPen(QPen(palette_.color("overlay-border"), 1));
    for (const QPoint& separator : layout_.separators) {
        painter.drawLine(QPointF(separator.x() + 0.5, separator.y() + 8),
                         QPointF(separator.x() + 0.5, separator.y() + kToolbarButton - 8));
    }
}

void SnapToolbar::paintButton(QPainter& painter, int index) const {
    const ToolbarButton& button = layout_.buttons[index];
    if (button.kind == QLatin1String("delay")) {
        return; // The delay chip paints itself and owns its preset picker.
    }
    if (button.kind == QLatin1String("color")) {
        painter.save();
        if (!enabled(button)) {
            painter.setOpacity(0.32);
        }
        paintSwatch(painter, index);
        painter.restore();
        return;
    }
    const QRectF rect = QRectF(button.rect).adjusted(2, 2, -2, -2);
    const auto [background, icon] = buttonColors(index);
    if (background) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(*background);
        painter.drawRoundedRect(rect, 8, 8);
    }
    if (!button.label.isEmpty()) {
        paintLabeledButton(painter, button, rect, icon);
        return;
    }
    const double inset = (rect.width() - kToolbarIcon) / 2;
    const QString glyph = button.kind == QLatin1String("tool") ? button.value : button.kind;
    paintSnapIcon(painter, glyph.toStdString(), rect.adjusted(inset, inset, -inset, -inset), icon);
}

void SnapToolbar::paintLabeledButton(QPainter& painter, const ToolbarButton& button,
                                     const QRectF& rect, const QColor& icon) const {
    const QRectF iconRect(rect.left() + kToolbarLabelPad, rect.center().y() - kToolbarIcon / 2.0,
                          kToolbarIcon, kToolbarIcon);
    if (button.kind == QLatin1String("keep_marks") || button.kind == QLatin1String("infill")) {
        painter.setPen(QPen(icon, 1.3));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(iconRect.adjusted(3, 3, -3, -3), 3, 3);
        const bool checked = button.kind == QLatin1String("keep_marks") ? keepMarks_ : boxInfill_;
        if (checked) {
            paintSnapIcon(painter, "keep", iconRect, icon);
        }
    } else {
        paintSnapIcon(painter, button.kind.toStdString(), iconRect, icon);
    }
    painter.setFont(labelFont_);
    painter.setPen(!enabled(button) ? icon : palette_.color("overlay-text"));
    QRectF labelRect(rect);
    labelRect.setLeft(iconRect.right() + kToolbarLabelGap);
    painter.drawText(labelRect, Qt::AlignVCenter, button.label);
}

std::pair<std::optional<QColor>, QColor> SnapToolbar::buttonColors(int index) const {
    const ToolbarButton& button = layout_.buttons[index];
    const QColor text = palette_.color("overlay-text");
    const bool hovered = index == hover_;
    if (button.kind == QLatin1String("keep")) {
        return {palette_.color(hovered ? "accent-hover" : "accent"), QColor(255, 255, 255)};
    }
    if (!enabled(button)) {
        QColor faded(text);
        faded.setAlphaF(0.32F);
        return {std::nullopt, faded};
    }
    if ((button.kind == QLatin1String("snap") && snapEnabled_) ||
        (button.kind == QLatin1String("infill") && boxInfill_) ||
        (button.kind == QLatin1String("keep_marks") && keepMarks_) ||
        (button.kind == QLatin1String("region") && regionActive_)) {
        return {palette_.color("overlay-rgb", 36), palette_.color("accent")};
    }
    if (button.kind == QLatin1String("tool") && tool_ &&
        button.value == QLatin1String(toolName(*tool_))) {
        const QColor color = *tool_ == SnapTool::Redact
                                 ? text
                                 : QColor(QString::fromLatin1(kSwatches[color_].color));
        QColor tint(color);
        tint.setAlpha(hovered ? 46 : 34);
        return {tint, color};
    }
    if (hovered) {
        return {palette_.color("overlay-rgb", 24), text};
    }
    return {std::nullopt, text};
}

void SnapToolbar::paintSwatch(QPainter& painter, int index) const {
    const ToolbarButton& button = layout_.buttons[index];
    const QPointF center = QRectF(button.rect).center();
    const int swatch = button.value.toInt();
    const QColor color(QString::fromLatin1(kSwatches[swatch].color));
    const bool selected = swatch == color_;
    if (selected || index == hover_) {
        QColor ring(palette_.color("overlay-text"));
        ring.setAlphaF(selected ? 1.0F : 0.35F);
        painter.setPen(QPen(ring, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, 11.0, 11.0);
    }
    painter.setPen(QPen(QColor(0, 0, 0, 60), 1.0));
    painter.setBrush(color);
    painter.drawEllipse(center, 8.0, 8.0);
}

} // namespace fv::ui_snap
