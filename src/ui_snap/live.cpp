#include "ui_snap/live.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>

#include "base/clock.h"
#include "base/identity.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_snap/affinity.h"
#include "ui_snap/capture.h"
#include "ui_snap/capture_delay.h"
#include "ui_snap/chrome.h"
#include "ui_snap/geometry.h"
#include "ui_snap/icons.h"
#include "ui_snap/overlay.h"
#include "ui_snap/pointed.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"

namespace fv::ui_snap {
namespace {

constexpr Qt::WindowFlags kFloating = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
constexpr Qt::WindowFlags kNoFocus = Qt::WindowDoesNotAcceptFocus;
constexpr Qt::WindowFlags kPassThrough = kNoFocus | Qt::WindowTransparentForInput;

// Keys that are part of a shortcut chord never act on the canvas.
constexpr Qt::KeyboardModifiers kChord =
    Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;

// The Snap overlay's tool keys (overlay.py _TOOL_KEYS).
constexpr std::array<std::pair<int, SnapTool>, 8> kToolKeys{{
    {Qt::Key_A, SnapTool::Arrow},
    {Qt::Key_B, SnapTool::Box},
    {Qt::Key_O, SnapTool::Ellipse},
    {Qt::Key_P, SnapTool::Pen},
    {Qt::Key_H, SnapTool::Highlighter},
    {Qt::Key_T, SnapTool::Text},
    {Qt::Key_N, SnapTool::Step},
    {Qt::Key_R, SnapTool::Redact},
}};

std::optional<SnapTool> toolForKey(int key) {
    for (const auto& [toolKey, tool] : kToolKeys) {
        if (toolKey == key) {
            return tool;
        }
    }
    return std::nullopt;
}

// The overlay's colour keys 1 to 4 (overlay.py _COLOR_KEYS), as swatch indexes.
std::optional<int> colorForKey(int key) {
    if (key >= Qt::Key_1 && key < Qt::Key_1 + static_cast<int>(kSwatches.size())) {
        return key - Qt::Key_1;
    }
    return std::nullopt;
}

QColor swatchColor(int index) {
    return QColor(QString::fromLatin1(kSwatches[index].color));
}

bool readBool(const SettingsStore* settings, const BoolSetting& setting, bool current) {
    return settings != nullptr ? settings->get(setting) : current;
}

void saveBool(SettingsStore* settings, const BoolSetting& setting, bool value) {
    if (settings != nullptr) {
        settings->save_setting(setting.key(), value);
    }
}

} // namespace

// ---- LiveCanvas ----

LiveCanvas::LiveCanvas(LiveAnnotations* live, QScreen* screen)
    : QWidget(nullptr, kFloating | kPassThrough), live_(live), screen_(screen) {
    expire_.setSingleShot(true);
    expire_.setTimerType(Qt::PreciseTimer);
    connect(&expire_, &QTimer::timeout, this, &LiveCanvas::expireMarks);
    setWindowTitle(QString::fromUtf8(identity().product) + QStringLiteral(" Walkthrough Canvas"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
    // The marker the Scratchpad sets: app hotkeys behave as in our other windows while a canvas
    // has the focus.
    ui::setStyleProperty(this, ui::prop::fiorivoceHotkeyWindow, true);
    setFocusPolicy(Qt::StrongFocus);
    connect(screen, &QScreen::geometryChanged, this, &LiveCanvas::geometryChanged);
    setGeometry(screen->geometry());
}

void LiveCanvas::geometryChanged(const QRect& rect) {
    setGeometry(rect);
    if (region()) {
        live_->clearRegion();
    }
}

void LiveCanvas::present() {
    const bool armed = live_->armed();
    const Qt::WindowFlags flags = armed ? kFloating : kFloating | kPassThrough;
    if ((windowFlags() & kPassThrough) != (flags & kPassThrough)) {
        setWindowFlags(flags);
    }
    if (!armed && marks_.empty() && !region()) {
        hide();
        return;
    }
    if (screen_) {
        setScreen(screen_);
        setGeometry(screen_->geometry());
    }
    setCursor(live_->tool() == SnapTool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
    show();
    if (!isKeptOut(this)) {
        keepOutOfScreenshots(this, true);
    }
    update();
}

std::vector<Annotation> LiveCanvas::items(bool pinnedOnly) const {
    std::vector<Annotation> found;
    for (const Mark& mark : marks_) {
        if (!mark.expires || !pinnedOnly) {
            found.push_back(mark.mark);
        }
    }
    return found;
}

void LiveCanvas::add(Annotation mark, bool pinned) {
    if (mark.kind == AnnotationKind::Step) {
        ++nextStep_;
    }
    std::optional<std::chrono::steady_clock::time_point> expires;
    if (!pinned) {
        expires = live_->clock().steady() + kMarkLifetime;
    }
    marks_.push_back({std::move(mark), expires});
    expireMarks();
}

void LiveCanvas::expireMarks() {
    if (live_->capturePending()) {
        expire_.stop();
        update();
        return;
    }
    const auto now = live_->clock().steady();
    std::erase_if(marks_, [now](const Mark& mark) { return mark.expires && *mark.expires <= now; });
    std::optional<std::chrono::steady_clock::time_point> next;
    for (const Mark& mark : marks_) {
        if (mark.expires && (!next || *mark.expires < *next)) {
            next = mark.expires;
        }
    }
    if (next) {
        expire_.start(std::chrono::ceil<std::chrono::milliseconds>(*next - now));
    }
    if (marks_.empty() && !live_->armed() && !region()) {
        hide();
    }
    update();
}

void LiveCanvas::clear() {
    marks_.clear();
    nextStep_ = 1;
    expire_.stop();
    cancelDraft();
    expireMarks();
}

void LiveCanvas::keepPinnedOnly() {
    std::erase_if(marks_, [](const Mark& mark) { return mark.expires.has_value(); });
}

void LiveCanvas::renewTemporary() {
    const auto expires = live_->clock().steady() + kMarkLifetime;
    for (Mark& mark : marks_) {
        if (mark.expires) {
            mark.expires = expires;
        }
    }
}

void LiveCanvas::cancelDraft() {
    draft_.reset();
    regionStart_.reset();
    regionDraft_.reset();
    if (LabelEditor* editor = labelEditor_.data()) {
        labelEditor_ = nullptr;
        editor->hide();
        editor->deleteLater();
    }
    Q_EMIT live_->interactionChanged(false);
    update();
}

std::optional<QRect> LiveCanvas::region() const {
    const std::optional<LiveRegion> region = live_->captureRegion();
    if (region && region->screen == screen_) {
        return region->rect;
    }
    return std::nullopt;
}

QPointF LiveCanvas::point(QPointF point) const {
    const std::optional<SnapTool> tool = live_->tool();
    if (live_->snapEnabled() &&
        (tool == SnapTool::Arrow || tool == SnapTool::Box || tool == SnapTool::Ellipse)) {
        point = snapPoint(point, std::span<const QRect>(targets_.constData(),
                                                        static_cast<std::size_t>(targets_.size())));
    }
    return QPointF(std::min(std::max(point.x(), 0.0), static_cast<double>(width())),
                   std::min(std::max(point.y(), 0.0), static_cast<double>(height())));
}

void LiveCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !live_->armed() || live_->busy()) {
        return;
    }
    if (labelEditor_) {
        commitLabel();
        return;
    }
    const std::optional<SnapTool> tool = live_->tool();
    const QColor color = swatchColor(live_->color());
    const QPointF at = point(event->position());
    if (live_->selectingRegion()) {
        regionStart_ = at;
        regionDraft_ = QRect(at.toPoint(), at.toPoint());
    } else if (tool == SnapTool::Text) {
        openLabel(at, color, (event->modifiers() & Qt::ControlModifier) != 0);
    } else if (tool == SnapTool::Step) {
        draft_ = Annotation::step(at, nextStep_, color);
    } else if (tool) {
        draft_ = newDraft(*tool, at, color, live_->boxInfill());
    }
    if (live_->interacting()) {
        Q_EMIT live_->interactionChanged(true);
    }
    update();
}

void LiveCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    mousePressEvent(event);
}

void LiveCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (regionStart_) {
        regionDraft_ = span(regionStart_->toPoint(), point(event->position()).toPoint());
        update();
    }
    if (draft_) {
        const QRectF before = annotationBounds(*draft_);
        extendDraft(*draft_, point(event->position()),
                    (event->modifiers() & Qt::ShiftModifier) != 0, QRectF(rect()));
        update(before.united(annotationBounds(*draft_)).toAlignedRect().adjusted(-2, -2, 2, 2));
    }
}

void LiveCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && regionStart_) {
        const QRect rect = span(regionStart_->toPoint(), point(event->position()).toPoint());
        regionStart_.reset();
        regionDraft_.reset();
        live_->setRegion(screen_, rect);
        update();
        return;
    }
    if (event->button() != Qt::LeftButton || !draft_) {
        return;
    }
    Annotation draft = std::move(*draft_);
    draft_.reset();
    extendDraft(draft, point(event->position()), (event->modifiers() & Qt::ShiftModifier) != 0,
                QRectF(rect()));
    Q_EMIT live_->interactionChanged(false);
    if (meaningful(draft)) {
        live_->complete(this, draft, (event->modifiers() & Qt::ControlModifier) != 0);
    } else {
        update();
    }
}

void LiveCanvas::keyPressEvent(QKeyEvent* event) {
    if ((event->modifiers() & kChord) != 0) {
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        live_->cancelCapture();
    } else if (key == Qt::Key_C) {
        live_->clear();
    } else if (key == Qt::Key_K && !event->isAutoRepeat()) {
        live_->toggleKeepMarks();
    } else if (key == Qt::Key_S) {
        live_->toggleSnap();
    } else if (const std::optional<SnapTool> tool = toolForKey(key); tool && !draft_) {
        live_->arm(*tool);
    } else if (const std::optional<int> color = colorForKey(key)) {
        live_->setColor(*color);
    }
}

