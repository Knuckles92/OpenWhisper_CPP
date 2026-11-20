#include "ui_overlay/waveform_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <string>

#include <QCloseEvent>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QTimer>

#include "base/clock.h"
#include "base/log.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "ui_overlay/classic_look.h"
#include "ui_overlay/live_label_painter.h"
#include "ui_overlay/visual.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/drag_move.h"
#include "uimath/easing.h"
#include "uimath/moments.h"
#include "uimath/overlay_look.h"
#include "uimath/overlay_position.h"

namespace fv {
namespace {

using ui::tokenColor;

constexpr log::Logger kLog{"ui_qt.overlays.waveform_overlay"};

// config.WAVEFORM_FRAME_RATE.
constexpr int kFrameRate = 30;
// How fast a finish runs in each stage; the step to Transcribing eases up.
constexpr double kProcessingRate = 1.0;
constexpr double kTranscribingRate = 1.7;
constexpr double kWorkEaseS = 0.3;

bool isListening(const QString& state) {
    return state == WaveformOverlay::kStateRecording || state == WaveformOverlay::kStateStreaming ||
           state == WaveformOverlay::kStateCommandListening;
}

// The states that can show live preview words.
bool isPreviewState(const QString& state) {
    return state == WaveformOverlay::kStateStreaming ||
           state == WaveformOverlay::kStateCommandListening;
}

bool isTransient(const QString& state) {
    return state == WaveformOverlay::kStateSttEnable ||
           state == WaveformOverlay::kStateSttDisable || state == WaveformOverlay::kStateCopied ||
           state == WaveformOverlay::kStateLanguage || state == WaveformOverlay::kStateNotice;
}

// Each look draws its own Processing and Transcribing.
bool isWork(const QString& state) {
    return state == WaveformOverlay::kStateProcessing ||
           state == WaveformOverlay::kStateTranscribing;
}

// States that fade out rather than vanish when the overlay hides.
bool isFading(const QString& state) {
    return isWork(state) || state == WaveformOverlay::kStateCleaning ||
           state == WaveformOverlay::kStateRewriting;
}

// ParticleStyle simulates only its own states; the command and rewrite looks borrow theirs.
QString styleState(const QString& state) {
    if (state == WaveformOverlay::kStateCommandListening) {
        return WaveformOverlay::kStateRecording;
    }
    if (state == WaveformOverlay::kStateRewriting) {
        return WaveformOverlay::kStateCleaning;
    }
    return state;
}

QFont segoe(int pointSize, QFont::Weight weight = QFont::Normal) {
    return QFont(QStringLiteral("Segoe UI"), pointSize, weight);
}

Rect toRect(const QRect& rect) {
    return Rect{rect.x(), rect.y(), rect.width(), rect.height()};
}

QRect toQRect(const Rect& rect) {
    return QRect(rect.x, rect.y, rect.width, rect.height);
}

std::mt19937 seededRng() {
    std::random_device device;
    return std::mt19937(device());
}

PreviewLayout toLayout(const std::string& stored) {
    return stored == "footer" ? PreviewLayout::Footer : PreviewLayout::Header;
}

QString fromUtf8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

WaveformOverlay::WaveformOverlay(const SettingsStore* settings, const Clock* clock)
    : QWidget(nullptr), settings_(settings), clock_(clock != nullptr ? clock : &system_clock()),
      rng_(seededRng()), overlayWidth_(kOverlayWidth), overlayHeight_(kOverlayBaseHeight),
      baseHeight_(kOverlayBaseHeight) {
    init();
}

WaveformOverlay::WaveformOverlay(QWidget* parent, Mode mode, const SettingsStore* settings,
                                 const Clock* clock)
    : QWidget(mode == Mode::Preview ? parent : nullptr), settings_(settings),
      clock_(clock != nullptr ? clock : &system_clock()), rng_(seededRng()),
      preview_(mode == Mode::Preview), overlayWidth_(kOverlayWidth),
      overlayHeight_(kOverlayBaseHeight), baseHeight_(kOverlayBaseHeight) {
    init();
}

void WaveformOverlay::init() {
    if (preview_) {
        setWindowFlags(Qt::Widget);
    } else {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                       Qt::WindowDoesNotAcceptFocus);
    }
    setAttribute(Qt::WA_TranslucentBackground, !preview_);
    setFocusPolicy(Qt::NoFocus);
    if (preview_) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    } else {
        // The paste goes to whichever app has focus, so neither showing nor clicking the
        // overlay may take it (WS_EX_NOACTIVATE on Windows). It can be dragged aside, and opens
        // by the pointer again each time it is shown afresh.
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);
        drag_ = std::make_unique<DragToMove>(this);
    }

    setFixedSize(overlayWidth_, overlayHeight_);

    const std::shared_ptr<const Json> saved = savedSettings();
    streamingFontSize_ = keys::streaming_overlay_font_size.read(*saved);
    streamingLayout_ = toLayout(keys::streaming_overlay_layout.read(*saved));

    style_ = std::make_unique<ParticleStyle>(overlayWidth_, overlayHeight_,
                                             overlay_particle_config(), seededRng());
    legacyAudioResponse_ = style_->config.audio_response;
    listenStarted_ = now();
    defaultEmissionRate_ = style_->config.emission_rate;
    recordingLook_ = fromUtf8(kLookLegacy);
    refreshRecordingLook();

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &WaveformOverlay::updateAnimation);
    frameRate_ = kFrameRate;
    lastFrameTime_ = now();

    hiddenTimer_ = new QTimer(this);
    hiddenTimer_->setSingleShot(true);
    connect(hiddenTimer_, &QTimer::timeout, this, &WaveformOverlay::hide);

    captionTimer_ = new QTimer(this);
    captionTimer_->setSingleShot(true);
    connect(captionTimer_, &QTimer::timeout, this, &WaveformOverlay::clearCaption);
}

WaveformOverlay::~WaveformOverlay() = default;

double WaveformOverlay::now() const {
    return std::chrono::duration<double>(clock_->steady().time_since_epoch()).count();
}

std::shared_ptr<const Json> WaveformOverlay::savedSettings() const {
    return settings_ != nullptr ? settings_->snapshot()
                                : std::make_shared<const Json>(Json::object());
}

// --- painting -------------------------------------------------------------------------------

void WaveformOverlay::paintEvent(QPaintEvent* /*event*/) {
    try {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (fadingSince_) {
            const double faded = (now() - *fadingSince_) / kFadeOutS;
            painter.setOpacity(std::max(0.0, 1.0 - faded));
        }
        drawBackground(painter);
        drawState(painter);
        drawListeningExtras(painter);
    } catch (const std::exception& error) {
        // paintEvent runs every frame: log the first failure only.
        if (!paintErrorLogged_) {
            paintErrorLogged_ = true;
            kLog.error_with(error, "Error drawing waveform frame");
        }
        try {
            QPainter painter(this);
            painter.fillRect(rect(), tokenColor("overlay-bg"));
            painter.setPen(QPen(tokenColor("overlay-text")));
            painter.setFont(segoe(10, QFont::Bold));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Error"));
        } catch (...) {
            // The error frame is cosmetic; the failure is logged above.
        }
    }
}

