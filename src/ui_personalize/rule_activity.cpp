#include "ui_personalize/rule_activity.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QEasingCurve>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPen>
#include <QPropertyAnimation>
#include <QRectF>
#include <QSizePolicy>
#include <QTimer>
#include <QVariantAnimation>

#include "base/clock.h"
#include "pyport/number.h"
#include "ui_personalize/detail/activity_glyph.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/eliding_label.h"
#include "uimath/loudness.h"

namespace fv {
namespace {

int roundedAlpha(double value) {
    return static_cast<int>(py::round_half_even(value));
}

} // namespace

std::string_view ruleActivityStateName(RuleActivityState state) {
    switch (state) {
    case RuleActivityState::Idle:
        return "idle";
    case RuleActivityState::Listening:
        return "listening";
    case RuleActivityState::Transcribing:
        return "transcribing";
    case RuleActivityState::Polishing:
        return "polishing";
    }
    return "idle";
}

QString formatRuleClock(int seconds) {
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QColor mixColors(const QColor& start, const QColor& end, double t, double alpha) {
    t = std::min(1.0, std::max(0.0, t));
    const auto channel = [t](int from, int to) {
        return static_cast<int>(py::round_half_even(from + (to - from) * t));
    };
    QColor color(channel(start.red(), end.red()), channel(start.green(), end.green()),
                 channel(start.blue(), end.blue()));
    color.setAlphaF(std::min(1.0, std::max(0.0, alpha)));
    return color;
}

// --- LevelTrace -----------------------------------------------------------------------------

LevelTrace::LevelTrace(QWidget* parent, const Clock* clock) : QWidget(parent), clock_(clock) {
    setMinimumWidth(96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

QSize LevelTrace::sizeHint() const {
    return QSize(240, 22);
}

std::vector<double> LevelTrace::barHeights() const {
    std::vector<double> heights;
    heights.reserve(bars_.size());
    for (const auto& bar : bars_) {
        heights.push_back(bar[0]);
    }
    return heights;
}

std::vector<double> LevelTrace::shownHeights() const {
    std::vector<double> heights;
    heights.reserve(bars_.size());
    for (const auto& bar : bars_) {
        heights.push_back(bar[1]);
    }
    return heights;
}

void LevelTrace::startListening() {
    bars_.clear();
    peak_ = 0.0;
    steps_ = 0;
    state_ = RuleActivityState::Listening;
    clock_.start();
    update();
}

void LevelTrace::pushLevel(double level) {
    if (state_ == RuleActivityState::Listening) {
        peak_ = std::max(peak_, level);
    }
}

void LevelTrace::hold(RuleActivityState state) {
    for (auto& bar : bars_) {
        bar[0] = bar[1];
    }
    state_ = state;
    clock_.start();
    update();
}

void LevelTrace::clear() {
    state_ = RuleActivityState::Idle;
    bars_.clear();
    update();
}

void LevelTrace::advance() {
    if (state_ == RuleActivityState::Listening && clock_.isValid()) {
        const auto due = static_cast<std::int64_t>(std::floor(clock_.elapsed() / kStepMs));
        while (steps_ < due) {
            bars_.push_back({level_to_height(peak_), 0.0});
            if (bars_.size() > kMaxBars) {
                bars_.pop_front();
            }
            peak_ = 0.0;
            ++steps_;
        }
    }
    for (auto& bar : bars_) {
        bar[1] += (bar[0] - bar[1]) * kEase;
    }
    update();
}

void LevelTrace::paintEvent(QPaintEvent*) {
    if (state_ == RuleActivityState::Idle) {
        return;
    }
    const QRectF rect(this->rect());
    if (rect.width() <= 0 || rect.height() <= 0) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);

    const auto slots = static_cast<std::size_t>(std::floor(rect.width() / kBarPitch)) + 1;
    std::vector<double> shown;
    shown.reserve(slots);
    for (auto bar = bars_.rbegin(); bar != bars_.rend() && shown.size() < slots; ++bar) {
        shown.push_back((*bar)[1]);
    }
    // Silence before the first bar is the dotted baseline, so a new take reads as a track
    // filling up rather than a half-empty strip.
    shown.resize(slots, 0.0);
    const bool listening = state_ == RuleActivityState::Listening;
    const std::int64_t elapsed = clock_.isValid() ? clock_.elapsed() : 0;
    double sweep = 0.0;
    double band = 0.0;
    double offset = 0.0;
    if (listening) {
        // Slide left between steps so the trace moves continuously.
        offset = std::fmod(static_cast<double>(elapsed), kStepMs) / kStepMs * kBarPitch;
    } else {
        band = std::max(rect.width() * 0.3, 48.0);
        const double phase = static_cast<double>(elapsed % kSweepMs) / kSweepMs;
        const double eased = phase * phase * (3.0 - 2.0 * phase);
        sweep = rect.left() - band + (rect.width() + 2.0 * band) * eased;
    }

    const QColor start = ui::tokenColor("accent");
    const QColor end = ui::tokenColor("accent-cyan");
    const double fade = rect.width() * 0.3;
    const double middle = rect.center().y();
    const double tallest = rect.height() - 2.0;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        double height = shown[i];
        const double right = rect.right() - static_cast<double>(i) * kBarPitch - offset;
        const double left = right - kBarWidth;
        if (right < rect.left()) {
            break;
        }
        // Older bars fade out toward the left edge.
        double alpha = std::min(1.0, std::max(0.0, (left - rect.left()) / fade));
        if (!listening) {
            const double center = left + kBarWidth / 2.0;
            const double glow = std::max(0.0, 1.0 - std::abs(center - sweep) / (band / 2.0));
            alpha *= 0.35 + 0.65 * glow;
            height = std::max(height, 0.3 * glow);
        }
        if (alpha <= 0.01) {
            continue;
        }
        const double bar = std::max(kBarWidth, height * tallest);
        painter.setBrush(mixColors(start, end, (left - rect.left()) / rect.width(), alpha));
        painter.drawRoundedRect(QRectF(left, middle - bar / 2.0, kBarWidth, bar), kBarWidth / 2.0,
                                kBarWidth / 2.0);
    }
}

// --- RuleActivityStrip ----------------------------------------------------------------------

RuleActivityStrip::RuleActivityStrip(int capSeconds, QWidget* parent, const Clock* clock)
    : QWidget(parent), capSeconds_(std::max(1, capSeconds)), listenClock_(clock) {
    setObjectName(QStringLiteral("ruleActivityHost"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    panel_ = new QFrame(this);
    panel_->setObjectName(QStringLiteral("ruleActivityStrip"));
    ui::setStyleProperty(panel_, ui::prop::state,
                         QString::fromLatin1(ruleActivityStateName(RuleActivityState::Idle)));
    auto* row = new QHBoxLayout(panel_);
    row->setContentsMargins(12, 7, 12, 7);
    row->setSpacing(10);
    glyph_ = new detail::ActivityGlyph(panel_, clock);
    row->addWidget(glyph_, 0, Qt::AlignVCenter);
    title_ = new QLabel(panel_);
    title_->setObjectName(QStringLiteral("ruleActivityTitle"));
    row->addWidget(title_);
    // The detail keeps its text width and elides only when space runs out; the trace takes
    // everything else.
    detail_ = new ElidingLabel(QString(), Qt::ElideRight, panel_);
    detail_->setObjectName(QStringLiteral("ruleActivityDetail"));
    row->addWidget(detail_);
    trace_ = new LevelTrace(panel_, clock);
    row->addWidget(trace_, 1);
    time_ = new QLabel(panel_);
    time_->setObjectName(QStringLiteral("ruleActivityTime"));
    time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(time_);
    // The panel is placed by hand, so its size changes (a font scale change, say) have to be
    // passed on to this host's layout.
    panel_->installEventFilter(this);

    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });
    revealTimer_ = new QTimer(this);
    revealTimer_->setSingleShot(true);
    revealTimer_->setInterval(kWorkRevealDelayMs);
    connect(revealTimer_, &QTimer::timeout, this, [this] { open(); });
    heightAnim_ = createMaxHeightAnimation(this, this);
    setMaximumHeight(0);
    hide();
}

RuleActivityStrip::~RuleActivityStrip() = default;

QSize RuleActivityStrip::sizeHint() const {
    const QSize panel = panel_->sizeHint();
    return QSize(panel.width(), kGap + panel.height());
}

QSize RuleActivityStrip::minimumSizeHint() const {
    return QSize(panel_->minimumSizeHint().width(), 0);
}

void RuleActivityStrip::showListening(const QString& detail) {
    revealTimer_->stop();
    enter(RuleActivityState::Listening, QStringLiteral("Listening"), detail);
    trace_->startListening();
    listenClock_.start();
    nearCap_ = false;
    repolishClock(false);
    time_->show();
    updateClock();
    open();
}

void RuleActivityStrip::showTranscribing(const QString& detail) {
    showWork(RuleActivityState::Transcribing, QStringLiteral("Transcribing"), detail);
}

void RuleActivityStrip::showPolishing(const QString& detail) {
    showWork(RuleActivityState::Polishing, QStringLiteral("Polishing with AI"), detail);
}

void RuleActivityStrip::pushLevel(double level) {
    trace_->pushLevel(level);
}

void RuleActivityStrip::finish(bool animate) {
    revealTimer_->stop();
    if (isHidden()) {
        reset();
        return;
    }
    closing_ = true;
    timer_->stop();
    if (!animate) {
        heightAnim_->stop();
        onClosed();
        return;
    }
    runMaxHeightAnimation(heightAnim_, std::min(maximumHeight(), height()), 0,
                          [this] { onClosed(); });
}

void RuleActivityStrip::showWork(RuleActivityState state, const QString& title,
                                 const QString& detail) {
    enter(state, title, detail);
    trace_->hold(state);
    time_->hide();
    if (closing_) {
        open();
    } else if (isHidden()) {
        revealTimer_->start();
    }
}

void RuleActivityStrip::enter(RuleActivityState state, const QString& title,
                              const QString& detail) {
    state_ = state;
    title_->setText(title);
    detail_->setText(detail);
    glyph_->setState(state);
    ui::setStyleProperty(panel_, ui::prop::state,
                         QString::fromLatin1(ruleActivityStateName(state)));
    setAccessibleName(title);
    setAccessibleDescription(detail);
    syncTimer();
}

void RuleActivityStrip::open() {
    if (state_ == RuleActivityState::Idle) {
        return;
    }
    closing_ = false;
    if (isHidden()) {
        setMaximumHeight(0);
        show();
    }
    placePanel();
    syncTimer();
    const int target = sizeHint().height();
    const int current = std::min(maximumHeight(), height());
    const bool running = heightAnim_->state() == QAbstractAnimation::Running;
    if (current >= target && !running) {
        setMaximumHeight(kUnlimitedHeight);
        return;
    }
    runMaxHeightAnimation(heightAnim_, current, target, [this] { onOpened(); });
}

void RuleActivityStrip::onOpened() {
    // Unbounded once open, so a larger font can still grow the panel.
    setMaximumHeight(kUnlimitedHeight);
}

void RuleActivityStrip::onClosed() {
    hide();
    setMaximumHeight(0);
    closing_ = false;
    reset();
}

void RuleActivityStrip::reset() {
    state_ = RuleActivityState::Idle;
    trace_->clear();
    glyph_->setState(RuleActivityState::Idle);
    ui::setStyleProperty(panel_, ui::prop::state,
                         QString::fromLatin1(ruleActivityStateName(RuleActivityState::Idle)));
    syncTimer();
}

void RuleActivityStrip::syncTimer() {
    if (state_ != RuleActivityState::Idle && isVisible() && !closing_) {
        if (!timer_->isActive()) {
            timer_->start();
        }
    } else {
        timer_->stop();
    }
}

void RuleActivityStrip::tick() {
    trace_->advance();
    glyph_->update();
    if (state_ == RuleActivityState::Listening) {
        updateClock();
    }
}

void RuleActivityStrip::updateClock() {
    std::int64_t elapsed = 0;
    if (listenClock_.isValid()) {
        elapsed = std::min<std::int64_t>(listenClock_.elapsed() / 1000, capSeconds_);
    }
    const QString text = QStringLiteral("%1 / %2").arg(formatRuleClock(static_cast<int>(elapsed)),
                                                       formatRuleClock(capSeconds_));
    if (time_->text() != text) {
        time_->setText(text);
    }
    const bool nearStop = capSeconds_ - elapsed <= kCapWarningSeconds;
    if (nearStop != nearCap_) {
        nearCap_ = nearStop;
        repolishClock(nearStop);
    }
}

void RuleActivityStrip::repolishClock(bool nearStop) {
    ui::setStyleProperty(time_, ui::prop::nearCap, nearStop);
}

void RuleActivityStrip::placePanel() {
    panel_->setGeometry(0, kGap, width(), panel_->sizeHint().height());
}

bool RuleActivityStrip::eventFilter(QObject* watched, QEvent* event) {
    if (watched == panel_ && event->type() == QEvent::LayoutRequest) {
        updateGeometry();
        placePanel();
    }
    return QWidget::eventFilter(watched, event);
}

void RuleActivityStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placePanel();
}

void RuleActivityStrip::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncTimer();
}