void LiveCanvas::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && !live_->busy()) {
        // A click on another monitor can move focus between our canvases.
        QTimer::singleShot(0, live_, &LiveAnnotations::checkFocus);
    }
}

void LiveCanvas::openLabel(QPointF point, const QColor& color, bool pin) {
    pinLabel_ = pin;
    auto* editor = new LabelEditor(this, color);
    labelEditor_ = editor;
    editor->move(labelOrigin(rect(), point, editor->size()));
    editor->refit();
    connect(editor, &LabelEditor::committed, this, &LiveCanvas::commitLabel);
    // Esc in a label is Esc on the canvas: it also cancels a pending screenshot.
    connect(editor, &LabelEditor::cancelled, live_, &LiveAnnotations::cancelCapture);
    editor->show();
    editor->setFocus();
}

void LiveCanvas::commitLabel() {
    if (!labelEditor_) {
        return;
    }
    const Annotation label = labelEditor_->annotation();
    const bool pinned =
        pinLabel_ || (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) != 0;
    cancelDraft();
    if (meaningful(label)) {
        live_->complete(this, label, pinned);
    }
}

void LiveCanvas::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    if (live_->armed()) {
        // Windows lets mouse events through fully transparent layered pixels.
        painter.fillRect(event->rect(), QColor(0, 0, 0, 1));
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (const Mark& mark : marks_) {
        paintAnnotation(painter, mark.mark);
    }
    if (draft_) {
        paintAnnotation(painter, *draft_);
    }
    const std::optional<QRect> shown = regionDraft_ ? regionDraft_ : region();
    if (shown) {
        paintFrame(painter, *shown);
    }
    if (live_->selectingRegion()) {
        const QString text = QString::fromUtf16(kRegionHint);
        const QFont font = chromeFont(this->font(), 12);
        const QSize size = badgeSize(text, font);
        const int toolbarHeight = live_->toolbar() != nullptr ? live_->toolbar()->height() : 0;
        const QRect hint((width() - size.width()) / 2, toolbarHeight + 2 * kEdgeMargin,
                         size.width(), size.height());
        paintBadge(painter, hint, text, font, ui::currentPalette());
    }
}

// ---- LiveRegionClose ----

LiveRegionClose::LiveRegionClose(LiveAnnotations* live)
    : QWidget(nullptr, kFloating | kNoFocus), live_(live) {
    setFixedSize(24, 24);
    setWindowTitle(QStringLiteral("Close automatic capture region"));
    setAccessibleName(QStringLiteral("Close region and capture the whole screen"));
    setToolTip(QStringLiteral(u"Close region · capture the whole screen"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_AlwaysShowToolTips);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
    setCursor(Qt::PointingHandCursor);
}

void LiveRegionClose::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton;
}

