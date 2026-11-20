#include "ui_widgets/agent_tile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <tuple>

#include <QDesktopServices>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "pyport/number.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/agent_mark.h"

namespace fv {

namespace paint = agent_paint;
namespace picker = agent_picker;

namespace {

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

int pyRound(double value) {
    return static_cast<int>(py::round_half_even(value));
}

} // namespace

// ---- StatusPill ----

StatusPill::StatusPill(QWidget* parent) : QWidget(parent), tone_(qs(picker::kIdle)) {
    setObjectName(QStringLiteral("agentStatusPill"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatusPill::setState(const QString& tone, const QString& text) {
    if (tone == tone_ && text == text_) {
        return;
    }
    tone_ = tone;
    text_ = text;
    setAccessibleName(text);
    updateGeometry();
    update();
}

void StatusPill::setProgress(double appear, double check) {
    appear_ = paint::clamp(appear);
    check_ = paint::clamp(check);
    update();
}

bool StatusPill::hasGlyph() const {
    return tone_ == qs(picker::kFound) || tone_ == qs(picker::kWarning);
}

double StatusPill::glyphSide() const {
    return QFontMetricsF(font()).ascent() * 0.9;
}

QSize StatusPill::sizeHint() const {
    const QFontMetricsF metrics(font());
    double width = text_.isEmpty() ? 0.0 : metrics.horizontalAdvance(text_) + 2 * kPad;
    if (!text_.isEmpty() && hasGlyph()) {
        width += glyphSide() + kGap;
    }
    return {static_cast<int>(std::ceil(width)), static_cast<int>(std::ceil(metrics.height() + 6))};
}

QSize StatusPill::minimumSizeHint() const {
    return sizeHint();
}

void StatusPill::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updateGeometry();
    }
}

void StatusPill::paintEvent(QPaintEvent*) {
    if (text_.isEmpty() || appear_ <= 0.0) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(appear_);
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Grows the last few percent into place as it fades in.
    const double scale = 0.86 + 0.14 * paint::easeOut(appear_);
    const QPointF center = rect.center();
    painter.translate(center);
    painter.scale(scale, scale);
    painter.translate(-center);

    QColor fill;
    QColor border;
    QColor ink;
    if (tone_ == qs(picker::kFound)) {
        const QColor rgb = ui::tokenColor("success-rgb");
        fill = paint::alpha(rgb, 0.15);
        border = paint::alpha(rgb, 0.42);
        ink = ui::tokenColor("success-text-strong");
    } else if (tone_ == qs(picker::kWarning)) {
        const QColor rgb = ui::tokenColor("warning-rgb");
        fill = paint::alpha(rgb, 0.15);
        border = paint::alpha(rgb, 0.48);
        ink = ui::tokenColor("warning-text");
    } else if (tone_ == qs(picker::kBuiltinTone)) {
        fill = ui::tokenColor("accent-tint");
        border = ui::tokenColor("accent-tint-border");
        ink = ui::tokenColor("accent-soft");
    } else {
        fill = ui::tokenColor("slate-raised");
        border = ui::tokenColor("slate-border");
        ink = ui::tokenColor("slate-text-3");
    }
    const double radius = rect.height() / 2.0;
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);

    double x = rect.left() + kPad;
    if (hasGlyph()) {
        const double side = glyphSide();
        const QRectF box(x, center.y() - side / 2.0, side, side);
        if (tone_ == qs(picker::kFound)) {
            paintCheck(painter, box, ink);
        } else {
            paintAlert(painter, box, ink);
        }
        x += side + kGap;
    }
    painter.setOpacity(appear_);
    painter.setPen(ink);
    painter.setFont(font());
    painter.drawText(QRectF(x, rect.top(), rect.right() - x, rect.height()),
                     Qt::AlignVCenter | Qt::AlignLeft, text_);
}

void StatusPill::paintCheck(QPainter& painter, const QRectF& box, const QColor& ink) const {
    const std::array<QPointF, 3> points{
        QPointF(box.left() + box.width() * 0.14, box.top() + box.height() * 0.54),
        QPointF(box.left() + box.width() * 0.40, box.top() + box.height() * 0.80),
        QPointF(box.left() + box.width() * 0.88, box.top() + box.height() * 0.24),
    };
    std::array<double, 2> lengths{};
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        lengths[i] =
            std::hypot(points[i + 1].x() - points[i].x(), points[i + 1].y() - points[i].y());
    }
    double remaining = (lengths[0] + lengths[1]) * check_;
    if (remaining <= 0.0) {
        return;
    }
    QPainterPath path(points[0]);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (remaining <= 0.0) {
            break;
        }
        const double length = lengths[i];
        const double t = length != 0.0 ? std::min(1.0, remaining / length) : 1.0;
        path.lineTo(points[i] + (points[i + 1] - points[i]) * t);
        remaining -= length;
    }
    QPen pen(ink, std::max(1.4, box.height() * 0.16));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