void WaveformOverlay::drawState(QPainter& painter) {
    const QString& state = currentState_;
    if (isWork(state)) {
        drawWork(painter);
    } else if (state == kStateRecording) {
        drawListeningBand(painter, QStringLiteral("Recording..."), "overlay-text", bandTop());
    } else if (state == kStateStreaming) {
        drawStreamingState(painter, QStringLiteral("Listening..."), "overlay-text");
    } else if (state == kStateCommandListening) {
        drawStreamingState(painter, QStringLiteral("Listening for an edit..."), "accent-soft");
    } else if (state == kStateCleaning) {
        drawCleaningState(painter, QStringLiteral("Cleaning up..."));
    } else if (state == kStateRewriting) {
        drawCleaningState(painter, QStringLiteral("Rewriting..."));
    } else if (state == kStateCanceling) {
        drawCanceled(painter);
    } else if (state == kStateSttEnable) {
        drawMoment(painter, QStringLiteral("Enabled"));
    } else if (state == kStateSttDisable) {
        drawMoment(painter, QStringLiteral("Disabled"));
    } else if (state == kStateCopied) {
        drawMoment(painter, QStringLiteral("Copied!"));
    } else if (state == kStateLanguage) {
        drawLanguageState(painter);
    } else if (state == kStateNotice) {
        drawNoticeState(painter);
    } else if (state == kStateLargeFileSplitting) {
        drawLargeFileSplittingState(painter);
    }
}

void WaveformOverlay::drawStreamingState(QPainter& painter, const QString& status,
                                         std::string_view statusToken) {
    // Once words arrive, the layout setting puts the look in a slim header over them or keeps
    // it full size under them.
    if (streamingText_.isEmpty()) {
        drawListeningBand(painter, status, statusToken, bandTop());
        return;
    }
    const Margins margins = previewTextMargins();
    if (streamingLayout_ == PreviewLayout::Footer) {
        const int top = footer_band_top(height(), baseHeight_);
        drawDivider(painter, top);
        drawListeningBand(painter, status, statusToken, top);
    } else {
        drawPreviewHeader(painter, statusToken);
        drawDivider(painter, headerHeight());
    }
    drawStreamingPreviewText(painter, toQRect(preview_text_rect(width(), height(), margins)));
}

void WaveformOverlay::drawListeningBand(QPainter& painter, const QString& status,
                                        std::string_view statusToken, double top) {
    if (visual_ != nullptr) {
        QString label = status;
        while (label.endsWith(u'.')) {
            label.chop(1);
        }
        drawLiveListening(painter, label, statusToken, top);
        return;
    }
    drawParticles(painter, top);
    if (caption_.isEmpty()) {
        drawStatus(painter, status, tokenColor(statusToken));
    }
}

void WaveformOverlay::drawParticles(QPainter& painter, double top) {
    // The recording particles stay in the compact band, under anything above it.
    painter.save();
    if (top != 0.0) {
        painter.setClipRect(QRectF(0, top, width(), baseHeight_));
        painter.translate(0, top);
    }
    drawStyleState(painter, *style_, QRect(0, 0, width(), baseHeight_), QString());
    painter.restore();
}

std::string_view WaveformOverlay::liveTone() const {
    // Accent for a command or a destination, else recording red.
    const bool command = currentState_ == kStateCommandListening;
    return command || !recordingDestination_.isEmpty() ? "accent" : "danger";
}

void WaveformOverlay::drawLiveListening(QPainter& painter, const QString& label,
                                        std::string_view textTone, double top) {
    const double elapsed = now() - listenStarted_;
    const std::string_view tone = liveTone();
    if (currentState_ != kStateCommandListening) {
        drawEdgeFlash(painter, QRectF(rect()), elapsed, 0.0, tone);
    }
    const double grow = ease_out_cubic(phase(elapsed, 0.05, 0.4));
    painter.save();
    if (top != 0.0) {
        painter.setClipRect(QRectF(0, top, width(), baseHeight_));
        painter.translate(0, top);
    }
    visual_->draw(painter, grow, width());
    painter.restore();
    if (visual_->particles()) {
        drawParticles(painter, top);
    }
    const LabelParts parts = labelParts_;
    if (caption_.isEmpty() && (parts.dot || parts.text || parts.clock)) {
        drawLiveLabel(painter, QRectF(statusRect().adjusted(12, 0, -12, 0)), label, elapsed, tone,
                      textTone, parts.dot, parts.text, parts.clock);
    }
}

void WaveformOverlay::drawPreviewHeader(QPainter& painter, std::string_view textTone) {
    // "● 0:04" and a small copy of the look, between the badge and the chips; when the badge row
    // has no room for them they take a line of their own under it.
    const double elapsed = now() - listenStarted_;
    const LabelParts parts = labelParts_;
    HeaderRowInput input;
    input.width = width();
    input.shares_top_row = liveSharesTopRow();
    input.top_row_span = top_row_span(topRowInput());
    input.dot = parts.dot;
    input.clock = parts.clock;
    input.live = visual_ != nullptr;
    input.label_width = wordlessLabelWidth(elapsed, parts.dot, parts.clock);
    input.label_width_without_clock = wordlessLabelWidth(elapsed, parts.dot, false);
    input.caption = !caption_.isEmpty();
    const HeaderRow row = header_row(input);
    headerRow_ = row;
    if (row.draw_label) {
        const RectD& at = row.label_rect;
        drawLiveLabel(painter, QRectF(at.x, at.y, at.width, at.height), QString(), elapsed,
                      liveTone(), textTone, parts.dot, false, row.label_clock);
    }
    headerRoom_ = QRectF(row.room.x, row.room.y, row.room.width, row.room.height);
    if (row.draw_look) {
        drawSmallLook(painter, headerRoom_);
    }
}

void WaveformOverlay::drawSmallLook(QPainter& painter, const QRectF& area) {
    // A live look leaves its particles out at this size; Classic, which is only particles,
    // shrinks the whole band to fit.
    const std::string_view small = visual_ != nullptr ? visual_->small() : kSmallFit;
    const SmallLook look =
        small_look(small, RectD{area.x(), area.y(), area.width(), area.height()}, width());
    painter.save();
    painter.setClipRect(area);
    painter.translate(area.left(), area.top());
    if (small == kSmallFit) {
        painter.translate(look.origin_x - area.left(), look.origin_y - area.top());
    }
    painter.scale(look.scale_x, look.scale_y);
    if (visual_ == nullptr) {
        drawParticles(painter, 0.0);
        painter.restore();
        return;
    }
    // The look draws at the room's width (in its own units) or, fitting, at the overlay's; the
    // width is an argument, so the look keeps nothing of it.
    const double drawWidth = look.band_width.value_or(width());
    const double grow = ease_out_cubic(phase(now() - listenStarted_, 0.05, 0.4));
    visual_->draw(painter, grow, drawWidth);
    painter.restore();
}

