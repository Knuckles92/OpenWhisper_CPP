#include "ui_snap/overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <span>
#include <utility>

#include <QCloseEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>

#include "base/identity.h"
#include "pyport/number.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_snap/affinity.h"
#include "ui_snap/desktop.h"
#include "ui_snap/export.h"
#include "ui_snap/toolbar.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"

namespace fv::ui_snap {
namespace {

constexpr int kHintEdge = 14;

constexpr Qt::KeyboardModifiers kChord =
    Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;

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

bool isKeepKey(int key) {
    return key == Qt::Key_Return || key == Qt::Key_Enter;
}

Qt::CursorShape resizeCursor(Grab grab) {
    if (grab.x != 0 && grab.y != 0) {
        return grab.x == grab.y ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    }
    return grab.x != 0 ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}

// The frozen screen under the smoked layer, made once so drags only copy pixels.
QImage dimmedCopy(const QImage& frozen) {
    QImage dimmed = frozen.copy();
    QPainter painter(&dimmed);
    painter.fillRect(QRectF(QPointF(0, 0), dimmed.deviceIndependentSize()), kDim);
    return dimmed;
}

int pyRound(double value) {
    return static_cast<int>(py::round_half_even(value));
}

bool readBoxInfill(const SettingsStore* settings) {
    return settings != nullptr ? settings->get(keys::snap_box_infill)
                               : keys::snap_box_infill.read(Json::object());
}

constexpr char16_t kLabelSheet[] =
    u"QLineEdit#snapLabelEditor { background: %1; color: %2; border: none; border-radius: 6px; "
    u"padding: 3px 6px; font-size: 16px; font-weight: 600; selection-background-color: "
    u"rgba(%3, 0.35); selection-color: %2; }";

} // namespace

Qt::WindowFlags snapWindowFlags(const std::string& system, bool wayland) {
    const Qt::WindowFlags frameless = Qt::FramelessWindowHint;
    if (wayland) {
        return Qt::Window | frameless;
    }
    const bool tool = system == "win32" || system == "darwin";
    const Qt::WindowFlags kind = tool ? Qt::Tool : Qt::Window;
    return kind | frameless | Qt::WindowStaysOnTopHint;
}

Qt::WindowFlags snapWindowFlags() {
    return snapWindowFlags(systemPlatform(),
                           qtPlatformName().toLower().startsWith(QStringLiteral("wayland")));
}

QPoint labelOrigin(const QRect& area, QPointF point, QSize size) {
    const int left = area.left();
    const int top = area.top();
    const int roomX = left + std::max(0, area.width() - size.width());
    const int roomY = top + std::max(0, area.height() - size.height());
    return QPointF(
               std::min(std::max(point.x(), static_cast<double>(left)), static_cast<double>(roomX)),
               std::min(std::max(point.y() - size.height() / 2.0, static_cast<double>(top)),
                        static_cast<double>(roomY)))
        .toPoint();
}

// ---- LabelEditor ----

LabelEditor::LabelEditor(QWidget* parent, const QColor& color) : QLineEdit(parent), color_(color) {
    setObjectName(QStringLiteral("snapLabelEditor"));
    setPlaceholderText(QStringLiteral("Label"));
    setFrame(false);
    setMaxLength(kMaxLength);
    connect(this, &QLineEdit::textChanged, this, [this] { fit(); });
    setColor(color);
}

void LabelEditor::setColor(const QColor& color) {
    color_ = color;
    const QColor ink = inkFor(color);
    const QString wash =
        ink.lightness() > 128 ? QStringLiteral("255, 255, 255") : QStringLiteral("0, 0, 0");
    ui::setLocalStyleSheet(this,
                           QString::fromUtf16(kLabelSheet).arg(color.name(), ink.name(), wash));
    QPalette colors = palette();
    QColor hint(ink);
    hint.setAlphaF(0.6F);
    colors.setColor(QPalette::PlaceholderText, hint);
    setPalette(colors);
    fit();
}

void LabelEditor::refit() {
    fit();
}

Annotation LabelEditor::annotation() const {
    return Annotation::label(QPointF(pos()), text().trimmed(), color_, font());
}

void LabelEditor::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (isKeepKey(key)) {
        Q_EMIT committed();
    } else if (key == Qt::Key_Escape) {
        Q_EMIT cancelled();
    } else if (key != Qt::Key_Tab && key != Qt::Key_Backtab) {
        QLineEdit::keyPressEvent(event);
        return;
    }
    // Swallowed, Tab included: an ignored key goes on to the overlay, where Esc drops the snap
    // and Tab moves the keyboard off the label.
    event->accept();
}