void StatusPill::paintAlert(QPainter& painter, const QRectF& box, const QColor& ink) const {
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    const double radius = box.width() * 0.42;
    painter.drawEllipse(box.center(), radius, radius);
    QPen pen(ui::tokenColor("slate-surface"), std::max(1.2, box.width() * 0.14));
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    const QPointF top(box.center().x(), box.center().y() - radius * 0.5);
    const QPointF bottom(box.center().x(), box.center().y() + radius * 0.12);
    painter.drawLine(top, bottom);
    painter.drawPoint(QPointF(box.center().x(), box.center().y() + radius * 0.5));
}

// ---- AgentTile ----

AgentTile::AgentTile(const QString& tileId, const QString& name, const QString& monogram,
                     const QColor& accent, const QPixmap& pixmap, const QString& installUrl,
                     const QString& installHint, QWidget* parent)
    : QWidget(parent), tileId_(tileId), name_(name), purpose_(qs(picker::kInsightsPurpose)),
      accent_(accent), installUrl_(installUrl), state_(picker::idle_state()) {
    setObjectName(QStringLiteral("agentTile"));
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    layout_ = new QVBoxLayout(this);
    layout_->setSpacing(5);
    auto* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 4);
    top->setSpacing(8);
    mark_ = new AgentMark(monogram, accent, pixmap, this);
    top->addWidget(mark_, 0, Qt::AlignTop);
    top->addStretch(1);
    pill_ = new StatusPill(this);
    pill_->hide();
    top->addWidget(pill_, 0, Qt::AlignTop);
    layout_->addLayout(top);
    nameLabel_ = new QLabel(name, this);
    nameLabel_->setObjectName(QStringLiteral("agentTileName"));
    layout_->addWidget(nameLabel_);
    detailLabel_ = new QLabel(this);
    detailLabel_->setObjectName(QStringLiteral("agentTileDetail"));
    detailLabel_->setWordWrap(true);
    detailLabel_->hide();
    layout_->addWidget(detailLabel_);
    link_ = new QPushButton(QStringLiteral(u"Get %1 ↗").arg(name), this);
    link_->setObjectName(QStringLiteral("agentTileLink"));
    link_->setFlat(true);
    link_->setCursor(Qt::PointingHandCursor);
    link_->setToolTip(installHint);
    link_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(link_, &QPushButton::clicked, this, &AgentTile::openInstallPage);
    link_->hide();
    layout_->addWidget(link_, 0, Qt::AlignLeft);
    // Content sits at the top of a row's tallest tile. The grid places tiles by hand, so this
    // stretch never makes a page layout expand (a layout alignment would squeeze wrapped text to
    // the preferred width's height instead).
    layout_->addStretch(1);
    applyMargins();

    hoverAnim_ = makeAnimation(picker::kHoverMs, &AgentTile::onHoverValue);
    liftAnim_ = makeAnimation(picker::kLiftMs, &AgentTile::onLiftValue);
    arrivalAnim_ = makeAnimation(picker::kArrivalMs, &AgentTile::onArrivalValue);
    shimmerAnim_ = makeAnimation(picker::kShimmerFadeMs, &AgentTile::onShimmerValue);
    arrivalTimer_ = new QTimer(this);
    arrivalTimer_->setSingleShot(true);
    connect(arrivalTimer_, &QTimer::timeout, this, &AgentTile::beginArrival);
    syncInteraction();
}

void AgentTile::setPurpose(const QString& purpose) {
    purpose_ = purpose;
}

bool AgentTile::arrivalPending() const {
    return arrivalTimer_->isActive();
}

void AgentTile::beginScan(int index) {
    scanning_ = true;
    shimmerOffset_ = index * picker::kShimmerOffsetMs;
    clock_.start();
    shimmerAnim_->stop();
    shimmer_ = 1.0;
    if (state_.tone == picker::kIdle) {
        setDetail(qs(picker::kCheckingDetail), true);
    }
    setPill(qs(picker::kScanning), QStringLiteral(u"Looking…"));
    pill_->setProgress(1.0, 1.0);
    advanceScan();
}

void AgentTile::advanceScan() {
    syncMark();
    update();
}