void WaveformOverlay::drawDivider(QPainter& painter, double y) {
    // A hairline between the preview words and the look.
    painter.setPen(QPen(tokenColor("overlay-border"), 1));
    painter.drawLine(QPointF(kTextMargin, y + 0.5), QPointF(width() - kTextMargin, y + 0.5));
}

QFont WaveformOverlay::streamingPreviewFont() const {
    return segoe(streamingFontSize_);
}

void WaveformOverlay::drawStreamingPreviewText(QPainter& painter, const QRect& textRect) {
    // Aligned to the bottom, so the newest words stay in sight when the text is taller than the
    // overlay's height cap.
    painter.setPen(QPen(tokenColor("overlay-text")));
    painter.setFont(streamingPreviewFont());
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, streamingText_);
}

void WaveformOverlay::drawWork(QPainter& painter) {
    // Processing or Transcribing: the look's own finish, or Classic's vortex.
    const QRect band(0, 0, width(), baseHeight_);
    painter.save();
    painter.translate(0, bandTop());
    if (visual_ != nullptr) {
        const double morph = smooth((now() - workStarted_) / kMorphS);
        visual_->drawWork(painter, morph, workClock_, width());
    } else {
        drawStyleState(painter, *style_, band, QString());
    }
    painter.restore();
    if (caption_.isEmpty()) {
        drawWorkLabel(painter);
    }
}

void WaveformOverlay::drawWorkLabel(QPainter& painter) {
    // The stage's name; a new stage's rises in as the old one lifts away.
    double shown = 1.0;
    if (!labelFrom_.isEmpty()) {
        shown = smooth((now() - labelChanged_) / kLabelCrossfadeS);
        if (shown < 1.0) {
            drawLabelAt(painter, labelFrom_, 1.0 - shown, -6.0 * shown);
        }
    }
    drawLabelAt(painter, workLabel(), shown, 6.0 * (1.0 - shown));
}

QString WaveformOverlay::workLabel() const {
    if (currentState_ == kStateProcessing && !processingLabel_.isEmpty()) {
        return processingLabel_;
    }
    if (currentState_ == kStateProcessing) {
        return QStringLiteral("Processing...");
    }
    if (currentState_ == kStateTranscribing) {
        return QStringLiteral("Transcribing...");
    }
    return QString();
}

void WaveformOverlay::setProcessingLabel(const QString& text) {
    processingLabel_ = text;
    update();
}

void WaveformOverlay::drawLabelAt(QPainter& painter, const QString& text, double alpha, double dy) {
    painter.setPen(QPen(tokenColor("overlay-text", static_cast<int>(255 * alpha))));
    painter.setFont(segoe(10, QFont::Bold));
    painter.drawText(QRectF(statusRect()).translated(0, dy), Qt::AlignCenter, text);
}

QRect WaveformOverlay::statusRect() const {
    // The label row, under the look wherever the preview layout put it; with the header, a
    // caption takes the room its small look had.
    const RectD room{headerRoom_.x(), headerRoom_.y(), headerRoom_.width(), headerRoom_.height()};
    return toQRect(status_rect(!streamingText_.isEmpty(), streamingLayout_, width(), height(),
                               bandTop(), baseHeight_, room));
}

void WaveformOverlay::drawStatus(QPainter& painter, const QString& text, const QColor& color) {
    painter.setPen(QPen(color));
    painter.setFont(segoe(10, QFont::Bold));
    const QRect area = statusRect().adjusted(12, 0, -12, 0);
    const QString shown =
        QFontMetrics(painter.font()).elidedText(text, Qt::ElideRight, area.width());
    painter.drawText(area, Qt::AlignCenter, shown);
}

void WaveformOverlay::drawBackground(QPainter& painter) {
    const bool command = currentState_ == kStateCommandListening;
    const bool accented = command || !recordingDestination_.isEmpty();
    // Inset by half the pen width so the 1 px border isn't clipped.
    const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(area, 12, 12);
    painter.fillPath(path, tokenColor("overlay-bg"));
    painter.setPen(QPen(tokenColor("overlay-border"), 1));
    painter.drawPath(path);
    if (accented) {
        QPainterPath ring;
        ring.addRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 11.5, 11.5);
        painter.setPen(QPen(tokenColor("accent", 210), 1.6));
        painter.drawPath(ring);
    }
}

bool WaveformOverlay::badgeShown() const {
    // The destination, Hands-free or Command Mode badge.
    const bool latched = handsFree_ || currentState_ == kStateCommandListening;
    return !recordingDestination_.isEmpty() || (isListening(currentState_) && latched);
}

TopRowInput WaveformOverlay::topRowInput() const {
    TopRowInput input;
    input.width = width();
    input.badge_shown = badgeShown();
    input.badge_width = input.badge_shown ? badgeWidth() : 0.0;
    input.listening = isListening(currentState_);
    input.chip_visible = chipVisible();
    input.chip_width = input.chip_visible ? chipWidth() : 0.0;
    input.controls_clickable = controlsClickable();
    if (input.controls_clickable) {
        double left = width();
        for (const PlacedControl& placed : layoutControls()) {
            left = std::min(left, placed.rect.left());
        }
        input.controls_left = left;
    }
    return input;
}

bool WaveformOverlay::topRowInUse() const {
    return top_row_in_use(topRowInput());
}

int WaveformOverlay::bandTop() const {
    return band_top(topRowInput());
}

std::optional<double> WaveformOverlay::liveLabelWidth() const {
    const LabelParts parts = labelParts_;
    if (visual_ != nullptr && (parts.dot || parts.clock)) {
        // At its widest clock, so the header does not jump as the seconds run.
        return wordlessLabelWidth(0.0, parts.dot, parts.clock);
    }
    return std::nullopt;
}

bool WaveformOverlay::liveSharesTopRow() const {
    return live_shares_top_row(topRowInput(), liveLabelWidth());
}

int WaveformOverlay::headerHeight() const {
    return header_height(topRowInput(), liveLabelWidth());
}

Margins WaveformOverlay::previewTextMargins() const {
    return preview_text_margins(streamingLayout_, baseHeight_, topRowInUse(), headerHeight());
}

void WaveformOverlay::drawListeningExtras(QPainter& painter) {
    badgeRect_ = QRectF();
    if (badgeShown()) {
        drawModeBadge(painter);
    }
    if (!isListening(currentState_)) {
        chipRect_ = QRectF();
        controlRects_.clear();
        if (!caption_.isEmpty()) {
            drawCaption(painter);
        }
        return;
    }
    chipRect_ = chipVisible() ? drawLanguageChip(painter) : QRectF();
    controlRects_.clear();
    if (controlsClickable()) {
        drawWalkthroughControls(painter);
    }
    if (!caption_.isEmpty()) {
        drawCaption(painter);
    }
}