bool LabelEditor::focusNextPrevChild(bool) {
    return false;
}

void LabelEditor::fit() {
    ensurePolished();
    const QFontMetrics metrics = fontMetrics();
    const QString shown = text().isEmpty() ? placeholderText() : text();
    const int width = pyRound(metrics.horizontalAdvance(shown) + 2 * kLabelPadX + 8);
    const int room = parentWidget()->width() - x() - 8;
    resize(std::max(48, std::min(width, room)), pyRound(metrics.height() + 2 * kLabelPadY + 2));
}

// ---- SnapOverlay ----

SnapOverlay::SnapOverlay(const QImage& frozen, QScreen* screen)
    : SnapOverlay(frozen, screen, Options{}) {}

SnapOverlay::SnapOverlay(const QImage& frozen, QScreen* screen, const Options& options)
    : QWidget(nullptr, snapWindowFlags()), frozen_(frozen), dimmed_(dimmedCopy(frozen)),
      canvas_(logicalRect(frozen)), screen_(screen), settings_(options.settings),
      snapEnabled_(options.snapEnabled), palette_(ui::currentPalette()),
      hint_(options.hint, QGuiApplication::font(), palette_, canvas_.width() - 2 * kHintEdge),
      badgeFont_(chromeFont(QGuiApplication::font(), 11.5, QFont::DemiBold)),
      boxInfill_(readBoxInfill(options.settings)) {
    targets_.append(canvas_);
    targets_.append(options.targets);
    configure();
    buildToolbar();
}

SnapOverlay::~SnapOverlay() = default;