void LiveRegionClose::mouseReleaseEvent(QMouseEvent* event) {
    const bool pressed = std::exchange(pressed_, false);
    if (pressed && event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint())) {
        live_->clearRegion();
    }
}

void LiveRegionClose::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& palette = ui::currentPalette();
    paintPanel(painter, QRectF(rect()), 6, palette, 0.0);
    paintSnapIcon(painter, "drop", QRectF(2, 2, 20, 20), palette.color("overlay-text"));
}

// ---- LiveAnnotations ----

LiveAnnotations::LiveAnnotations(QObject* parent, QWidget* panel, MarkShortcuts markShortcuts)
    : LiveAnnotations(parent, panel, std::move(markShortcuts), Options{}) {}

LiveAnnotations::LiveAnnotations(QObject* parent, QWidget* panel, MarkShortcuts markShortcuts,
                                 Options options)
    : QObject(parent), settings_(options.settings),
      clock_(options.clock != nullptr ? options.clock : &system_clock()), panel_(panel) {
    boxInfill_ =
        readBool(settings_, keys::snap_box_infill, keys::snap_box_infill.read(Json::object()));
    keepMarks_ = readBool(settings_, keys::walkthrough_keep_marks,
                          keys::walkthrough_keep_marks.read(Json::object()));
    screen_ = screenUnderCursor();
    auto* toolbar = new SnapToolbar(nullptr, ui::currentPalette(), SnapToolbar::Variant::Live,
                                    kFloating | kNoFocus, std::move(markShortcuts));
    toolbar_ = toolbar;
    captureDelay_ = new CaptureDelay(toolbar, settings_);
    toolbar->setCaptureDelay(captureDelay_);
    toolbar->setWindowTitle(QString::fromUtf8(identity().product) +
                            QStringLiteral(" Walkthrough Drawing"));
    toolbar->setAttribute(Qt::WA_ShowWithoutActivating);
    toolbar->setAttribute(Qt::WA_AlwaysShowToolTips);
    toolbar->setAttribute(Qt::WA_QuitOnClose, false);
    toolbar->setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
    connect(toolbar, &SnapToolbar::toolChosen, this, &LiveAnnotations::toggleTool);
    connect(toolbar, &SnapToolbar::colorChosen, this, &LiveAnnotations::setColor);
    connect(toolbar, &SnapToolbar::snapClicked, this, &LiveAnnotations::toggleSnap);
    connect(toolbar, &SnapToolbar::infillClicked, this, &LiveAnnotations::toggleBoxInfill);
    toolbar->setBoxInfill(boxInfill_);
    connect(toolbar, &SnapToolbar::keepMarksClicked, this, &LiveAnnotations::toggleKeepMarks);
    connect(toolbar, &SnapToolbar::dropClicked, this, &LiveAnnotations::cancelCapture);
    connect(captureDelay_, &CaptureDelay::changed, this, &LiveAnnotations::captureDelayChanged);
    // The chip's × cancels only the screenshot; the tool stays in hand.
    connect(captureDelay_, &CaptureDelay::cancelled, this, &LiveAnnotations::captureCancelled);
    connect(toolbar, &SnapToolbar::clearClicked, this, &LiveAnnotations::clear);
    connect(toolbar, &SnapToolbar::regionClicked, this, &LiveAnnotations::selectRegion);
    toolbar->setKeepMarks(keepMarks_);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &LiveAnnotations::removeScreen);
    syncToolbar();
    setEnabled(true);
}

LiveAnnotations::~LiveAnnotations() {
    // The windows are top-level: nothing else deletes them.
    delete regionClose_.data();
    for (const Surface& surface : canvases_) {
        delete surface.canvas.data();
    }
    for (const QPointer<LiveCanvas>& canvas : retired_) {
        delete canvas.data();
    }
    delete toolbar_.data();
}

bool LiveAnnotations::interacting() const {
    return std::any_of(canvases_.begin(), canvases_.end(), [](const Surface& surface) {
        return surface.canvas && (surface.canvas->drafting() || surface.canvas->labelEditor());
    });
}

QList<QWidget*> LiveAnnotations::windows() const {
    QList<QWidget*> found;
    if (toolbar_) {
        found.append(toolbar_.data());
    }
    for (LiveCanvas* canvas : canvases()) {
        found.append(canvas);
    }
    if (regionClose_) {
        found.append(regionClose_.data());
    }
    if (captureDelay_) {
        found.append(captureDelay_->popupWindows());
    }
    return found;
}