QString WaveformOverlay::badgeText() const {
    if (currentState_ == kStateCommandListening) {
        return QStringLiteral("Command Mode");
    }
    return recordingDestination_.isEmpty() ? QStringLiteral("Hands-free") : recordingDestination_;
}

double WaveformOverlay::badgeWidth() const {
    const int lock = handsFree_ ? 8 + 5 : 0;
    return QFontMetrics(segoe(8, QFont::DemiBold)).horizontalAdvance(badgeText()) + 18 + lock;
}

namespace {

void drawLock(QPainter& painter, double x, double cy, const QColor& color) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(x, cy - 1.0, 8.0, 6.0), 1.5, 1.5);
    QPainterPath shackle;
    shackle.moveTo(x + 1.9, cy - 1.0);
    shackle.lineTo(x + 1.9, cy - 3.0);
    shackle.arcTo(QRectF(x + 1.9, cy - 5.9, 4.2, 5.8), 180, -180);
    shackle.lineTo(x + 6.1, cy - 1.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(roundPen(color, 1.4));
    painter.drawPath(shackle);
}

} // namespace

void WaveformOverlay::drawModeBadge(QPainter& painter) {
    const QString text = badgeText();
    const QFont font = segoe(8, QFont::DemiBold);
    const int lockWidth = handsFree_ ? 8 : 0;
    const QRectF badge(kBadgeMargin, kBadgeMargin, badgeWidth(), kBadgeHeight);
    badgeRect_ = badge;
    const double radius = kBadgeHeight / 2.0;
    const QColor ink = tokenColor("accent-soft");
    painter.setPen(Qt::NoPen);
    // Opaque first so particles drifting behind never show through the text.
    painter.setBrush(tokenColor("overlay-bg"));
    painter.drawRoundedRect(badge, radius, radius);
    painter.setBrush(tokenColor("accent", 46));
    painter.drawRoundedRect(badge, radius, radius);
    double x = badge.left() + 9;
    if (handsFree_) {
        drawLock(painter, x, badge.center().y(), ink);
        x += lockWidth + 5;
    }
    painter.setPen(QPen(ink));
    painter.setFont(font);
    painter.drawText(QRectF(x, badge.top(), badge.right() - x, badge.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, text);
}

// --- the language chip ----------------------------------------------------------------------

QString WaveformOverlay::languageText() const {
    return QString::fromStdString(speech::short_label(language_.toStdString()));
}

QString WaveformOverlay::languageName() const {
    return QString::fromStdString(speech::label(language_.toStdString()));
}

double WaveformOverlay::languageFlash() const {
    if (!languageChangedAt_) {
        return 0.0;
    }
    return std::max(0.0, 1.0 - (now() - *languageChangedAt_) / kLanguageFlashS);
}

double WaveformOverlay::chipWidth() const {
    return std::max(30, QFontMetrics(segoe(8, QFont::Bold)).horizontalAdvance(languageText()) + 18);
}

QRectF WaveformOverlay::drawLanguageChip(QPainter& painter) {
    const QFont font = segoe(8, QFont::Bold);
    const QString text = languageText();
    const double chip = chipWidth();
    const QRectF area(width() - kBadgeMargin - chip, kBadgeMargin, chip, kBadgeHeight);
    const double radius = kBadgeHeight / 2.0;
    const double flash = languageFlash();
    const bool hover = hover_ == QStringLiteral("language") && chipClickable();
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenColor("overlay-bg"));
    painter.drawRoundedRect(area, radius, radius);
    painter.setBrush(tokenColor("overlay-rgb", hover ? 38 : 22));
    painter.drawRoundedRect(area, radius, radius);
    if (flash != 0.0) {
        painter.setBrush(tokenColor("accent", static_cast<int>(170 * flash)));
        painter.drawRoundedRect(area, radius, radius);
    }
    const QColor edge =
        hover || flash != 0.0 ? tokenColor("accent", 220) : tokenColor("overlay-border");
    painter.setPen(QPen(edge, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(area.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.setPen(QPen(flash > 0.5 ? tokenColor("on-accent") : tokenColor("overlay-text")));
    painter.setFont(font);
    painter.drawText(area, Qt::AlignCenter, text);
    return area;
}

void WaveformOverlay::drawCaption(QPainter& painter) {
    const double elapsed = now() - captionStarted_;
    const double remaining = kCaptionMs / 1000.0 - elapsed;
    const double fade = std::max(0.0, std::min({1.0, elapsed / 0.15, remaining / 0.35}));
    drawStatus(painter, caption_, tokenColor("warning-text", static_cast<int>(255 * fade)));
}

void WaveformOverlay::drawLanguageState(QPainter& painter) {
    // The language shortcut's notice: the new language's chip pops in. It plays the moments'
    // ring, edge flash, rising label and fade, with the chip in place of the disc.
    const double t = animationTime_;
    painter.setOpacity(exit_opacity(t, transientMs() / 1000.0));
    const double grow = phase(t, 0.0, kPopS);
    const double scale = 0.6 + 0.4 * ease_out_back(grow);
    QFont font = segoe(13, QFont::Bold);
    const QString text = languageText();
    const double fullWidth = std::max(54, QFontMetrics(font).horizontalAdvance(text) + 28);
    const double chip = fullWidth * scale;
    const double tall = 30 * scale;
    const QRectF area((width() - chip) / 2, 30 - tall / 2, chip, tall);
    const double radius = tall / 2;

    const double ring = phase(t, 0.06, kRingS);
    if (0.0 < ring && ring < 1.0) {
        // The ring follows the chip's own shape as it rolls out.
        const double spread = 12.0 * ease_out_cubic(ring);
        const double fade = (1.0 - ring) * (1.0 - ring);
        const QRectF outer = QRectF((width() - fullWidth) / 2, 15, fullWidth, 30)
                                 .adjusted(-spread, -spread, spread, spread);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(tokenColor("accent", static_cast<int>(210 * fade)), 0.6 + 2.4 * fade));
        const double corner = outer.height() / 2;
        painter.drawRoundedRect(outer, corner, corner);
    }
    drawEdgeFlash(painter, QRectF(rect()), t, 0.0, "accent");

    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenColor("accent", static_cast<int>(235 * std::min(1.0, grow * 3))));
    painter.drawRoundedRect(area, radius, radius);
    font.setPointSizeF(13 * std::max(scale, 0.1));
    painter.setFont(font);
    painter.setPen(QPen(tokenColor("on-accent", static_cast<int>(255 * std::min(1.0, grow * 2)))));
    painter.drawText(area, Qt::AlignCenter, text);
    drawMomentLabel(painter, QRectF(statusRect().adjusted(12, 0, -12, 0)), languageName(), t,
                    kLabelDelayS, "accent-soft");
    painter.setOpacity(1.0);
}

bool WaveformOverlay::chipVisible() const {
    return !language_.isEmpty() && languageChoices_.size() >= 2;
}

bool WaveformOverlay::platformTakesOverlayClicks() const {
    // Where a click on a non-activating window leaves the target app focused: Windows honours
    // WS_EX_NOACTIVATE, X11 the input hint, macOS a non-activating panel.
    if (takesClicksForTesting_) {
        return *takesClicksForTesting_;
    }
    const QString platform = QGuiApplication::platformName();
    return platform == QStringLiteral("windows") || platform == QStringLiteral("xcb") ||
           platform == QStringLiteral("cocoa");
}

bool WaveformOverlay::chipClickable() const {
    return platformTakesOverlayClicks() && isListening(currentState_) && chipVisible();
}

bool WaveformOverlay::chipHit(const QPointF& position) const {
    return chipClickable() && chipRect_.adjusted(-4, -4, 4, 4).contains(position);
}

QString WaveformOverlay::hit(const QPointF& position) const {
    if (chipHit(position)) {
        return QStringLiteral("language");
    }
    if (!controlsClickable()) {
        return QString();
    }
    for (const auto& [key, rect] : controlRects_) {
        if (rect.adjusted(-2, -4, 2, 4).contains(position)) {
            return key;
        }
    }
    return QString();
}

void WaveformOverlay::setHover(const QString& target) {
    if (target == hover_) {
        return;
    }
    hover_ = target;
    const QString shortcut = target == QStringLiteral("snap")     ? snapShortcut_
                             : target == QStringLiteral("finish") ? finishShortcut_
                                                                  : QString();
    const auto placed =
        std::find_if(controlRects_.begin(), controlRects_.end(),
                     [&target](const auto& entry) { return entry.first == target; });
    if (!shortcut.isEmpty() && placed != controlRects_.end()) {
        hotkeyHint_ = showHotkeyHint(hotkeyHint_, this, placed->second.toAlignedRect(), shortcut);
    } else {
        hideHotkeyHint(hotkeyHint_);
    }
    if (!target.isEmpty()) {
        setCursor(Qt::PointingHandCursor);
    } else if (drag_ != nullptr) {
        setCursor(Qt::OpenHandCursor);
    } else {
        unsetCursor();
    }
    update();
}

// --- Snap and Done while a walkthrough records ----------------------------------------------

void WaveformOverlay::setWalkthroughControls(bool on) {
    walkthroughControls_ = on;
    if (on && !cornered_) {
        cornered_ = true;
        repositionNearAnchor();
    }
    if (!on) {
        controlRects_.clear();
        if (hover_ == QStringLiteral("snap") || hover_ == QStringLiteral("finish")) {
            setHover(QString());
        }
    }
    applyHeight();
    update();
}

void WaveformOverlay::setWalkthroughShortcuts(const QString& snap, const QString& finish) {
    snapShortcut_ = snap;
    finishShortcut_ = finish;
}

void WaveformOverlay::setStandIn(std::function<bool()> showing) {
    standIn_ = std::move(showing);
    if (isVisible() && stoodIn(currentState_)) {
        finishHide();
    }
}

bool WaveformOverlay::stoodIn(const QString& state) const {
    return (isListening(state) || isFading(state)) && standIn_ && standIn_();
}

bool WaveformOverlay::controlsClickable() const {
    return walkthroughControls_ && platformTakesOverlayClicks() && isListening(currentState_);
}

std::vector<PlacedControl> WaveformOverlay::layoutControls() const {
    return layoutWalkthroughControls(width(), chipVisible(), chipVisible() ? chipWidth() : 0.0,
                                     badgeShown(), badgeShown() ? badgeWidth() : 0.0);
}

void WaveformOverlay::drawWalkthroughControls(QPainter& painter) {
    // The badge's own corner radius (_badge_radius of a standalone overlay).
    const double radius = kBadgeHeight / 2.0;
    for (const PlacedControl& placed : layoutControls()) {
        const QString key = walkthroughControlKey(placed.control);
        drawWalkthroughControl(painter, placed, hover_ == key, radius);
        controlRects_.emplace_back(key, placed.rect);
    }
}

void WaveformOverlay::setLanguage(const QString& code, const QStringList& choices) {
    if (code != language_ && !language_.isEmpty() && isVisible()) {
        languageChangedAt_ = now();
    }
    language_ = code;
    languageChoices_ = choices;
    applyHeight();
    update();
}

void WaveformOverlay::showLanguageNotice() {
    if (currentState_ == kStateLanguage && isVisible()) {
        animationTime_ = 0.0;
        // config.OVERLAY_HIDE_DELAY_MS, the language state's time on screen.
        hiddenTimer_->start(transientMs());
        update();
        return;
    }
    showAtCursor(kStateLanguage);
}

void WaveformOverlay::refreshLanguage() {
    QStringList choices;
    QString code;
    try {
        const std::shared_ptr<const Json> saved = savedSettings();
        for (const std::string& choice : speech::language_choices(*saved)) {
            choices.append(QString::fromStdString(choice));
        }
        if (choices.size() > 1) {
            code = QString::fromStdString(speech::current_language(*saved));
        }
    } catch (const std::exception& error) {
        kLog.debug("Dictation language unavailable for the overlay: {}", error.what());
        choices.clear();
        code.clear();
    }
    languageChangedAt_.reset();
    setLanguage(code, choices);
}

// --- the large-file split -------------------------------------------------------------------

void WaveformOverlay::setLargeFileInfo(double mb) {
    largeFileSizeMb_ = mb;
}

void WaveformOverlay::drawLargeFileSplittingState(QPainter& painter) {
    // A file bar that a blade sweeps along, snipping it into chunks. The sweep says work is
    // under way; it does not track progress, which the splitter does not report.
    const QRect area = rect();
    const int w = area.width();
    const int h = area.height();
    const double cy = baseHeight_ / 2.0 - 6;
    constexpr int kChunks = 5;
    constexpr double kChunkW = 22.0;
    constexpr double kGap = 4.0;
    const double track = kChunks * kChunkW + (kChunks - 1) * kGap;
    const double left = (w - track) / 2;
    const double cycle = std::fmod(animationTime_, 1.4) / 1.4;
    const double eased = cycle * cycle * (3.0 - 2.0 * cycle);
    const double blade = left - 12 + (track + 24) * eased;

    // Each cut springs open as the blade passes it, then eases shut.
    std::array<double, kChunks - 1> cuts{};
    std::array<double, kChunks - 1> opened{};
    for (int i = 0; i < kChunks - 1; ++i) {
        cuts[i] = left + (i + 1) * kChunkW + (i + 0.5) * kGap;
        opened[i] = blade >= cuts[i] ? std::max(0.0, 1.0 - (blade - cuts[i]) / 50.0) : 0.0;
    }
    const QColor amber = tokenColor("warning");
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < kChunks; ++i) {
        double before = 0.0;
        double after = 0.0;
        for (int k = 0; k < kChunks - 1; ++k) {
            (k < i ? before : after) += opened[k];
        }
        const double shift = 2.5 * (before - after);
        const double x = left + i * (kChunkW + kGap) + shift;
        const bool passed = blade >= x + kChunkW / 2;
        painter.setBrush(tokenColor("warning", passed ? 235 : 120));
        painter.drawRoundedRect(QRectF(x, cy - 4, kChunkW, 8), 4, 4);
    }
    for (int i = 0; i < kChunks - 1; ++i) {
        if (opened[i] > 0.05) {
            drawSparkle(painter, cuts[i], cy, 3.0 + 4.0 * opened[i],
                        tokenColor("warning", static_cast<int>(255 * opened[i])));
        }
    }
    if (left - 12 < blade && blade < left + track + 12) {
        const double reach =
            std::min({1.0, (blade - left + 12) / 14, (left + track + 12 - blade) / 14});
        painter.setPen(
            roundPen(tokenColor("warning-text-strong", static_cast<int>(230 * reach)), 2));
        painter.drawLine(QPointF(blade, cy - 11), QPointF(blade, cy + 11));
    }

    painter.setPen(QPen(amber));
    painter.setFont(segoe(10, QFont::Bold));
    const QString text =
        QString::fromStdString(std::format("Splitting ({:.1f} MB)...", largeFileSizeMb_));
    painter.drawText(area.adjusted(0, h - 25, 0, 0), Qt::AlignCenter, text);
}