void SnapOverlay::configure() {
    setObjectName(QStringLiteral("snapOverlay"));
    setWindowTitle(QString::fromUtf8(identity().product) + QStringLiteral(" Snap"));
    // Every pixel is painted from the frozen image: no background to clear.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_QuitOnClose, false);
    // A macOS panel hides whenever another app is active; the covers on other screens must stay
    // while the user picks.
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    // The marker the Scratchpad sets: app hotkeys behave as they do in our other windows while
    // the overlay has focus.
    ui::setStyleProperty(this, ui::prop::fiorivoceHotkeyWindow, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    resize(canvas_.size());
}

void SnapOverlay::buildToolbar() {
    toolbar_ = new SnapToolbar(this, palette_);
    toolbar_->fitToWidth(canvas_.width() - 2 * kEdgeMargin);
    toolbar_->hide();
    connect(toolbar_, &SnapToolbar::toolChosen, this, &SnapOverlay::setTool);
    connect(toolbar_, &SnapToolbar::colorChosen, this, &SnapOverlay::setColor);
    connect(toolbar_, &SnapToolbar::undoClicked, this, &SnapOverlay::undo);
    connect(toolbar_, &SnapToolbar::redoClicked, this, &SnapOverlay::redo);
    connect(toolbar_, &SnapToolbar::dropClicked, this, &SnapOverlay::drop);
    connect(toolbar_, &SnapToolbar::keepClicked, this, &SnapOverlay::keep);
    connect(toolbar_, &SnapToolbar::snapClicked, this, &SnapOverlay::toggleSnap);
    toolbar_->setSnapEnabled(snapEnabled_);
    connect(toolbar_, &SnapToolbar::infillClicked, this, &SnapOverlay::toggleBoxInfill);
    toolbar_->setBoxInfill(boxInfill_);
}

void SnapOverlay::toggleBoxInfill() {
    const bool enabled = !boxInfill_;
    if (settings_ != nullptr) {
        settings_->save_setting(keys::snap_box_infill.key(), enabled);
    }
    setBoxInfill(enabled);
    Q_EMIT boxInfillToggled(enabled);
}

void SnapOverlay::setBoxInfill(bool enabled) {
    boxInfill_ = enabled;
    toolbar_->setBoxInfill(enabled);
}

void SnapOverlay::setSnapEnabled(bool enabled) {
    snapEnabled_ = enabled;
    toolbar_->setSnapEnabled(enabled);
}

void SnapOverlay::toggleSnap() {
    setSnapEnabled(!snapEnabled_);
    Q_EMIT snapToggled(snapEnabled_);
}

QColor SnapOverlay::currentColor() const {
    return QColor(QString::fromLatin1(kSwatches[color_].color));
}

// ---- showing, keeping, dropping ----

void SnapOverlay::present() {
    if (screen_) {
        setScreen(screen_);
        setGeometry(screen_->geometry());
    }
    // macOS shows the overlay as a cover rather than full screen, which would open its own
    // Space (macos.cover); that is not ported.
    showFullScreen();
    raise();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    bringToFront(this);
}

void SnapOverlay::keep() {
    if (done_) {
        return;
    }
    if (editor_) {
        commitLabel();
    }
    if (draft_) {
        finishDraft();
    }
    const QRect area = selection_.value_or(canvas_);
    const QImage image = renderKept(frozen_, area, stack_.items());
    done_ = true;
    Q_EMIT kept(image);
}

void SnapOverlay::drop() {
    if (done_) {
        return;
    }
    done_ = true;
    Q_EMIT dropped();
}

void SnapOverlay::selectAll() {
    origin_.reset();
    setSelection(canvas_);
    enterAnnotate();
    Q_EMIT selecting();
}

void SnapOverlay::startDrawing() {
    setTool(SnapTool::Pen);
    selectAll();
}

void SnapOverlay::clearSelection() {
    if (editor_) {
        closeLabel();
    }
    draft_.reset();
    origin_.reset();
    stack_ = AnnotationStack();
    phase_ = SnapPhase::Select;
    toolbar_->hide();
    selection_.reset();
    update();
}

void SnapOverlay::closeEvent(QCloseEvent* event) {
    // Closed from outside (a compositor keybind, Alt+F4): that is a cancel.
    drop();
    event->accept();
}

// ---- tools, colours, history ----

void SnapOverlay::setTool(SnapTool tool) {
    if (editor_) {
        commitLabel();
    }
    tool_ = tool;
    syncToolbar();
}

void SnapOverlay::setColor(int index) {
    if (index < 0 || index >= static_cast<int>(kSwatches.size())) {
        return;
    }
    color_ = index;
    if (editor_) {
        editor_->setColor(currentColor());
    }
    syncToolbar();
}

void SnapOverlay::undo() {
    if (editor_) {
        // The label being typed is the newest mark: undo throws it away.
        closeLabel();
        return;
    }
    afterHistory(stack_.undo());
}

void SnapOverlay::redo() {
    if (!editor_) {
        afterHistory(stack_.redo());
    }
}

void SnapOverlay::afterHistory(const std::optional<Annotation>& item) {
    if (item) {
        syncToolbar();
        updateMarks(annotationBounds(*item));
    }
}

void SnapOverlay::syncToolbar() {
    toolbar_->setState(tool_, color_, stack_.canUndo(), stack_.canRedo());
}

// ---- keys ----

void SnapOverlay::keyPressEvent(QKeyEvent* event) {
    if (!shortcut(event)) {
        QWidget::keyPressEvent(event);
    }
}

bool SnapOverlay::shortcut(QKeyEvent* event) {
    const int key = event->key();
    const Qt::KeyboardModifiers modifiers = event->modifiers() & kChord;
    std::function<void()> action;
    if (modifiers == Qt::ControlModifier) {
        if (key == Qt::Key_Z) {
            action = [this] {
                undo();
            };
        } else if (key == Qt::Key_Y) {
            action = [this] {
                redo();
            };
        }
    } else if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (key == Qt::Key_Z) {
            action = [this] {
                redo();
            };
        }
    } else if (modifiers == Qt::NoModifier) {
        if (isKeepKey(key)) {
            action = [this] {
                keep();
            };
        } else if (key == Qt::Key_Escape) {
            action = [this] {
                drop();
            };
        } else if (key == Qt::Key_S) {
            action = [this] {
                toggleSnap();
            };
        } else if (key >= Qt::Key_1 && key <= Qt::Key_4) {
            const int index = key - Qt::Key_1;
            action = [this, index] {
                setColor(index);
            };
        } else {
            for (const auto& [toolKey, tool] : kToolKeys) {
                if (key == toolKey) {
                    action = [this, tool] {
                        setTool(tool);
                    };
                }
            }
        }
    }
    if (!action) {
        return false;
    }
    event->accept();
    action();
    return true;
}