LiveCanvas* LiveAnnotations::canvas(QScreen* screen) const {
    for (const Surface& surface : canvases_) {
        if (surface.screen == screen && surface.canvas) {
            return surface.canvas;
        }
    }
    return nullptr;
}

QList<LiveCanvas*> LiveAnnotations::canvases() const {
    QList<LiveCanvas*> found;
    for (const Surface& surface : canvases_) {
        if (surface.canvas) {
            found.append(surface.canvas);
        }
    }
    return found;
}

void LiveAnnotations::setCaptureCountdown(double remaining, double total) {
    if (captureDelay_) {
        captureDelay_->setCountdown(remaining, total);
    }
    countingDown_ = remaining > 0;
    syncDrop();
}

void LiveAnnotations::syncDrop() {
    // × puts a tool down or cancels a screenshot that is coming; with neither it has nothing to
    // do, and says so instead of ignoring clicks.
    if (toolbar_) {
        toolbar_->setDropEnabled(armed() || capturePending_ || countingDown_);
    }
}

void LiveAnnotations::setCapturePending(bool pending, bool captured,
                                        std::optional<QScreen*> screen) {
    if (capturePending_ == pending) {
        return;
    }
    capturePending_ = pending;
    syncDrop();
    for (const Surface& surface : canvases_) {
        LiveCanvas* canvas = surface.canvas;
        if (canvas == nullptr) {
            continue;
        }
        if (pending) {
            canvas->expireTimer().stop();
        } else {
            if (captured && (!screen || *screen == surface.screen)) {
                // Delayed marks leave the desktop with their screenshot; Keep marks and
                // individually pinned marks stay put.
                canvas->keepPinnedOnly();
            } else {
                // A cancelled or failed snap gives temporary marks their usual lifetime again
                // instead of clearing them early.
                canvas->renewTemporary();
            }
            canvas->expireMarks();
        }
    }
}

void LiveAnnotations::cancelCapture() {
    Q_EMIT captureCancelled();
    disarm();
}

void LiveAnnotations::setEnabled(bool enabled) {
    if (enabled == enabled_) {
        return;
    }
    enabled_ = enabled;
    if (enabled) {
        snapEnabled_ = settings_ != nullptr ? settings_->get(keys::snap_to_edges)
                                            : keys::snap_to_edges.read(Json::object());
        if (toolbar_) {
            toolbar_->setSnapEnabled(snapEnabled_);
        }
        syncToolbar();
        for (LiveCanvas* canvas : canvases()) {
            canvas->present();
        }
        showToolbar();
        showRegionClose();
    } else {
        disarm();
        if (toolbar_) {
            toolbar_->hide();
        }
        for (LiveCanvas* canvas : canvases()) {
            canvas->hide();
        }
        if (regionClose_) {
            regionClose_->hide();
        }
    }
    Q_EMIT enabledChanged(enabled);
}

void LiveAnnotations::showToolbar() {
    if (!screen_) {
        screen_ = screenUnderCursor();
    }
    if (!screen_ || !toolbar_) {
        return;
    }
    const QRect area = screen_->geometry();
    toolbar_->fitToWidth(area.width() - 2 * kEdgeMargin);
    toolbar_->setScreen(screen_);
    toolbar_->move(area.x() + std::max(0, (area.width() - toolbar_->width()) / 2),
                   area.y() + kEdgeMargin);
    toolbar_->show();
    if (!isKeptOut(toolbar_)) {
        keepOutOfScreenshots(toolbar_, true);
    }
    toolbar_->raise();
}

void LiveAnnotations::toggleTool(SnapTool tool) {
    if (tool_ == tool) {
        disarm();
    } else {
        arm(tool);
    }
}

void LiveAnnotations::selectRegion() {
    if (selectingRegion_) {
        disarm();
    } else {
        armFor(std::nullopt);
    }
}

void LiveAnnotations::arm(SnapTool tool) {
    armFor(tool);
}