void WaveformOverlay::drawNoticeState(QPainter& painter) {
    // A finished action's check, or a problem's mark beside its text.
    if (noticeDone_) {
        drawMoment(painter, notice_);
        return;
    }
    // A problem plays the moments' pop, ring and edge flash without the celebration: the "!"
    // gives a small head-shake instead of confetti.
    const double t = animationTime_;
    constexpr double radius = 11.0;
    constexpr double left = 16.0;
    const QPointF center(left + radius + shake_offset(t, 0.2, 0.36, 2.6, 3), height() / 2.0);
    painter.setOpacity(exit_opacity(t, transientMs() / 1000.0));
    drawRings(painter, center, t, 0.0, {"warning"}, 12.0, radius);
    drawEdgeFlash(painter, QRectF(rect()), t, 0.0, "warning");
    drawDisc(painter, center, t, 0.0, "warning", radius);
    drawGlyph(painter, center, t, 0.08, "bang");
    const int textLeft = static_cast<int>(left + 2 * radius + 10);
    const QRect area(textLeft, 8, width() - textLeft - 14, height() - 16);
    const QFont font = segoe(10, QFont::DemiBold);
    const double enter = phase(t, 0.06, kLabelS);
    painter.setFont(font);
    painter.setPen(QPen(tokenColor("overlay-text", static_cast<int>(255 * enter))));
    painter.drawText(area.translated(static_cast<int>(-8 * (1.0 - ease_out_cubic(enter))), 0),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, fitNotice(font, area));
    painter.setOpacity(1.0);
}