// ---- mouse ----

void SnapOverlay::mousePressEvent(QMouseEvent* event) {
    press(event);
}

void SnapOverlay::press(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || done_) {
        return;
    }
    if (editor_) {
        // A click away from the label being typed finishes it.
        commitLabel();
        return;
    }
    const QPointF point = event->position();
    if (const std::optional<Grab> grab = grabUnder(point.toPoint())) {
        startResize(*grab, point.toPoint());
    } else if (maySelect(point.toPoint())) {
        startSelection(point.toPoint());
    } else {
        startAnnotation(point);
    }
}

void SnapOverlay::mouseMoveEvent(QMouseEvent* event) {
    const QPointF point = event->position();
    if (origin_) {
        dragSelection(point.toPoint());
    } else if (grab_) {
        dragResize(point.toPoint());
    } else if (draft_) {
        extendCurrentDraft(point, event->modifiers());
    } else {
        updateCursor(point.toPoint());
    }
}

void SnapOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (origin_) {
        dragSelection(event->position().toPoint());
        finishSelection();
    } else if (grab_) {
        dragResize(event->position().toPoint());
        finishResize();
    } else if (draft_) {
        extendCurrentDraft(event->position(), event->modifiers());
        finishDraft();
    }
}

void SnapOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    const bool firstClick = phase_ == SnapPhase::Select && !selection_;
    if (firstClick && event->button() == Qt::LeftButton && !done_) {
        selectAll();
        return;
    }
    // Two quick clicks while annotating are two clicks (two step badges).
    press(event);
}

void SnapOverlay::updateCursor(QPoint point) {
    const std::optional<Grab> grab = grabUnder(point);
    const bool typing = phase_ == SnapPhase::Annotate && tool_ == SnapTool::Text && selection_ &&
                        selection_->contains(point);
    Qt::CursorShape shape = Qt::CrossCursor;
    if (grab) {
        shape = resizeCursor(*grab);
    } else if (typing) {
        shape = Qt::IBeamCursor;
    }
    if (cursor().shape() != shape) {
        setCursor(shape);
    }
}

// ---- selecting ----

bool SnapOverlay::maySelect(QPoint point) const {
    // Drags select at first; later only outside a selection with no marks.
    if (phase_ == SnapPhase::Select || !selection_) {
        return true;
    }
    return stack_.size() == 0 && !selection_->contains(point);
}

void SnapOverlay::startSelection(QPoint point) {
    const bool reselecting = phase_ == SnapPhase::Annotate;
    phase_ = SnapPhase::Select;
    toolbar_->hide();
    stack_ = AnnotationStack();
    origin_ = selectionPoint(point);
    setSelection(std::nullopt);
    if (reselecting) {
        // The hint pill may have moved out of the toolbar's way: repaint it all.
        update();
    }
    Q_EMIT selecting();
}

void SnapOverlay::dragSelection(QPoint point) {
    const QRect rect = span(*origin_, selectionPoint(point));
    const bool bigEnough = rect.width() >= kMinSelection && rect.height() >= kMinSelection;
    setSelection(bigEnough ? std::optional<QRect>(rect) : std::nullopt);
}

void SnapOverlay::finishSelection() {
    origin_.reset();
    if (selection_) {
        enterAnnotate();
    }
}

// ---- adjusting the selection ----

std::optional<Grab> SnapOverlay::grabUnder(QPoint point) const {
    // The selection's corner or edge under point, once it can be marked.
    if (phase_ != SnapPhase::Annotate || !selection_) {
        return std::nullopt;
    }
    return grabAt(*selection_, point);
}