void RuleActivityStrip::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_->stop();
}

// --- ItemGlow -------------------------------------------------------------------------------

ItemGlow::ItemGlow(QListWidget* view) : QWidget(view->viewport()), view_(view) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    anim_ = new QVariantAnimation(this);
    anim_->setDuration(kGlowMs);
    anim_->setStartValue(1.0);
    anim_->setEndValue(0.0);
    // Holds near full strength, then fades quickly.
    anim_->setEasingCurve(QEasingCurve::InQuad);
    connect(anim_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { onValue(value); });
    connect(anim_, &QAbstractAnimation::finished, this, &QWidget::hide);
    hide();
}

void ItemGlow::flash(QListWidgetItem* item) {
    index_ = QPersistentModelIndex(view_->model()->index(view_->row(item), 0));
    view_->scrollToItem(item);
    anim_->stop();
    strength_ = 1.0;
    followViewport();
    show();
    raise();
    anim_->start();
}

void ItemGlow::onValue(const QVariant& value) {
    strength_ = value.toDouble();
    followViewport();
    update();
}

void ItemGlow::followViewport() {
    const QRect rect = view_->viewport()->rect();
    if (geometry() != rect) {
        setGeometry(rect);
    }
}

void ItemGlow::paintEvent(QPaintEvent*) {
    if (strength_ <= 0.0 || !index_.isValid()) {
        return;
    }
    QRectF rect(view_->visualRect(QModelIndex(index_)));
    if (rect.isEmpty()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // Matches the rule rows' 9px radius and 1px border in theme.qss.
    rect = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(ui::tokenColor("accent", roundedAlpha(255 * strength_)), 1.0));
    painter.setBrush(ui::tokenColor("accent", roundedAlpha(70 * strength_)));
    painter.drawRoundedRect(rect, kRadius, kRadius);
}

} // namespace fv