QString WaveformOverlay::fitNotice(const QFont& font, const QRect& rect) const {
    // The notice, cut with an ellipsis if it would wrap past rect.
    const QFontMetrics metrics(font);
    const int flags = Qt::AlignLeft | Qt::TextWordWrap;
    if (metrics.boundingRect(rect, flags, notice_).height() <= rect.height()) {
        return notice_;
    }
    std::vector<std::string> words = py::split(notice_.toStdString());
    while (words.size() > 1) {
        words.pop_back();
        std::string joined;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (i > 0) {
                joined += ' ';
            }
            joined += words[i];
        }
        const QString shorter =
            QString::fromStdString(py::rstrip(joined, ",;:—-")) + QStringLiteral("…");
        if (metrics.boundingRect(rect, flags, shorter).height() <= rect.height()) {
            return shorter;
        }
    }
    return metrics.elidedText(notice_, Qt::ElideRight, rect.width());
}

// --- animation and state --------------------------------------------------------------------

void WaveformOverlay::updateAnimation() {
    const double currentTime = now();
    const double deltaTime = std::max(0.0, std::min(0.1, currentTime - lastFrameTime_));
    lastFrameTime_ = currentTime;

    animationTime_ += deltaTime;

    style_->advance(styleState(currentState_).toStdString(), deltaTime);
    if (isListening(currentState_) && visual_ != nullptr) {
        ribbon_.advance(deltaTime);
        visual_->advance(deltaTime);
    } else if (isWork(currentState_)) {
        const double target =
            currentState_ == kStateProcessing ? kProcessingRate : kTranscribingRate;
        workRate_ += (target - workRate_) * (1.0 - std::exp(-deltaTime / kWorkEaseS));
        workClock_ += deltaTime * workRate_;
        if (visual_ != nullptr) {
            visual_->advanceWork(deltaTime, workRate_);
        }
    }
    if (fadingSince_ && now() - *fadingSince_ >= kFadeOutS) {
        finishHide();
        return;
    }

    if (currentState_ == kStateCanceling) {
        const Moment& canceled = canceled_moment();
        if (!burstFired_ && animationTime_ >= canceled.pop_at) {
            burstFired_ = true;
            const QPointF center = momentCenter();
            sttParticles_ = bubble_burst(canceled, center.x(), center.y(), rng_);
        }
        advance_bubbles(sttParticles_, deltaTime, rng_);
        cancelProgress_ = std::min(1.0, animationTime_ / cancelSeconds());
        if (cancelProgress_ >= 1.0) {
            setState(kStateIdle);
            timer_->stop();
        }
    } else if (const Moment* playing = moment()) {
        // The confetti leaves with the disc, after the lead-in.
        if (!burstFired_ && animationTime_ >= playing->pop_at) {
            burstFired_ = true;
            const QPointF center = momentCenter();
            sttParticles_ = burst(*playing, center.x(), center.y(), rng_);
        }
        advance_confetti(sttParticles_, deltaTime, playing->fall);
    }

    update();
}

void WaveformOverlay::setState(const QString& state) {
    if (stoodIn(state)) {
        // A notice on screen keeps its time; the recording itself hides.
        if (isListening(currentState_) || isFading(currentState_)) {
            finishHide();
        }
        return;
    }
    if (fadingSince_) {
        // Something new arrived while the last result faded: show it.
        fadingSince_.reset();
        update();
    }
    if (currentState_ == state) {
        return;
    }
    const bool wasListening = isListening(currentState_);
    if (isWork(state)) {
        beginWorkStage();
    } else if (isFading(state)) {
        // Cleaning up hands off from the work stage, if it follows one.
        labelFrom_ = workLabel();
    }
    currentState_ = state;
    const bool keepsDestination =
        (isListening(state) || isFading(state)) && state != kStateCommandListening;
    if (!keepsDestination) {
        recordingDestination_.clear();
    }
    restartAnimation(state);

    if (!isPreviewState(state)) {
        streamingText_.clear();
    }

    followListening(state, wasListening);
    applyHeight();

    Q_EMIT stateChanged(state);
    kLog.debug("Overlay state changed to: {}", state.toStdString());

    if (isTransient(state)) {
        hiddenTimer_->start(transientMs());
    } else {
        // A recording that starts during a short notice (a language switch just before
        // dictating) must not be hidden by it.
        hiddenTimer_->stop();
    }
}