void SnapOverlay::startResize(Grab grab, QPoint point) {
    grab_ = grab;
    grabbedFrom_ = *selection_;
    // Grabbed a few pixels off the edge: the edge keeps that distance from the pointer rather
    // than jumping to it.
    const QRect& selection = *selection_;
    const int edgeX = grab.x < 0 ? selection.left() : selection.right() + 1;
    const int edgeY = grab.y < 0 ? selection.top() : selection.bottom() + 1;
    grabShift_ = QPoint(grab.x != 0 ? edgeX - point.x() : 0, grab.y != 0 ? edgeY - point.y() : 0);
    toolbar_->hide();
    update();
}

void SnapOverlay::dragResize(QPoint point) {
    const QPoint edge = selectionPoint(point + grabShift_);
    const QRect rect = resized(grabbedFrom_, *grab_, edge);
    if (rect.width() >= kMinSelection && rect.height() >= kMinSelection) {
        setSelection(rect);
    }
}

void SnapOverlay::finishResize() {
    grab_.reset();
    enterAnnotate();
}

void SnapOverlay::enterAnnotate() {
    phase_ = SnapPhase::Annotate;
    toolbar_->move(placeToolbar(toolbar_->size(), *selection_, canvas_));
    syncToolbar();
    toolbar_->show();
    toolbar_->raise();
    update();
}

QPoint SnapOverlay::clamp(QPoint point) const {
    return QPoint(std::min(std::max(point.x(), canvas_.left()), canvas_.right() + 1),
                  std::min(std::max(point.y(), canvas_.top()), canvas_.bottom() + 1));
}

QPoint SnapOverlay::selectionPoint(QPoint point) const {
    if (snapEnabled_) {
        point =
            snapPoint(QPointF(point), std::span<const QRect>(targets_.constData(), targets_.size()))
                .toPoint();
    }
    return clamp(point);
}

void SnapOverlay::setSelection(std::optional<QRect> rect) {
    const QRegion dirty = decorRegion();
    selection_ = rect;
    update(dirty.united(decorRegion()));
}

QRegion SnapOverlay::decorRegion() const {
    // What the selection's look touches: frame, size readout and the hint pill.
    QRegion region(hintRect());
    if (selection_) {
        region = region.united(QRegion(selection_->adjusted(-4, -4, 4, 4)));
        region = region.united(QRegion(currentBadgeRect().adjusted(-2, -2, 2, 2)));
    }
    return region;
}

// ---- annotating ----

void SnapOverlay::startAnnotation(QPointF point) {
    if (!selection_ || !selection_->contains(point.toPoint())) {
        return;
    }
    if (tool_ == SnapTool::Text) {
        openLabel(point);
    } else if (tool_ == SnapTool::Step) {
        add(Annotation::step(inside(point), stack_.nextStepNumber(), currentColor()));
    } else {
        draft_ = newDraft(tool_, annotationPoint(point), currentColor(), boxInfill_);
        if (draft_) {
            updateMarks(annotationBounds(*draft_));
        }
    }
}

void SnapOverlay::extendCurrentDraft(QPointF point, Qt::KeyboardModifiers modifiers) {
    const QRectF before = annotationBounds(*draft_);
    extendDraft(*draft_, annotationPoint(point), modifiers.testFlag(Qt::ShiftModifier),
                QRectF(*selection_));
    updateMarks(before.united(annotationBounds(*draft_)));
}

QPointF SnapOverlay::annotationPoint(QPointF point) const {
    if (snapEnabled_ &&
        (tool_ == SnapTool::Box || tool_ == SnapTool::Ellipse || tool_ == SnapTool::Arrow)) {
        QList<QRect> targets{*selection_};
        targets.append(targets_);
        return inside(
            snapPoint(point, std::span<const QRect>(targets.constData(), targets.size())));
    }
    if (tool_ == SnapTool::Redact) {
        return inside(point);
    }
    return point;
}

void SnapOverlay::finishDraft() {
    Annotation draft = std::move(*draft_);
    draft_.reset();
    if (meaningful(draft)) {
        add(std::move(draft));
    } else {
        updateMarks(annotationBounds(draft));
    }
}

void SnapOverlay::add(Annotation item) {
    const QRectF bounds = annotationBounds(item);
    stack_.add(std::move(item));
    syncToolbar();
    updateMarks(bounds);
}