void AgentTile::endScan(bool animate) {
    scanning_ = false;
    if (animate && shimmer_ > 0.0) {
        shimmerAnim_->stop();
        shimmerAnim_->setStartValue(shimmer_);
        shimmerAnim_->setEndValue(0.0);
        shimmerAnim_->start();
    } else {
        shimmerAnim_->stop();
        onShimmerValue(0.0);
    }
}

void AgentTile::showState(const picker::TileState& state, bool animate, int delayMs) {
    arrivalTimer_->stop();
    if (!animate) {
        pending_.reset();
        arrivalAnim_->stop();
        applyState(state);
        warmFrom_ = warmTo_ = state.installed ? 1.0 : 0.0;
        missingFrom_ = state.tone == picker::kMissing ? 1.0 : 0.0;
        onArrivalValue(1.0);
        return;
    }
    pending_ = state;
    if (delayMs > 0) {
        arrivalTimer_->start(delayMs);
    } else {
        beginArrival();
    }
}

void AgentTile::setIdentity(const QString& name, const QString& monogram) {
    name_ = name;
    nameLabel_->setText(name);
    link_->setText(QStringLiteral(u"Get %1 ↗").arg(name));
    mark_->setMonogram(monogram);
    const QString pill = QString::fromStdString(state_.pill);
    setAccessibleName(pill.isEmpty() ? name : QStringLiteral("%1, %2").arg(name, pill));
}

void AgentTile::setSelected(bool selected, bool animate) {
    if (selected == selected_) {
        return;
    }
    selected_ = selected;
    const double target = selected ? 1.0 : 0.0;
    if (animate) {
        run(liftAnim_, lift_, target, &AgentTile::onLiftValue);
    } else {
        liftAnim_->stop();
        onLiftValue(target);
    }
    setAccessibleDescription(accessibleDescriptionText());
    update();
}

double AgentTile::scanPhase() const {
    if (!clock_.isValid()) {
        return 0.0;
    }
    const double elapsed = static_cast<double>(clock_.elapsed()) +
                           (1.0 + picker::kShimmerStart) * picker::kShimmerPeriodMs -
                           shimmerOffset_;
    double phase = std::fmod(elapsed, picker::kShimmerPeriodMs);
    if (phase < 0.0) {
        phase += picker::kShimmerPeriodMs;
    }
    return phase / picker::kShimmerPeriodMs;
}

double AgentTile::scanBreath() const {
    if (shimmer_ <= 0.0) {
        return 0.0;
    }
    constexpr double kPi = 3.14159265358979323846;
    return (0.5 - 0.5 * std::cos(2.0 * kPi * scanPhase())) * shimmer_;
}

QVariantAnimation* AgentTile::makeAnimation(int durationMs, void (AgentTile::*slot)(double)) {
    auto* animation = new QVariantAnimation(this);
    animation->setDuration(durationMs);
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, slot](const QVariant& value) { (this->*slot)(value.toDouble()); });
    return animation;
}

void AgentTile::run(QVariantAnimation* animation, double start, double end,
                    void (AgentTile::*slot)(double)) {
    animation->stop();
    if (std::abs(start - end) < 1e-3) {
        (this->*slot)(end);
        return;
    }
    animation->setStartValue(start);
    animation->setEndValue(end);
    animation->start();
}

void AgentTile::beginArrival() {
    if (!pending_) {
        return;
    }
    const picker::TileState state = *pending_;
    pending_.reset();
    warmFrom_ = mark_->warmth();
    warmTo_ = state.installed ? 1.0 : 0.0;
    missingFrom_ = mark_->missing();
    applyState(state);
    arrivalAnim_->stop();
    onArrivalValue(0.0);
    arrivalAnim_->setStartValue(0.0);
    arrivalAnim_->setEndValue(1.0);
    arrivalAnim_->start();
}

void AgentTile::applyState(const picker::TileState& state) {
    state_ = state;
    const bool missing = state.tone == picker::kMissing;
    setPill(QString::fromStdString(state.tone), QString::fromStdString(state.pill));
    setDetail(QString::fromStdString(state.detail), missing, state.tone == picker::kWarning);
    link_->setVisible(missing && !installUrl_.isEmpty());
    if (nameLabel_->property(ui::prop::muted) != QVariant(missing)) {
        ui::setStyleProperty(nameLabel_, ui::prop::muted, missing);
    }
    syncInteraction();
    const QString pill = QString::fromStdString(state.pill);
    setAccessibleName(pill.isEmpty() ? name_ : QStringLiteral("%1, %2").arg(name_, pill));
    setAccessibleDescription(accessibleDescriptionText());
    update();
}