void LiveAnnotations::armFor(std::optional<SnapTool> tool) {
    if (!enabled_ || busy_) {
        return;
    }
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return;
    }
    if (!armed()) {
        Q_EMIT arming();
        previousFocus_ = foregroundWindow();
        for (QScreen* screen : screens) {
            if (canvas(screen) == nullptr) {
                std::erase_if(canvases_, [screen](const Surface& surface) {
                    return surface.screen == screen;
                });
                canvases_.push_back({screen, new LiveCanvas(this, screen)});
            }
        }
        // Read the edges before any of the drawing surfaces take focus.
        QList<QPointer<QWidget>> exclude;
        for (QWidget* window : windows()) {
            exclude.append(window);
        }
        if (panel_) {
            exclude.append(panel_.data());
        }
        for (const Surface& surface : canvases_) {
            if (surface.canvas && surface.screen) {
                QList<QRect> targets{surface.canvas->rect()};
                targets.append(windowTargets(surface.screen, exclude));
                surface.canvas->setTargets(std::move(targets));
            }
        }
    } else if (!tool || tool_ != tool) {
        // A different tool (the region always counts as one) drops what was being made.
        for (LiveCanvas* canvas : canvases()) {
            canvas->cancelDraft();
        }
    }
    selectingRegion_ = !tool.has_value();
    tool_ = tool;
    for (LiveCanvas* canvas : canvases()) {
        canvas->present();
    }
    if (LiveCanvas* pointed = canvas(screenUnderCursor())) {
        pointed->activateWindow();
        pointed->setFocus(Qt::ActiveWindowFocusReason);
        bringToFront(pointed);
    }
    raiseControls();
    syncToolbar();
}

void LiveAnnotations::disarm() {
    if (!armed()) {
        return;
    }
    QList<QPointer<QWidget>> surfaces;
    for (LiveCanvas* canvas : canvases()) {
        surfaces.append(canvas);
    }
    returnForeground(previousFocus_, surfaces);
    previousFocus_ = 0;
    tool_.reset();
    selectingRegion_ = false;
    for (LiveCanvas* canvas : canvases()) {
        canvas->cancelDraft();
        if (enabled_) {
            canvas->present();
        }
    }
    syncToolbar();
    raiseControls();
}

void LiveAnnotations::raiseControls() {
    if (panel_ && panel_->isVisible()) {
        panel_->raise();
    }
    if (toolbar_) {
        toolbar_->raise();
    }
    if (regionClose_ && regionClose_->isVisible()) {
        regionClose_->raise();
    }
}

void LiveAnnotations::setColor(int color) {
    if (color < 0 || color >= static_cast<int>(kSwatches.size())) {
        return;
    }
    color_ = color;
    for (LiveCanvas* canvas : canvases()) {
        if (LabelEditor* editor = canvas->labelEditor()) {
            editor->setColor(swatchColor(color));
        }
    }
    syncToolbar();
}

void LiveAnnotations::toggleSnap() {
    snapEnabled_ = !snapEnabled_;
    if (toolbar_) {
        toolbar_->setSnapEnabled(snapEnabled_);
    }
    saveBool(settings_, keys::snap_to_edges, snapEnabled_);
}

void LiveAnnotations::toggleBoxInfill() {
    boxInfill_ = !boxInfill_;
    saveBool(settings_, keys::snap_box_infill, boxInfill_);
    if (toolbar_) {
        toolbar_->setBoxInfill(boxInfill_);
    }
}

void LiveAnnotations::toggleKeepMarks() {
    const bool keepMarks = !keepMarks_;
    saveBool(settings_, keys::walkthrough_keep_marks, keepMarks);
    keepMarks_ = keepMarks;
    if (toolbar_) {
        toolbar_->setKeepMarks(keepMarks);
    }
}

void LiveAnnotations::handleMarkShortcut(std::string_view action) {
    if (!enabled_) {
        return;
    }
    if (action == "keep_marks") {
        toggleKeepMarks();
    } else if (action == "clear") {
        clear();
    }
}

void LiveAnnotations::setBusy(bool busy) {
    if (busy == busy_) {
        return;
    }
    busy_ = busy;
    if (toolbar_) {
        toolbar_->setEnabled(!busy);
    }
    if (!busy && tool_ && !canvasHasFocus()) {
        // A screenshot can temporarily hide the canvas that had focus.
        arm(*tool_);
    }
}