void WaveformOverlay::beginWorkStage() {
    // Entering Processing or Transcribing, before currentState_ changes.
    if (isWork(currentState_)) {
        labelFrom_ = workLabel();
        labelChanged_ = now();
    } else {
        workStarted_ = now();
        workRate_ = 1.0;
        workClock_ = 0.0;
        labelFrom_.clear();
        if (visual_ != nullptr) {
            visual_->beginWork();
        }
    }
}

void WaveformOverlay::restartAnimation(const QString& state) {
    animationTime_ = 0.0;
    cancelProgress_ = 0.0;
    // Reset so the first frame does not take a huge step.
    lastFrameTime_ = now();

    if (state == kStateCanceling) {
        // What was on screen when the user canceled implodes.
        collapseFrom_.clear();
        for (const Particle& particle : style_->particles()) {
            if (particle.life > 0.05) {
                collapseFrom_.push_back(CollapsingDot{particle.x, particle.y,
                                                      particleColor(particle),
                                                      particle.size * particle.life});
            }
        }
    }

    sttParticles_.clear();
    burstFired_ = false;

    if (state == kStateIdle) {
        timer_->stop();
    } else {
        timer_->start(1000 / frameRate_);
    }
}

void WaveformOverlay::followListening(const QString& state, bool wasListening) {
    if (!isListening(state)) {
        endListeningExtras();
    } else if (!wasListening) {
        // A new recording: no latch yet, and the language as saved now.
        handsFree_ = false;
        refreshLanguage();
        ribbon_.reset();
        if (visual_ != nullptr) {
            visual_->reset();
        }
        listenStarted_ = now();
    }
}

void WaveformOverlay::updateAudioLevels(const QList<float>& levels) {
    audioLevels_.assign(levels.begin(), levels.begin() + std::min<qsizetype>(levels.size(), 20));
    if (visual_ != nullptr) {
        // Raw RMS barely moves for speech; feed loudness as it is heard.
        double loudest = 0.0;
        if (!audioLevels_.empty()) {
            loudest = *std::max_element(audioLevels_.begin(), audioLevels_.end());
        }
        ribbon_.hear(loudest);
        style_->update_audio_levels(std::vector<double>(audioLevels_.size(), ribbon_.loudness()));
    } else {
        style_->update_audio_levels(audioLevels_);
    }
}

void WaveformOverlay::setRecordingLook(const QString& look) {
    const std::string_view normalized = normalize_look(look.toStdString());
    recordingLook_ = fromUtf8(normalized);
    visual_ =
        makeVisual(normalized, LookContext{&ribbon_, style_.get(), overlayWidth_, baseHeight_});
    const bool live = visual_ != nullptr;
    style_->emitter = live ? visual_->emitter() : std::function<double(double)>();
    style_->config.audio_response = live ? kLiveAudioResponse : legacyAudioResponse_;
    const std::optional<double> rate = live ? visual_->emissionRate() : std::nullopt;
    style_->config.emission_rate = rate.value_or(defaultEmissionRate_);
    update();
}

void WaveformOverlay::setLabelParts(bool dot, bool text, bool clock) {
    labelParts_ = LabelParts{dot, text, clock};
    update();
}

void WaveformOverlay::refreshRecordingLook() {
    const std::shared_ptr<const Json> saved = savedSettings();
    const std::string look = keys::recording_overlay_look.read(*saved);
    setRecordingLook(QString::fromStdString(look));
    setLabelParts(keys::recording_overlay_dot.read(*saved),
                  keys::recording_overlay_text.read(*saved),
                  keys::recording_overlay_clock.read(*saved));
}

void WaveformOverlay::updateStreamingText(const QString& text, bool /*final*/) {
    streamingText_ = QString::fromStdString(py::strip(text.toStdString()));
    applyHeight();
    update();
}

void WaveformOverlay::clearStreamingText() {
    streamingText_.clear();
    applyHeight();
}

void WaveformOverlay::refreshStreamingFontSize() {
    const int size = keys::streaming_overlay_font_size.read(*savedSettings());
    if (size == streamingFontSize_) {
        return;
    }
    streamingFontSize_ = size;
    if (!streamingText_.isEmpty()) {
        applyHeight();
        update();
    }
}

void WaveformOverlay::refreshStreamingLayout() {
    const PreviewLayout layout = toLayout(keys::streaming_overlay_layout.read(*savedSettings()));
    if (layout == streamingLayout_) {
        return;
    }
    streamingLayout_ = layout;
    if (!streamingText_.isEmpty()) {
        applyHeight();
        update();
    }
}

void WaveformOverlay::setHandsFree(bool on) {
    if (on != handsFree_) {
        handsFree_ = on;
        applyHeight();
        update();
    }
}

void WaveformOverlay::setRecordingDestination(const QString& destination) {
    recordingDestination_ = destination;
    applyHeight();
    update();
}

void WaveformOverlay::showNotice(const QString& text, bool done) {
    if (text.isEmpty()) {
        return;
    }
    if (isVisible() && !fadingSince_ &&
        !(isTransient(currentState_) || currentState_ == kStateIdle)) {
        showCaption(text);
        return;
    }
    notice_ = text;
    noticeDone_ = done;
    if (currentState_ == kStateNotice && isVisible()) {
        animationTime_ = 0.0;
        sttParticles_.clear();
        burstFired_ = false;
        hiddenTimer_->start(noticeMs());
        update();
        return;
    }
    showAtCursor(kStateNotice);
}

int WaveformOverlay::noticeMs() const {
    // len() counts code points.
    return kCaptionMs + kNoticeMsPerChar * static_cast<int>(notice_.toUcs4().size());
}

void WaveformOverlay::showCaption(const QString& text) {
    // Ignored while hidden: the status line carries the same news.
    if (text.isEmpty() || !isVisible()) {
        return;
    }
    caption_ = text;
    captionStarted_ = now();
    captionTimer_->start(kCaptionMs);
    update();
}

void WaveformOverlay::clearCaption() {
    captionTimer_->stop();
    if (!caption_.isEmpty()) {
        caption_.clear();
        update();
    }
}

void WaveformOverlay::endListeningExtras() {
    handsFree_ = false;
    languageChangedAt_.reset();
    chipRect_ = QRectF();
    controlRects_.clear();
    setHover(QString());
    clearCaption();
}

void WaveformOverlay::hide() {
    // The fade keeps nothing waiting: the text is already delivered, and a new recording or
    // notice cancels it (see setState).
    if (isVisible() && isFading(currentState_)) {
        if (!fadingSince_) {
            fadingSince_ = now();
            hiddenTimer_->stop();
            if (!timer_->isActive()) {
                timer_->start(1000 / frameRate_);
            }
        }
        return;
    }
    finishHide();
}