QString AgentTile::accessibleDescriptionText() const {
    QStringList parts;
    if (!state_.detail.empty()) {
        parts << QString::fromStdString(state_.detail);
    }
    if (selected_) {
        parts << QStringLiteral("Runs %1").arg(purpose_);
    }
    return parts.join(QStringLiteral(". "));
}

void AgentTile::setPill(const QString& tone, const QString& text) {
    pill_->setState(tone, text);
    pill_->setVisible(!text.isEmpty());
}

void AgentTile::setDetail(const QString& text, bool muted, bool warning) {
    detailLabel_->setText(text);
    detailLabel_->setVisible(!text.isEmpty());
    const QString tone = warning ? QStringLiteral("warning")
                         : muted ? QStringLiteral("muted")
                                 : QString();
    if (detailLabel_->property(ui::prop::tone).toString() != tone) {
        ui::setStyleProperty(detailLabel_, ui::prop::tone, tone);
    }
}

void AgentTile::syncInteraction() {
    const bool canChoose = state_.selectable;
    setCursor(canChoose ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setFocusPolicy(canChoose ? Qt::StrongFocus : Qt::NoFocus);
    if (!canChoose && hover_ > 0.0) {
        run(hoverAnim_, hover_, 0.0, &AgentTile::onHoverValue);
    }
}

void AgentTile::syncMark() {
    // Mark colour: the result's warmth, plus a breath while scanning.
    const double t = paint::easeOut(arrival_ / 0.75);
    double warm = warmFrom_ + (warmTo_ - warmFrom_) * t;
    warm = std::max(warm, 0.3 * scanBreath());
    mark_->setWarmth(warm);
    const double missingTo = state_.tone == picker::kMissing ? 1.0 : 0.0;
    mark_->setMissing(missingFrom_ + (missingTo - missingFrom_) * t);
}

void AgentTile::onHoverValue(double value) {
    hover_ = value;
    applyMargins();
    update();
}

void AgentTile::onLiftValue(double value) {
    lift_ = value;
    applyMargins();
    update();
}

void AgentTile::onShimmerValue(double value) {
    shimmer_ = value;
    syncMark();
    update();
}

void AgentTile::onArrivalValue(double value) {
    arrival_ = value;
    syncMark();
    pill_->setProgress(paint::easeOut((value - 0.12) / 0.45),
                       paint::easeInOut((value - 0.35) / 0.6));
    update();
}

double AgentTile::liftAmount() const {
    const double hover = state_.selectable ? hover_ : 0.0;
    return kLiftPx * lift_ + (1.0 - lift_) * hover;
}

void AgentTile::applyMargins() {
    // Moves the content with the card so the whole tile lifts.
    const int shift = pyRound(liftAmount());
    if (marginShift_ == shift) {
        return;
    }
    marginShift_ = shift;
    layout_->setContentsMargins(kBleedX + kPadX, kBleedTop + kPadY - shift, kBleedX + kPadX,
                                kBleedBottom + kPadY + shift);
}

QRectF AgentTile::cardRect() const {
    const QRectF rect =
        QRectF(this->rect())
            .adjusted(kBleedX + 0.5, kBleedTop + 0.5, -kBleedX - 0.5, -kBleedBottom - 0.5);
    return rect.translated(0.0, -liftAmount());
}

double AgentTile::warmth() const {
    if (state_.tone != picker::kFound) {
        return 0.0;
    }
    return paint::easeOut(arrival_ / 0.75);
}

void AgentTile::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF card = cardRect();
    const double radius = kRadius;
    const bool isDark = paint::dark();
    const std::string& tone = state_.tone;
    const QColor accent = tone == picker::kBuiltinTone ? ui::tokenColor("accent") : accent_;
    const bool blocked = selected_ && (tone == picker::kMissing || tone == picker::kWarning);
    const QColor ring = blocked ? ui::tokenColor("warning") : paint::ringColor(accent);
    const double hover = state_.selectable ? hover_ : 0.0;

    const double raised = std::max(lift_, hover * 0.6);
    if (raised > 0.0) {
        paintShadow(painter, card, raised, selected_ ? std::optional<QColor>(ring) : std::nullopt);
    }

    const QColor base = ui::tokenColor(tone == picker::kMissing ? "slate-field" : "slate-surface");
    QPainterPath path;
    path.addRoundedRect(card, radius, radius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(base);
    painter.drawPath(path);
    // Found, hovered and chosen tiles glow in their own colour, never grey.
    double wash = warmth() * (isDark ? 0.085 : 0.05);
    wash += (1.0 - lift_) * hover * (isDark ? 0.05 : 0.035);
    wash += lift_ * (isDark ? 0.07 : 0.055);
    if (wash > 0.0) {
        painter.setBrush(paint::alpha(blocked ? ring : accent, wash));
        painter.drawPath(path);
    }

    if (shimmer_ > 0.0) {
        paintShimmer(painter, path, card, accent);
    }

    if (selected_) {
        painter.setPen(QPen(ring, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
    } else {
        QColor border =
            ui::tokenColor(tone == picker::kMissing ? "slate-border-subtle" : "slate-border");
        border = paint::mix(border, paint::alpha(accent, 0.6), warmth() * 0.45);
        // While the scan looks, the edge breathes the agent's colour.
        border = paint::mix(border, paint::alpha(accent, 0.7), scanBreath() * 0.5);
        border = paint::mix(border, paint::alpha(paint::ringColor(accent), 0.85), hover * 0.8);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    if (focusVisible_ && hasFocus() && state_.selectable) {
        QPen pen(ui::tokenColor("accent-border"), 1.0);
        pen.setStyle(Qt::DotLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(card.adjusted(-2.0, -2.0, 2.0, 2.0), radius + 2, radius + 2);
    }
}

void AgentTile::paintShadow(QPainter& painter, const QRectF& card, double amount,
                            const std::optional<QColor>& glow) const {
    // A soft drop shadow, tinted by the ring colour when selected.
    painter.setPen(Qt::NoPen);
    constexpr int kLayers = 4;
    const QColor base =
        glow ? paint::alpha(*glow, (paint::dark() ? 0.17 : 0.14) * amount)
             : paint::alpha(ui::tokenColor("shadow-rgb"), (paint::dark() ? 0.30 : 0.08) * amount);
    for (int layer = 0; layer < kLayers; ++layer) {
        const double spread = 0.8 + layer * 0.9;
        painter.setBrush(
            paint::alpha(base, base.alphaF() * (1.0 - static_cast<double>(layer) / kLayers)));
        const QRectF rect = card.translated(0.0, 1.0 + 1.5 * amount)
                                .adjusted(-spread, -spread * 0.4, spread, spread);
        painter.drawRoundedRect(rect, kRadius + spread, kRadius + spread);
    }
}

void AgentTile::paintShimmer(QPainter& painter, const QPainterPath& path, const QRectF& card,
                             const QColor& accent) const {
    // A soft band of the agent's colour sweeping across the card: linear, and never fully off
    // the card, so there is no dead moment.
    const double center = -0.15 + 1.3 * scanPhase();
    constexpr double kBand = 0.32;
    const double peak = (paint::dark() ? 0.22 : 0.14) * shimmer_;
    const auto at = [&](double position) {
        const double strength = std::max(0.0, 1.0 - std::abs(position - center) / kBand);
        return paint::alpha(accent, peak * strength);
    };
    QLinearGradient gradient(card.topLeft(),
                             QPointF(card.right(), card.bottom() + card.height() * 0.8));
    const std::set<double> stops{0.0, 1.0, center - kBand, center, center + kBand};
    for (const double position : stops) {
        if (position >= 0.0 && position <= 1.0) {
            gradient.setColorAt(position, at(position));
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawPath(path);
}

bool AgentTile::event(QEvent* event) {
    const QEvent::Type kind = event->type();
    if (kind == QEvent::HoverEnter && state_.selectable) {
        run(hoverAnim_, hover_, 1.0, &AgentTile::onHoverValue);
    } else if (kind == QEvent::HoverLeave && hover_ > 0.0) {
        run(hoverAnim_, hover_, 0.0, &AgentTile::onHoverValue);
    } else if (kind == QEvent::StyleChange || kind == QEvent::PaletteChange) {
        update();
    }
    return QWidget::event(event);
}

void AgentTile::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && state_.selectable) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AgentTile::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && state_.selectable &&
        rect().contains(event->position().toPoint())) {
        Q_EMIT clicked(tileId_);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AgentTile::keyPressEvent(QKeyEvent* event) {
    if (state_.selectable && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
                              event->key() == Qt::Key_Enter)) {
        Q_EMIT clicked(tileId_);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AgentTile::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    // Ring only for keyboard focus; a click already shows the choice.
    const Qt::FocusReason reason = event->reason();
    focusVisible_ = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
                    reason == Qt::ShortcutFocusReason;
    update();
}

void AgentTile::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    update();
}

void AgentTile::openInstallPage() {
    if (!installUrl_.isEmpty()) {
        QDesktopServices::openUrl(QUrl(installUrl_));
    }
}

} // namespace fv