QPointF SnapOverlay::inside(QPointF point) const {
    const double left = selection_->left();
    const double top = selection_->top();
    return QPointF(std::min(std::max(point.x(), left), left + selection_->width()),
                   std::min(std::max(point.y(), top), top + selection_->height()));
}

void SnapOverlay::updateMarks(const QRectF& rect) {
    update(rect.toAlignedRect().adjusted(-2, -2, 2, 2));
}

// ---- text labels ----

void SnapOverlay::openLabel(QPointF point) {
    auto* editor = new LabelEditor(this, currentColor());
    connect(editor, &LabelEditor::committed, this, &SnapOverlay::commitLabel);
    connect(editor, &LabelEditor::cancelled, this, &SnapOverlay::closeLabel);
    editor->move(labelOrigin(*selection_, point, editor->size()));
    editor->refit();
    editor_ = editor;
    editor->show();
    editor->setFocus(Qt::MouseFocusReason);
}

void SnapOverlay::commitLabel() {
    if (!editor_) {
        return;
    }
    Annotation label = editor_->annotation();
    closeLabel();
    if (meaningful(label)) {
        add(std::move(label));
    }
}

void SnapOverlay::closeLabel() {
    LabelEditor* editor = editor_;
    editor_ = nullptr;
    if (editor == nullptr) {
        return;
    }
    editor->hide();
    editor->deleteLater();
    setFocus(Qt::OtherFocusReason);
}

// ---- painting ----

void SnapOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (!canvas_.contains(rect())) {
        painter.fillRect(rect(), QColor(0, 0, 0));
    }
    painter.drawImage(QPointF(0, 0), dimmed_);
    if (selection_) {
        paintSelection(painter);
    }
    if (toolbarShown()) {
        // The toolbar is exactly its panel; its shadow belongs to the screen beneath, so clicks
        // beside the panel still reach the overlay.
        paintShadow(painter, QRectF(toolbar_->geometry()), kToolbarRadius);
    }
    const QRect hint = hintRect();
    if (!hint.isNull()) {
        hint_.paint(painter, hint);
    }
}

void SnapOverlay::paintSelection(QPainter& painter) {
    const QRect selection = *selection_;
    painter.save();
    painter.setClipRect(selection);
    painter.drawImage(QPointF(0, 0), frozen_);
    if (phase_ == SnapPhase::Annotate) {
        paintAnnotations(painter);
    }
    painter.restore();
    paintFrame(painter, selection);
    if (phase_ == SnapPhase::Select || grab_) {
        paintBadge(painter, currentBadgeRect(), badgeText(), badgeFont_, palette_);
    }
}

void SnapOverlay::paintAnnotations(QPainter& painter) const {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (const Annotation& item : stack_.items()) {
        paintAnnotation(painter, item);
    }
    if (draft_) {
        paintAnnotation(painter, *draft_);
    }
}

QString SnapOverlay::badgeText() const {
    const QRect crop = deviceRect(*selection_, frozen_.devicePixelRatio(), frozen_.rect());
    return QStringLiteral(u"%1 × %2").arg(crop.width()).arg(crop.height());
}

QRect SnapOverlay::currentBadgeRect() const {
    return badgeRect(badgeSize(badgeText(), badgeFont_), *selection_, canvas_);
}

QRect SnapOverlay::hintRect() const {
    const QSize size = hint_.size();
    const int x = canvas_.center().x() - size.width() / 2;
    const int top = canvas_.top() + kHintEdge;
    const int bottom = canvas_.bottom() + 1 - kHintEdge - size.height();
    for (const int y : {top, bottom}) {
        const QRect rect(QPoint(x, y), size);
        if (!blocksHint(rect)) {
            return rect;
        }
    }
    return QRect();
}

bool SnapOverlay::toolbarShown() const {
    return phase_ == SnapPhase::Annotate && !toolbar_->isHidden();
}

bool SnapOverlay::blocksHint(const QRect& rect) const {
    if (selection_ && rect.intersects(*selection_)) {
        return true;
    }
    return toolbarShown() && rect.intersects(toolbar_->geometry().adjusted(-8, -8, 8, 8));
}

} // namespace fv::ui_snap