void WaveformOverlay::finishHide() {
    // Python's _finish_hide emits no state_changed either: callers follow the overlay through
    // the states they set, not its hiding.
    fadingSince_.reset();
    timer_->stop();
    hiddenTimer_->stop();

    currentState_ = kStateIdle;
    animationTime_ = 0.0;
    cancelProgress_ = 0.0;
    streamingText_.clear();
    anchorPos_.reset();
    unparkedAt_.reset();
    cornered_ = false;
    if (drag_ != nullptr) {
        drag_->reset();
    }
    endListeningExtras();
    recordingDestination_.clear();
    notice_.clear();
    collapseFrom_.clear();
    language_.clear();
    languageChoices_.clear();
    if (overlayHeight_ != baseHeight_) {
        overlayHeight_ = baseHeight_;
        setFixedSize(overlayWidth_, overlayHeight_);
    }

    QWidget::hide();
}

void WaveformOverlay::showAtCursor(std::optional<QString> state) {
    const QString shown = state && !state->isEmpty()    ? *state
                          : currentState_ == kStateIdle ? kStateRecording
                                                        : currentState_;
    if (stoodIn(shown)) {
        return;
    }
    anchorPos_ = QCursor::pos();
    repositionNearAnchor();
    show();

    if (state) {
        setState(*state);
    } else if (currentState_ == kStateIdle) {
        setState(kStateRecording);
    }

    // The height may have changed with the state; clamp again.
    repositionNearAnchor();
}

void WaveformOverlay::closeEvent(QCloseEvent* event) {
    timer_->stop();
    hiddenTimer_->stop();
    captionTimer_->stop();
    event->accept();
}

void WaveformOverlay::hideEvent(QHideEvent* event) {
    setHover(QString());
    QWidget::hideEvent(event);
}

void WaveformOverlay::leaveEvent(QEvent* event) {
    setHover(QString());
    QWidget::leaveEvent(event);
}

// --- placement ------------------------------------------------------------------------------

std::optional<QRect> WaveformOverlay::availableGeometryForAnchor() const {
    const QPoint point = anchorPos_.value_or(QCursor::pos());
    QScreen* screen = QGuiApplication::screenAt(point);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return std::nullopt;
    }
    return screen->availableGeometry();
}

void WaveformOverlay::repositionNearAnchor() {
    // Moves the overlay near its anchor while keeping it fully on screen. The preview is placed
    // by its parent's layout.
    if (preview_) {
        return;
    }
    if (placed()) {
        // Where the user put it stays put; growing only nudges it back on screen.
        if (QScreen* current = screen()) {
            const Point at = clamp_rect_to_available(x(), y(), overlayWidth_, overlayHeight_,
                                                     toRect(current->availableGeometry()));
            move(at.x, at.y);
        }
        return;
    }
    if (cornered_) {
        if (const std::optional<QRect> available = availableGeometryForAnchor()) {
            move(available->left() + kParkMargin, available->top() + kParkMargin);
            return;
        }
    }
    if (!anchorPos_) {
        return;
    }
    const std::optional<QRect> available = availableGeometryForAnchor();
    if (!available) {
        move(anchorPos_->x() + 10, anchorPos_->y() + 10);
        return;
    }
    const Point at = preferred_overlay_position(Point{anchorPos_->x(), anchorPos_->y()},
                                                overlayWidth_, overlayHeight_, toRect(*available));
    move(at.x, at.y);
}

int WaveformOverlay::effectiveStreamingMaxHeight() const {
    // The configured cap, further limited by the free space near the anchor.
    MaxHeightInput input;
    input.base_height = baseHeight_;
    input.configured_max = kOverlayStreamingMaxHeight;
    if (placed() && screen() != nullptr) {
        input.placed = true;
        input.screen_available = toRect(screen()->availableGeometry());
        input.y = y();
    } else {
        const std::optional<QRect> available = availableGeometryForAnchor();
        if (available) {
            input.anchor_available = toRect(*available);
        }
        if (anchorPos_) {
            input.anchor = Point{anchorPos_->x(), anchorPos_->y()};
        }
    }
    return effective_max_height(input);
}

void WaveformOverlay::applyHeight() {
    // Grows or shrinks to fit the preview words, or the look under the badge row.
    HeightInput input;
    input.has_words = !streamingText_.isEmpty();
    input.base_height = baseHeight_;
    input.band_top = bandTop();
    if (input.has_words) {
        input.max_height = effectiveStreamingMaxHeight();
        const QRect bounded =
            QFontMetrics(streamingPreviewFont())
                .boundingRect(QRect(0, 0, text_flow_width(overlayWidth_), input.max_height),
                              Qt::AlignLeft | Qt::TextWordWrap, streamingText_);
        input.margins = previewTextMargins();
        input.text_height = bounded.height();
    }
    const int targetHeight = target_height(input);
    if (targetHeight != overlayHeight_) {
        overlayHeight_ = targetHeight;
        setFixedSize(overlayWidth_, overlayHeight_);
        repositionNearAnchor();
    }
}

bool WaveformOverlay::dragged() const {
    return drag_ != nullptr && drag_->moved();
}

bool WaveformOverlay::placed() const {
    // Where the overlay sits was chosen (dragged or parked), not taken from the pointer.
    return dragged() || unparkedAt_.has_value();
}

void WaveformOverlay::park(const QRect& area) {
    if (preview_ || !isVisible()) {
        return;
    }
    if (!unparkedAt_) {
        unparkedAt_ = pos();
    }
    move(area.left() + kParkMargin, area.top() + kParkMargin);
    raise();
}

void WaveformOverlay::unpark() {
    if (!unparkedAt_) {
        return;
    }
    const QPoint spot = *unparkedAt_;
    unparkedAt_.reset();
    move(spot);
    // It may have grown while parked.
    repositionNearAnchor();
}

void WaveformOverlay::mousePressEvent(QMouseEvent* event) {
    const QString target = event->button() == Qt::LeftButton ? hit(event->position()) : QString();
    // A press on the chip, Snap or Done never starts a drag.
    if (target == QStringLiteral("language")) {
        event->accept();
        Q_EMIT languageCycleRequested();
        return;
    }
    if (target == QStringLiteral("snap")) {
        event->accept();
        Q_EMIT walkthroughSnapRequested();
        return;
    }
    if (target == QStringLiteral("finish")) {
        event->accept();
        Q_EMIT walkthroughFinishRequested();
        return;
    }
    if (drag_ != nullptr && drag_->press(event)) {
        return;
    }
    QWidget::mousePressEvent(event);
}

void WaveformOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ != nullptr && drag_->move(event)) {
        return;
    }
    setHover(hit(event->position()));
    QWidget::mouseMoveEvent(event);
}

void WaveformOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (drag_ != nullptr && drag_->release(event)) {
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

} // namespace fv