void LiveAnnotations::syncToolbar() {
    boxInfill_ = readBool(settings_, keys::snap_box_infill, boxInfill_);
    if (!toolbar_) {
        return;
    }
    toolbar_->setBoxInfill(boxInfill_);
    toolbar_->setState(tool_, color_, false, false);
    toolbar_->setRegionActive(selectingRegion_ || captureRegion_.has_value());
    syncDrop();
}

void LiveAnnotations::setRegion(QScreen* screen, QRect rect) {
    if (screen == nullptr) {
        return;
    }
    const QRect bounds(0, 0, screen->geometry().width(), screen->geometry().height());
    rect = rect.intersected(bounds);
    if (rect.width() < 4 || rect.height() < 4) {
        return;
    }
    Q_EMIT captureChanged();
    captureRegion_ = LiveRegion{screen, rect};
    disarm();
    for (LiveCanvas* canvas : canvases()) {
        if (enabled_) {
            canvas->present();
        }
    }
    if (!regionClose_) {
        regionClose_ = new LiveRegionClose(this);
    }
    showRegionClose();
    syncToolbar();
}

void LiveAnnotations::clearRegion() {
    // Discard pending captures and queued hook releases before hiding ×.
    Q_EMIT captureChanged();
    captureRegion_.reset();
    if (regionClose_) {
        regionClose_->hide();
    }
    for (LiveCanvas* canvas : canvases()) {
        if (enabled_) {
            canvas->present();
        }
    }
    syncToolbar();
}

void LiveAnnotations::showRegionClose() {
    if (!enabled_ || !captureRegion_ || !regionClose_ || !captureRegion_->screen) {
        return;
    }
    QScreen* screen = captureRegion_->screen;
    const QRect rect = captureRegion_->rect;
    LiveRegionClose* close = regionClose_;
    const QRect area = screen->geometry();
    close->setScreen(screen);
    // Keep the control on screen, including a selection against an edge.
    const int x = area.x() + rect.right() - close->width() + 1;
    close->move(std::max(area.left(), std::min(area.right() - close->width() + 1, x)),
                std::max(area.y(), area.y() + rect.y() - close->height() - 3));
    close->show();
    keepOutOfScreenshots(close, true);
    close->raise();
}

bool LiveAnnotations::canvasHasFocus() const {
    auto* active = qobject_cast<LiveCanvas*>(QApplication::activeWindow());
    return active != nullptr && canvases().contains(active);
}

void LiveAnnotations::checkFocus() {
    if (armed() && !busy_ && !canvasHasFocus()) {
        disarm();
    }
}

void LiveAnnotations::complete(LiveCanvas* canvas, const Annotation& mark, bool pinned) {
    canvas->add(mark, pinned || keepMarks_);
    const std::vector<Annotation> items = canvas->items();
    Q_EMIT drawn(canvas->canvasScreen(), QList<Annotation>(items.begin(), items.end()));
}

std::vector<Annotation> LiveAnnotations::pinnedAt(QPoint position) const {
    return pinnedOn(screenAtClick(position));
}

std::vector<Annotation> LiveAnnotations::pinnedOn(QScreen* screen) const {
    LiveCanvas* found = screen != nullptr ? canvas(screen) : nullptr;
    return found != nullptr ? found->items(true) : std::vector<Annotation>{};
}

void LiveAnnotations::clear() {
    Q_EMIT captureCancelled();
    for (LiveCanvas* canvas : canvases()) {
        canvas->clear();
    }
}

void LiveAnnotations::removeScreen(QScreen* screen) {
    disarm();
    if (captureRegion_ && captureRegion_->screen == screen) {
        clearRegion();
    }
    for (auto it = canvases_.begin(); it != canvases_.end(); ++it) {
        if (it->screen == screen) {
            if (LiveCanvas* canvas = it->canvas) {
                canvas->hide();
                canvas->deleteLater();
                retired_.removeAll(nullptr);
                retired_.append(canvas);
            }
            canvases_.erase(it);
            break;
        }
    }
    if (screen_ == screen) {
        screen_ = nullptr;
        if (enabled_) {
            showToolbar();
        }
    }
}

void LiveAnnotations::close() {
    setEnabled(false);
    for (QWidget* window : windows()) {
        window->deleteLater();
    }
    canvases_.clear();
    deleteLater();
}

} // namespace fv::ui_snap
