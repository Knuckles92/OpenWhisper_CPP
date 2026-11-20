#include "ui_walkthrough/walkthrough_panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <vector>

#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QToolTip>
#include <QWheelEvent>

#include "base/clock.h"
#include "base/utf8.h"
#include "settings/enums.h"
#include "ui_snap/pointed.h"
#include "ui_theme/palette.h"
#include "ui_walkthrough/screenshot_flight.h"
#include "ui_walkthrough/walkthrough_icons.h"
#include "ui_walkthrough/walkthrough_snap_menu.h"
#include "walkthrough/document.h"

namespace fv {
namespace {

using namespace ui_walkthrough;

// Chips drawn on a screenshot stay dark in either theme: the picture is what they sit on.
const QColor kOnImage(28, 28, 30, 220);
const QColor kOnImageHover(72, 72, 74, 235);
const QColor kOnImageText(245, 245, 247);
const QColor kOnImageDanger(255, 105, 97);

constexpr double kTau = 2.0 * std::numbers::pi;

// The thumbnail a ShotItem holds: the column keeps it, the panel paints it.
class Thumbnail final : public walkthrough::ShotImage {
public:
    explicit Thumbnail(QImage image) : image_(std::move(image)) {}

    const QImage& image() const { return image_; }
    int width() const override { return image_.width(); }
    int height() const override { return image_.height(); }
    double device_pixel_ratio() const override { return image_.devicePixelRatio(); }
    void save_png(const std::filesystem::path& path) const override {
        if (!image_.save(QString::fromStdString(path_to_utf8(path)), "PNG")) {
            throw std::runtime_error("Could not write the screenshot file");
        }
    }
    std::vector<std::uint8_t> png_bytes() const override {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !image_.save(&buffer, "PNG")) {
            return {};
        }
        return {bytes.begin(), bytes.end()};
    }

private:
    QImage image_;
};

const QImage& thumbnailOf(const ShotItem& shot) {
    static const QImage none;
    const auto* thumbnail = dynamic_cast<const Thumbnail*>(shot.image.get());
    return thumbnail != nullptr ? thumbnail->image() : none;
}

QString plural(int count, const char* word) {
    return QStringLiteral("%1 %2%3")
        .arg(count)
        .arg(QString::fromLatin1(word))
        .arg(count == 1 ? QString() : QStringLiteral("s"));
}

QString clockText(double seconds) {
    return QString::fromStdString(walkthrough::clock(seconds));
}

// Python's round() of a colour channel: half to even.
int roundChannel(double value) {
    return static_cast<int>(std::nearbyint(value));
}

QColor token(std::string_view name, std::optional<int> alpha = std::nullopt) {
    return ui::tokenColor(name, alpha);
}

} // namespace

WalkthroughPanel::WalkthroughPanel(const Clock& clock, QWidget* parent)
    : QWidget(parent), clock_(clock), drag_(this), clockStart_(clock.steady()),
      snapMode_(QStringLiteral("region")) {
    setObjectName(QStringLiteral("walkthroughPanel"));
    setWindowTitle(QStringLiteral("Walkthrough"));
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    // Its buttons explain themselves though the column is never active.
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                   Qt::WindowDoesNotAcceptFocus);
    setMouseTracking(true);
    frame_.setParent(this);
    frame_.setInterval(kFrameMs);
    connect(&frame_, &QTimer::timeout, this, &WalkthroughPanel::advanceFrame);
}

WalkthroughPanel::~WalkthroughPanel() {
    if (menu_ != nullptr) {
        delete menu_.data();
    }
}

// ---- what the walkthrough does ----

void WalkthroughPanel::begin() {
    endCaptureAnimation();
    column_ = WalkthroughColumn();
    scroll_ = scrolledBack_ = 0.0;
    summary_.clear();
    recordedS_ = 0.0;
    hover_.clear();
    pressed_.clear();
    hoverShot_ = 0;
    shotRects_.clear();
    countdown_ = 0;
    paused_ = false;
    pausedMs_ = pausedAt_ = 0;
    clockStart_ = clock_.steady();
    enter(State::Recording);
    drag_.reset();
    place();
    show();
    raise();
    last_ = now();
    frame_.start();
}

void WalkthroughPanel::hear(double level) {
    if (state_ == State::Recording) {
        column_.hear(level);
    }
}

void WalkthroughPanel::addShot(int number, const QImage& image, double keptAtS,
                               std::optional<QRect> sourceRect) {
    if (state_ != State::Recording || image.isNull()) {
        return;
    }
    endCaptureAnimation();
    // A thumbnail is all the column draws; a full screen is many MB.
    QImage small = image.scaledToWidth(kThumbPx, Qt::SmoothTransformation);
    column_.add_shot(number, std::make_shared<const Thumbnail>(std::move(small)), now(), keptAtS);
    // A new screenshot brings the newest back into view.
    scrolledBack_ = 0.0;
    if (sourceRect && !sourceRect->isEmpty() && isVisible()) {
        if (const std::optional<QRectF> target = flightTarget(number)) {
            if (flight_ == nullptr) {
                flight_ = new ScreenshotFlight(this);
            }
            // More detail during lift-off, bounded even for a 4K screen.
            const QImage flying =
                image.scaled(1600, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            flight_->start(number, flying, QRectF(*sourceRect), *target);
        }
    }
    update();
}

void WalkthroughPanel::endCaptureAnimation() {
    if (flight_ != nullptr) {
        flight_->land();
        update();
    }
}

std::optional<QRectF> WalkthroughPanel::flightTarget(int number) const {
    const QRectF body = bodyRect();
    for (const Block& block : layout(body.width())) {
        if (block.kind != Kind::Shot) {
            continue;
        }
        const auto& shot = std::get<ShotItem>(*block.item);
        if (shot.number == number) {
            const auto [w, h] = shotSize(shot, body.width());
            const QRectF rect(body.left() + (body.width() - w) / 2,
                              body.top() + block.top - scroll_, w, h);
            return rect.translated(QPointF(pos()));
        }
    }
    return std::nullopt;
}

void WalkthroughPanel::hideEvent(QHideEvent* event) {
    endCaptureAnimation();
    QWidget::hideEvent(event);
}

void WalkthroughPanel::removeShot(int number) {
    if (column_.remove_shot(number)) {
        if (flight_ != nullptr && flight_->number() == number) {
            endCaptureAnimation();
        }
        settleHover();
    }
}

void WalkthroughPanel::moveShot(int number, int step) {
    if (column_.move_shot(number, step)) {
        settleHover();
    }
}

bool WalkthroughPanel::showsRecording() const {
    return isVisible() && (state_ == State::Recording || state_ == State::Writing);
}

void WalkthroughPanel::setSnapMode(const QString& mode) {
    snapMode_ = mode;
    update();
}

void WalkthroughPanel::setSnapMode(const QString& mode, const QString& shortcut) {
    snapMode_ = mode;
    snapShortcut_ = shortcut;
    update();
}

void WalkthroughPanel::setAutoMode(bool enabled) {
    autoMode_ = enabled;
    update();
}

void WalkthroughPanel::setCountdown(int seconds) {
    countdown_ = seconds;
    update();
}

void WalkthroughPanel::setPaused(bool paused) {
    if (paused == paused_ || state_ != State::Recording) {
        return;
    }
    if (paused) {
        hold(false);
        pausedAt_ = elapsedMs();
    } else {
        pausedMs_ += elapsedMs() - pausedAt_;
    }
    paused_ = paused;
    update();
}

void WalkthroughPanel::setNotesOn(bool on) {
    notesOn_ = on;
    update();
}

QStringList WalkthroughPanel::snapModes() const {
    QStringList modes;
    for (const Json& value : enums::values(enums::EnumId::WalkthroughSnapMode)) {
        const QString mode = QString::fromStdString(value.get<std::string>());
        if (mode != QStringLiteral("window") || ui_snap::pointedSupported()) {
            modes.append(mode);
        }
    }
    return modes;
}

void WalkthroughPanel::finish() {
    endCaptureAnimation();
    closeMenu();
    countdown_ = 0;
    if (state_ != State::Recording) {
        return;
    }
    hold(false);
    recordedS_ = recorded();
    paused_ = false;
    hoverShot_ = 0;
    enter(State::Writing);
}

void WalkthroughPanel::complete(int shots, double durationS) {
    if (state_ != State::Recording && state_ != State::Writing) {
        return;
    }
    endCaptureAnimation();
    summary_ = clockText(durationS) + QStringLiteral(u" · ") + plural(shots, "screenshot");
    enter(State::Done);
}

void WalkthroughPanel::dismiss() {
    endCaptureAnimation();
    closeMenu();
    hold(false);
    if (state_ == State::Recording || state_ == State::Writing || state_ == State::Done) {
        summary_.clear();
        enter(State::Done);
        // Start the fade now instead of after the hold.
        stateAt_ -= kHoldS;
    }
}

SnapMenu* WalkthroughPanel::menu() const {
    return menu_.data();
}

// ---- frames ----

double WalkthroughPanel::now() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

std::int64_t WalkthroughPanel::elapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_.steady() - clockStart_)
        .count();
}

double WalkthroughPanel::recorded() const {
    if (state_ != State::Recording) {
        return recordedS_;
    }
    const std::int64_t at = paused_ ? pausedAt_ : elapsedMs();
    return static_cast<double>(at - pausedMs_) / 1000.0;
}

void WalkthroughPanel::enter(State state) {
    state_ = state;
    stateAt_ = now();
}

double WalkthroughPanel::since() const {
    return now() - stateAt_;
}

void WalkthroughPanel::advanceFrame() {
    const double at = now();
    if (flight_ != nullptr && flight_->number() != 0) {
        const ShotItem* shot = column_.shot(flight_->number());
        const std::optional<QRectF> target = flightTarget(flight_->number());
        if (shot == nullptr || !target || at - shot->born >= kFlightS) {
            endCaptureAnimation();
        } else {
            flight_->advance((at - shot->born) / kFlightS, *target);
        }
    }
    const double dt = std::min(0.1, at - last_);
    last_ = at;
    if (state_ == State::Recording && !paused_) {
        column_.advance(dt, at);
    }
    double target = 1.0;
    if (state_ == State::Done) {
        target = since() >= kHoldS ? 0.0 : 1.0;
    } else if (state_ == State::Writing && since() >= kGiveUpS) {
        target = 0.0;
    }
    const double step = dt / kFadeS;
    opacity_ =
        target > opacity_ ? std::min(target, opacity_ + step) : std::max(target, opacity_ - step);
    if (target == 0.0 && opacity_ <= 0.0) {
        frame_.stop();
        state_ = State::Hidden;
        hide();
        return;
    }
    update();
}

void WalkthroughPanel::place() {
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect area = screen->availableGeometry();
    const int height =
        static_cast<int>(std::min(static_cast<double>(kMaxHeight), area.height() * kHeightShare)) +
        2 * kShadow;
    const int width = kWidth + 2 * kShadow;
    setFixedSize(width, height);
    // Python's floor division of the spare height.
    const int spare = area.height() - height;
    const int half = spare >= 0 ? spare / 2 : -((-spare + 1) / 2);
    move(area.right() - width - kScreenMargin + kShadow + 1, area.top() + half);
}

// ---- the pointer ----

std::vector<WalkthroughPanel::Control> WalkthroughPanel::controls() const {
    std::vector<Control> found;
    for (const Control& control : footer()) {
        if (control.enabled) {
            found.push_back(control);
        }
    }
    if (state_ == State::Recording && shotRect(hoverShot_)) {
        for (const Control& tool : shotTools(hoverShot_)) {
            if (tool.enabled) {
                found.push_back(tool);
            }
        }
    }
    return found;
}

QString WalkthroughPanel::hit(QPointF position) const {
    for (const Control& control : controls()) {
        if (control.rect.contains(position)) {
            return control.key;
        }
    }
    return QString();
}

void WalkthroughPanel::setHover(std::optional<QPointF> position) {
    int shot = 0;
    if (position && state_ == State::Recording) {
        // The tools sit inside their screenshot, so hovering them keeps it.
        for (const auto& [number, rect] : shotRects_) {
            if (rect.adjusted(-4, -4, 4, 4).contains(*position)) {
                shot = number;
                break;
            }
        }
    }
    hoverShot_ = shot;
    const QString key = position ? hit(*position) : QString();
    if (key != hover_) {
        hover_ = key;
        setCursor(key.isEmpty() ? Qt::OpenHandCursor : Qt::PointingHandCursor);
    }
    update();
}

void WalkthroughPanel::settleHover() {
    // The board changed under the pointer: find what it is over once redrawn.
    shotRects_.clear();
    hoverShot_ = 0;
    hover_.clear();
    repaint();
    setHover(mapFromGlobal(QCursor::pos()).toPointF());
}

void WalkthroughPanel::mousePressEvent(QMouseEvent* event) {
    const QString key = event->button() == Qt::LeftButton ? hit(event->position()) : QString();
    if (key != QStringLiteral("snap-menu")) {
        // Any other click in the column puts the menu away.
        closeMenu();
    }
    if (!key.isEmpty()) {
        pressed_ = key;
        if (key == QStringLiteral("note")) {
            hold(true);
        }
        event->accept();
        update();
        return;
    }
    if (!drag_.press(event)) {
        QWidget::mousePressEvent(event);
    }
}

void WalkthroughPanel::mouseMoveEvent(QMouseEvent* event) {
    if (drag_.move(event)) {
        return;
    }
    if (column_.holding && !(event->buttons() & Qt::LeftButton)) {
        // The button came up somewhere this window never heard of.
        pressed_.clear();
        hold(false);
    }
    setHover(event->position());
    QWidget::mouseMoveEvent(event);
}

void WalkthroughPanel::mouseReleaseEvent(QMouseEvent* event) {
    const QString pressed = std::exchange(pressed_, QString());
    if (pressed == QStringLiteral("note")) {
        // Let go anywhere: the note ends.
        hold(false);
    } else if (!pressed.isEmpty() && hit(event->position()) == pressed) {
        click(pressed);
    }
    if (!pressed.isEmpty()) {
        event->accept();
        update();
        return;
    }
    if (!drag_.release(event)) {
        QWidget::mouseReleaseEvent(event);
    }
}

void WalkthroughPanel::leaveEvent(QEvent* event) {
    setHover(std::nullopt);
    QWidget::leaveEvent(event);
}

void WalkthroughPanel::wheelEvent(QWheelEvent* event) {
    const double notches = event->angleDelta().y() / 120.0;
    scrolledBack_ = std::max(0.0, scrolledBack_ + notches * kWheelStep);
    event->accept();
    update();
}

bool WalkthroughPanel::event(QEvent* event) {
    if (event->type() != QEvent::ToolTip) {
        return QWidget::event(event);
    }
    auto* help = static_cast<QHelpEvent*>(event);
    const QPointF at(help->pos());
    QString key = hit(at).section(u':', 0, 0);
    if (key == QStringLiteral("pause") && paused_) {
        key = QStringLiteral("resume");
    }
    if (key == QStringLiteral("cancel") && state_ == State::Writing) {
        key = QStringLiteral("cancel-writing");
    }
    if (key.isEmpty() && !notesOn_ && footerControl(QStringLiteral("note")).rect.contains(at)) {
        key = QStringLiteral("note-off");
    }
    const QString text = QString::fromStdString(tooltip_text(
        key.toStdString(), snapModeLabel(snapMode_).toStdString(), autoMode_, countdown_));
    if (!text.isEmpty()) {
        QToolTip::showText(help->globalPos(), text, this);
    } else {
        QToolTip::hideText();
        event->ignore();
    }
    return true;
}

void WalkthroughPanel::click(const QString& key) {
    const QString name = key.section(u':', 0, 0);
    const int number = key.section(u':', 1).toInt();
    if (name == QStringLiteral("up")) {
        Q_EMIT moveRequested(number, -1);
    } else if (name == QStringLiteral("down")) {
        Q_EMIT moveRequested(number, 1);
    } else if (name == QStringLiteral("remove")) {
        Q_EMIT removeRequested(number);
    } else if (name == QStringLiteral("pause")) {
        Q_EMIT pauseRequested(!paused_);
    } else if (name == QStringLiteral("snap-menu")) {
        if (menu_ == nullptr) {
            openMenu();
        } else {
            closeMenu();
        }
    } else if (name == QStringLiteral("snap")) {
        Q_EMIT snapRequested();
    } else if (name == QStringLiteral("undo")) {
        Q_EMIT undoRequested();
    } else if (name == QStringLiteral("cancel")) {
        Q_EMIT cancelRequested();
    } else if (name == QStringLiteral("done")) {
        Q_EMIT finishRequested();
    }
}

void WalkthroughPanel::hold(bool on) {
    if (on == column_.holding || (on && (state_ != State::Recording || paused_))) {
        return;
    }
    column_.hold_note(on, recorded());
    Q_EMIT noteHeld(on);
    update();
}

void WalkthroughPanel::openMenu() {
    auto* menu = new SnapMenu(clock_, snapModes(), snapMode_, snapShortcut_);
    menu_ = menu;
    connect(menu, &SnapMenu::chosen, this, &WalkthroughPanel::snapModeChosen);
    const QRect cardRect = card().toRect().translated(mapToGlobal(rect().topLeft()));
    const QRectF snap = footerControl(QStringLiteral("snap")).rect;
    menu->openBeside(cardRect, mapToGlobal(snap.bottomLeft().toPoint()).y(), frameGeometry());
}

void WalkthroughPanel::closeMenu() {
    if (menu_ != nullptr) {
        menu_->close();
    }
}

// ---- geometry ----

QRectF WalkthroughPanel::card() const {
    return QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow);
}

QRectF WalkthroughPanel::inner() const {
    return card().adjusted(kPadding, kPadding, -kPadding, -kPadding);
}

double WalkthroughPanel::footerTop() const {
    // Where the footer's buttons begin; the rule above them is 12 px higher.
    return inner().bottom() - 2 * kFootH - kFootGap;
}

QRectF WalkthroughPanel::bodyRect() const {
    const QRectF in = inner();
    // Title, recording status, divider and the gap below the header.
    const double top = in.top() + 78;
    return QRectF(in.left(), top, in.width(), footerTop() - 26 - top);
}

double WalkthroughPanel::cancelWidening() const {
    // 0 while it records; then Cancel grows to fill its row, the others gone.
    if (state_ == State::Writing) {
        return ease_out(since() / kFadeS);
    }
    return state_ == State::Recording ? 0.0 : 1.0;
}

std::vector<WalkthroughPanel::Control> WalkthroughPanel::footer() const {
    // While the walkthrough is written only Cancel is left, filling its row.
    const QRectF in = inner();
    const double top = footerTop();
    const double bottom = top + kFootH + kFootGap;
    const bool recording = state_ == State::Recording;
    const double half = (in.width() - kFootGap) / 2;
    const QRectF snap(in.left(), top, half - kSnapMenuW, kFootH);
    const QRectF snapMenu(snap.right(), top, kSnapMenuW, kFootH);
    const QRectF note(snapMenu.right() + kFootGap, top, half, kFootH);
    const QRectF undo(in.left(), bottom, kFootIconW, kFootH);
    const QRectF pause(undo.right() + kFootGap, bottom, kFootIconW, kFootH);
    QRectF cancel(pause.right() + kFootGap, bottom, kFootIconW, kFootH);
    const QRectF done(cancel.right() + kFootGap, bottom, in.right() - cancel.right() - kFootGap,
                      kFootH);
    const double widen = cancelWidening();
    if (widen != 0.0) {
        cancel = QRectF(cancel.left() + (in.left() - cancel.left()) * widen, bottom,
                        kFootIconW + (in.width() - kFootIconW) * widen, kFootH);
    }
    return {
        // Paused, Auto Mode's drawing is off: Draw would do nothing.
        {QStringLiteral("snap"), snap, recording && !(autoMode_ && paused_)},
        {QStringLiteral("snap-menu"), snapMenu, recording},
        {QStringLiteral("note"), note, recording && notesOn_ && !paused_},
        {QStringLiteral("pause"), pause, recording},
        {QStringLiteral("undo"), undo, recording && column_.shots() > 0},
        {QStringLiteral("cancel"), cancel, state_ == State::Recording || state_ == State::Writing},
        {QStringLiteral("done"), done, recording},
    };
}

WalkthroughPanel::Control WalkthroughPanel::footerControl(const QString& key) const {
    for (const Control& control : footer()) {
        if (control.key == key) {
            return control;
        }
    }
    return {};
}

std::optional<QRectF> WalkthroughPanel::shotRect(int number) const {
    for (const auto& [drawn, rect] : shotRects_) {
        if (drawn == number) {
            return rect;
        }
    }
    return std::nullopt;
}

std::vector<WalkthroughPanel::Control> WalkthroughPanel::shotTools(int number) const {
    // A screenshot's hover tools at its top right, and whether each can act.
    const QRectF rect = shotRect(number).value_or(QRectF());
    const int placed = column_.place(number);
    const int count = column_.shots();
    double right = rect.right() - 8;
    std::vector<Control> tools;
    const std::array<std::pair<const char*, bool>, 3> names{
        {{"remove", true}, {"down", placed < count}, {"up", placed > 1}}};
    for (const auto& [name, enabled] : names) {
        tools.push_back({QStringLiteral("%1:%2").arg(QString::fromLatin1(name)).arg(number),
                         QRectF(right - kTool, rect.top() + 8, kTool, kTool), enabled});
        right -= kTool + kToolGap;
    }
    return tools;
}

double WalkthroughPanel::slotTicksWidth(double width) const {
    // How wide the open slot's rows of ticks are, inside a body width wide.
    return width - 2 * kSlotPadX - kSlotBadge - 6 - 6 - kSlotTimeW;
}

std::vector<WalkthroughPanel::Block> WalkthroughPanel::layout(double width) const {
    std::vector<Block> laid;
    double y = 0.0;
    const auto& items = column_.items;
    // While it records, the items after the last screenshot are the section being spoken.
    std::size_t closed = items.size();
    if (state_ == State::Recording) {
        closed = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (std::holds_alternative<ShotItem>(items[i])) {
                closed = i + 1;
            }
        }
    }
    for (std::size_t i = 0; i < closed; ++i) {
        const Item& item = items[i];
        double height = 0.0;
        if (const auto* paragraph = std::get_if<Paragraph>(&item)) {
            if (paragraph->note) {
                height = kNotePadTop + kNoteHeadH + 3 +
                         std::max(1, rows(*paragraph, width - 2 * kNotePadX)) * kRowPitch +
                         kNotePadBottom;
                laid.push_back({Kind::Note, &item, {}, y, height});
            } else {
                if (paragraph->ticks.empty()) {
                    continue;
                }
                height = rows(*paragraph, width) * kRowPitch;
                laid.push_back({Kind::Words, &item, {}, y, height});
            }
        } else {
            height = shotSize(std::get<ShotItem>(item), width).second;
            laid.push_back({Kind::Shot, &item, {}, y, height});
        }
        y += height + kGap;
    }
    if (state_ == State::Recording) {
        const double ticks = slotTicksWidth(width);
        Block slot{Kind::Slot, nullptr, {}, y, 0.0};
        int count = 0;
        for (std::size_t i = closed; i < items.size(); ++i) {
            const auto& paragraph = std::get<Paragraph>(items[i]);
            slot.open.push_back(&paragraph);
            count += std::max(1, rows(paragraph, ticks));
        }
        slot.height =
            kSlotPadTop + std::max(kSlotBadge, count * kRowPitch) + 6 + kSlotHintH + kSlotPadBottom;
        laid.push_back(std::move(slot));
    }
    return laid;
}

std::pair<double, double> WalkthroughPanel::shotSize(const ShotItem& shot, double width) const {
    const double ratio =
        static_cast<double>(shot.image->height()) / std::max(1, shot.image->width());
    const double h = std::min(kShotMaxH, width * ratio);
    return {h / ratio, h};
}

double WalkthroughPanel::noteLength(const Paragraph& note) const {
    const double end = note.ended_s.value_or(recorded());
    return std::max(0.0, end - note.started_s);
}

double WalkthroughPanel::sectionStarted() const {
    // When the section being spoken began: the newest screenshot still kept.
    double started = 0.0;
    bool any = false;
    for (const Item& item : column_.items) {
        if (const auto* shot = std::get_if<ShotItem>(&item)) {
            started = any ? std::max(started, shot->kept_at_s) : shot->kept_at_s;
            any = true;
        }
    }
    return started;
}

// ---- painting ----

void WalkthroughPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(ease_out(opacity_));
    const QRectF cardRect = card();
    // Shadow, then the card.
    for (int i = kShadow; i > 0; i -= 2) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(
            QColor(0, 0, 0, static_cast<int>(7 * (1 - static_cast<double>(i) / kShadow) + 2)));
        painter.drawRoundedRect(cardRect.adjusted(-i, -i + 4, i, i + 4), kRadius + i, kRadius + i);
    }
    painter.setBrush(token("surface"));
    painter.setPen(QPen(token("border-strong"), 1.0));
    painter.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    const QRectF in = inner();
    paintHeader(painter, in);
    const double foot = footerTop() - 12;
    paintBody(painter, bodyRect());
    painter.setPen(QPen(token("border"), 1.0));
    painter.drawLine(QPointF(in.left(), foot), QPointF(in.right(), foot));
    paintFooter(painter);
}

void WalkthroughPanel::paintHeader(QPainter& painter, const QRectF& in) {
    QFont title(font());
    title.setPixelSize(21);
    title.setWeight(QFont::DemiBold);
    painter.setFont(title);
    painter.setPen(token("text"));
    painter.drawText(QRectF(in.left(), in.top(), in.width(), 28), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Walkthrough"));

    QFont sub(font());
    sub.setPixelSize(13);
    painter.setFont(sub);
    const double y = in.top() + 36;
    double x = in.left();
    QString text;
    if (state_ == State::Recording && paused_) {
        paintWalkthroughIcon(painter, "pause", QPointF(x + 5, y + 9), 12, token("text-secondary"));
        x += 16;
        const int shots = column_.shots();
        text = QStringLiteral("Paused  ") + clockText(recorded());
        if (shots != 0) {
            text += QStringLiteral(u"  ·  ") + plural(shots, "shot");
        }
    } else if (state_ == State::Recording) {
        const double pulse =
            0.55 +
            0.45 * (0.5 + 0.5 * std::cos(static_cast<double>(elapsedMs()) / 1000.0 * kTau / 1.6));
        painter.setPen(Qt::NoPen);
        painter.setBrush(token("danger", static_cast<int>(255 * pulse)));
        painter.drawEllipse(QPointF(x + 5, y + 9), 4.5, 4.5);
        x += 16;
        const int shots = column_.shots();
        text = QStringLiteral("Recording  ") + clockText(recorded());
        if (shots != 0) {
            text += QStringLiteral(u"  ·  ") + plural(shots, "shot");
        }
    } else if (state_ == State::Writing) {
        const int dots = 1 + static_cast<int>(since() / 0.4) % 3;
        text = QStringLiteral("Writing it up") + QString(dots, u'.');
    } else {
        text = summary_.isEmpty() ? QStringLiteral("Walkthrough") : summary_;
    }
    painter.setPen(token("text-secondary"));
    painter.drawText(QRectF(x, y, in.right() - x, 18), Qt::AlignLeft | Qt::AlignVCenter, text);
    const double rule = y + 28;
    painter.setPen(QPen(token("border"), 1.0));
    painter.drawLine(QPointF(in.left(), rule), QPointF(in.right(), rule));
}

void WalkthroughPanel::paintBody(QPainter& painter, const QRectF& body) {
    const std::vector<Block> laid = layout(body.width());
    const double contentH = laid.empty() ? 0.0 : laid.back().top + laid.back().height;
    // Keep the newest at the bottom in view, gliding there, unless the user scrolled back.
    const double newest = std::max(0.0, contentH - body.height());
    scrolledBack_ = std::min(scrolledBack_, newest);
    const double target = newest - scrolledBack_;
    scroll_ += (target - scroll_) * std::min(1.0, kFrameMs / 1000.0 / 0.18);
    painter.save();
    QPainterPath clip;
    clip.addRect(body.adjusted(-kShadow, 0, kShadow, 0));
    painter.setClipPath(clip);
    const double at = now();
    const double morph =
        (state_ == State::Writing || state_ == State::Done) ? ease_out(since() / kMorphS) : 0.0;
    shotRects_.clear();
    for (const Block& block : laid) {
        const double y = body.top() + block.top - scroll_;
        if (y + block.height < body.top() - 20 || y > body.bottom() + 20) {
            continue;
        }
        switch (block.kind) {
        case Kind::Words:
            paintTicks(painter, std::get<Paragraph>(*block.item), body.left(), y, body.width(), at,
                       morph, token("text-soft"));
            break;
        case Kind::Note:
            paintNote(painter, std::get<Paragraph>(*block.item),
                      QRectF(body.left(), y, body.width(), block.height), at, morph);
            break;
        case Kind::Shot:
            paintShot(painter, std::get<ShotItem>(*block.item), body.left(), y, body.width(), at);
            break;
        case Kind::Slot:
            paintSlot(painter, block.open, QRectF(body.left(), y, body.width(), block.height), at);
            break;
        }
    }
    painter.restore();
    // Fade the top edge where older items scroll away.
    if (scroll_ > 1.0) {
        painter.save();
        const QRectF fade(body.left(), body.top(), body.width(), 22);
        QLinearGradient gradient(fade.topLeft(), fade.bottomLeft());
        gradient.setColorAt(0.0, token("surface"));
        QColor surface = token("surface");
        surface.setAlpha(0);
        gradient.setColorAt(1.0, surface);
        painter.fillRect(fade, gradient);
        painter.restore();
    }
}

void WalkthroughPanel::paintTicks(QPainter& painter, const Paragraph& paragraph, double left,
                                  double top, double width, double at, double morph,
                                  const QColor& color) {
    const int per = per_row(width);
    const QColor line = token("text-faint");
    painter.setPen(Qt::NoPen);
    const auto count = static_cast<int>(paragraph.ticks.size());
    for (int rowStart = 0; rowStart < count; rowStart += per) {
        const int inRow = std::min(per, count - rowStart);
        const double centre = top + (rowStart / per) * kRowPitch + kRowPitch / 2;
        if (morph >= 1.0) {
            painter.setBrush(line);
            const double length = (inRow - 1) * kTickStep + kTickW;
            painter.drawRoundedRect(QRectF(left, centre - kLineH / 2, length, kLineH), kLineH / 2,
                                    kLineH / 2);
            continue;
        }
        // Ticks widen until they touch and settle at the line's height.
        const double w = kTickW + (kTickStep - kTickW) * morph;
        const QColor mixed(roundChannel(color.red() + (line.red() - color.red()) * morph),
                           roundChannel(color.green() + (line.green() - color.green()) * morph),
                           roundChannel(color.blue() + (line.blue() - color.blue()) * morph));
        painter.setBrush(mixed);
        for (int i = 0; i < inRow; ++i) {
            const double height = paragraph.ticks[static_cast<std::size_t>(rowStart + i)];
            const double born = paragraph.born[static_cast<std::size_t>(rowStart + i)];
            const double grow = ease_out((at - born) / kTickGrowS);
            const double tickH = (kTickMin + (kTickMax - kTickMin) * height) * grow;
            const double h = tickH + (kLineH - tickH) * morph;
            const double x = left + i * kTickStep;
            painter.drawRoundedRect(QRectF(x, centre - h / 2, w, h), 1.0, 1.0);
        }
    }
}

void WalkthroughPanel::paintNote(QPainter& painter, const Paragraph& note, const QRectF& rect,
                                 double at, double morph) {
    // A note for the agent: a purple row that says it stays out of the document.
    painter.save();
    painter.setPen(QPen(token("purple", 102), 1.0));
    painter.setBrush(token("purple", 36));
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    const QColor ink = token("purple-text");
    const QRectF head(rect.left() + kNotePadX, rect.top() + kNotePadTop,
                      rect.width() - 2 * kNotePadX, kNoteHeadH);
    paintWalkthroughIcon(painter, "note", QPointF(head.left() + 6, head.center().y()), 12, ink);
    QFont small(font());
    // 10 px, so all of it fits the column: "not in the document" is the point.
    small.setPixelSize(10);
    small.setWeight(QFont::DemiBold);
    painter.setFont(small);
    painter.setPen(ink);
    const QString text = QStringLiteral(u"Note for the agent · ") + clockText(noteLength(note)) +
                         QStringLiteral(u" · not in the document");
    painter.drawText(head.adjusted(18, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(text, Qt::ElideRight,
                                                      static_cast<int>(head.width() - 18)));
    paintTicks(painter, note, head.left(), head.bottom() + 3, head.width(), at, morph, ink);
    painter.restore();
}

void WalkthroughPanel::paintCaret(QPainter& painter, double x, double centre) {
    // Paused, it rests dim rather than blinking.
    const double blink =
        paused_ ? 0.0
                : 0.5 + 0.5 * std::cos(static_cast<double>(elapsedMs()) / 1000.0 * kTau / 1.1);
    painter.setPen(Qt::NoPen);
    painter.setBrush(token("accent", static_cast<int>(90 + 165 * blink)));
    painter.drawRoundedRect(QRectF(x, centre - 7, 2, 14), 1, 1);
}

void WalkthroughPanel::paintSlot(QPainter& painter, const std::vector<const Paragraph*>& items,
                                 const QRectF& rect, double at) {
    // The section being spoken: an open, dashed slot the next screenshot closes.
    painter.save();
    QPen pen(token("accent-tint-border"), 1.5, Qt::CustomDashLine);
    pen.setDashPattern({3.0, 2.5});
    painter.setPen(pen);
    painter.setBrush(token("accent-tint"));
    painter.drawRoundedRect(rect.adjusted(0.75, 0.75, -0.75, -0.75), 10, 10);

    const double top = rect.top() + kSlotPadTop;
    const QRectF badge(rect.left() + kSlotPadX, top, kSlotBadge, kSlotBadge);
    painter.setPen(Qt::NoPen);
    painter.setBrush(token("accent"));
    painter.drawRoundedRect(badge, 6, 6);
    QFont small(font());
    small.setPixelSize(11);
    small.setWeight(QFont::Bold);
    painter.setFont(small);
    painter.setPen(token("on-accent"));
    painter.drawText(badge, Qt::AlignCenter, QString::number(column_.shots() + 1));

    small.setWeight(QFont::Normal);
    painter.setFont(small);
    painter.setPen(token("accent-soft"));
    const double elapsed = std::max(0.0, recorded() - sectionStarted());
    const QRectF timeRect(rect.right() - kSlotPadX - kSlotTimeW, top, kSlotTimeW, kSlotBadge);
    painter.drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, clockText(elapsed));

    const double left = badge.right() + 6;
    const double width = slotTicksWidth(rect.width());
    const int per = per_row(width);
    // A single row of ticks sits level with the badge.
    double y = top + (kSlotBadge - kRowPitch) / 2;
    QPointF caret(left + 2, y + kRowPitch / 2);
    for (const Paragraph* item : items) {
        const int taken = std::max(1, rows(*item, width));
        const QColor ink = token(item->note ? "purple-text" : "text");
        paintTicks(painter, *item, left, y, width, at, 0.0, ink);
        const auto count = static_cast<int>(item->ticks.size());
        int row = count / per;
        int col = count % per;
        if (count != 0 && col == 0) {
            // A full row: the caret waits at its end, not on a row of its own.
            row -= 1;
            col = per;
        }
        caret = QPointF(left + col * kTickStep + 2, y + row * kRowPitch + kRowPitch / 2);
        y += taken * kRowPitch;
    }
    paintCaret(painter, caret.x(), caret.y());

    QFont hint(font());
    hint.setPixelSize(12);
    painter.setFont(hint);
    const bool holding = column_.holding;
    painter.setPen(token(holding ? "purple-text" : paused_ ? "text-secondary" : "accent-soft"));
    const double hintTop = rect.bottom() - kSlotPadBottom - kSlotHintH;
    const std::string_view words = holding     ? kNoteHint
                                   : paused_   ? kPausedHint
                                   : autoMode_ ? kAutoSlotHint
                                               : kSlotHint;
    painter.drawText(
        QRectF(rect.left() + kSlotPadX, hintTop, rect.width() - 2 * kSlotPadX, kSlotHintH),
        Qt::AlignLeft | Qt::AlignVCenter,
        QString::fromUtf8(words.data(), static_cast<qsizetype>(words.size())));
    painter.restore();
}

void WalkthroughPanel::paintShot(QPainter& painter, const ShotItem& shot, double left, double top,
                                 double width, double at) {
    const auto [w, h] = shotSize(shot, width);
    const double t = (at - shot.born) / kShotInS;
    const double scale = 0.82 + 0.18 * ease_out_back(t);
    const QRectF rect(left + (width - w) / 2, top, w, h);
    shotRects_.emplace_back(shot.number, rect);
    if (flight_ != nullptr && flight_->number() == shot.number) {
        // Reserve its destination while the real picture travels above the desktop; there is
        // only one visible copy of the screenshot.
        painter.save();
        painter.setPen(QPen(token("accent", 90), 1.0, Qt::DashLine));
        painter.setBrush(token("accent", 18));
        painter.drawRoundedRect(rect, 6, 6);
        painter.restore();
        return;
    }
    const QPointF centre = rect.center();
    const bool hovered = hoverShot_ == shot.number;
    painter.save();
    painter.setOpacity(painter.opacity() * ease_out(t * 1.6));
    painter.translate(centre.x() + 24 * (1 - ease_out(t)), centre.y());
    painter.scale(scale, scale);
    painter.translate(-centre.x(), -centre.y());
    painter.setPen(Qt::NoPen);
    for (const int i : {6, 4, 2}) {
        painter.setBrush(QColor(0, 0, 0, 16));
        const double half = i / 2.0;
        painter.drawRoundedRect(rect.adjusted(-half, -half + 3, half, half + 3), 6 + i, 6 + i);
    }
    if (hovered) {
        painter.setPen(QPen(token("accent"), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(-3, -3, 3, 3), 8, 8);
    }
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(rect, 6, 6);
    painter.setClipPath(clip, Qt::IntersectClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(rect, thumbnailOf(shot));
    painter.restore();
    paintBadge(painter, shot, rect);
    if (hovered && state_ == State::Recording) {
        paintShotTools(painter, shot.number);
    }
    painter.restore();
}

void WalkthroughPanel::paintBadge(QPainter& painter, const ShotItem& shot, const QRectF& rect) {
    // The screenshot's place on the board and when it was kept: "2 · 0:35".
    QFont small(font());
    small.setPixelSize(11);
    small.setWeight(QFont::DemiBold);
    painter.setFont(small);
    const QString text = QString::number(column_.place(shot.number)) + QStringLiteral(u" · ") +
                         clockText(shot.kept_at_s);
    const int width = painter.fontMetrics().horizontalAdvance(text) + 16;
    const QRectF badge(rect.left() + 8, rect.top() + 8, width, kBadgeH);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kOnImage);
    painter.drawRoundedRect(badge, kBadgeH / 2, kBadgeH / 2);
    painter.setPen(kOnImageText);
    painter.drawText(badge, Qt::AlignCenter, text);
}

void WalkthroughPanel::paintShotTools(QPainter& painter, int number) {
    for (const Control& tool : shotTools(number)) {
        const QString name = tool.key.section(u':', 0, 0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(hover_ == tool.key ? kOnImageHover : kOnImage);
        painter.drawRoundedRect(tool.rect, 8, 8);
        const bool remove = name == QStringLiteral("remove");
        QColor ink = remove ? kOnImageDanger : kOnImageText;
        if (!tool.enabled) {
            ink.setAlphaF(0.3f);
        }
        const std::string glyph = remove ? std::string("x") : name.toStdString();
        paintWalkthroughIcon(painter, glyph, tool.rect.center(), 14, ink);
    }
}

void WalkthroughPanel::paintFooter(QPainter& painter) {
    // Once it records no more, the buttons fade, all but Cancel, which stays while it is
    // written.
    const double fade = state_ != State::Recording ? 1.0 - ease_out(since() / kFadeS) : 1.0;
    QFont label(font());
    label.setPixelSize(13);
    label.setWeight(QFont::DemiBold);
    painter.setFont(label);
    for (const Control& control : footer()) {
        double alpha = 0.0;
        if (state_ == State::Recording) {
            alpha = control.enabled ? 1.0 : 0.4;
        } else {
            alpha = control.enabled ? 1.0 : fade;
        }
        if (alpha <= 0.0) {
            continue;
        }
        painter.save();
        painter.setOpacity(painter.opacity() * alpha);
        paintButton(painter, control.key, control.rect);
        painter.restore();
    }
}

void WalkthroughPanel::paintButton(QPainter& painter, const QString& key, const QRectF& rect) {
    const bool hover = hover_ == key;
    const bool pressed = pressed_ == key && hover;
    if (key == QStringLiteral("snap") || key == QStringLiteral("snap-menu")) {
        // One rounded button in two parts: Snap, then the menu's chevron.
        const QRectF whole = footerControl(QStringLiteral("snap"))
                                 .rect.united(footerControl(QStringLiteral("snap-menu")).rect);
        std::string_view fill;
        if (key == QStringLiteral("snap")) {
            fill = pressed ? "accent-pressed" : hover ? "accent-hover" : "accent";
        } else {
            fill = (pressed || hover || menu_ != nullptr) ? "accent-hover" : "accent-pressed";
        }
        painter.save();
        painter.setClipRect(rect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(fill));
        painter.drawRoundedRect(whole, 10, 10);
        painter.restore();
        const QColor ink = token("on-accent");
        if (key == QStringLiteral("snap-menu")) {
            painter.setPen(QPen(token("on-accent", 77), 1.0));
            painter.drawLine(QPointF(rect.left() + 0.5, rect.top()),
                             QPointF(rect.left() + 0.5, rect.bottom()));
            paintWalkthroughIcon(painter, "up", rect.center(), 12, ink);
        } else if (countdown_ != 0) {
            paintLabel(painter, rect, "camera", QStringLiteral("Snap in %1").arg(countdown_), ink);
        } else {
            paintLabel(painter, rect, "camera",
                       autoMode_ ? QStringLiteral("Draw") : QStringLiteral("Snap"), ink);
        }
        return;
    }
    const std::string_view fill = pressed ? "border-hover"
                                  : hover ? "surface-active"
                                          : "surface-hover";
    painter.setPen(QPen(token("border-strong"), 1.0));
    painter.setBrush(token(fill));
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    if (key == QStringLiteral("note")) {
        if (column_.holding) {
            painter.setPen(QPen(token("purple"), 1.0));
            painter.setBrush(token("purple", 70));
            painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        }
        paintLabel(painter, rect, "note", QStringLiteral("Hold for note"), token("purple-text"));
    } else if (key == QStringLiteral("undo")) {
        paintWalkthroughIcon(painter, "undo", rect.center(), 16, token("text"));
    } else if (key == QStringLiteral("pause")) {
        if (paused_) {
            // Paused: the button is the way back, so it stands out.
            painter.setPen(QPen(token("accent-tint-border"), 1.0));
            painter.setBrush(token("accent-tint-strong"));
            painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
            paintWalkthroughIcon(painter, "play", rect.center(), 15, token("accent-soft"));
        } else {
            paintWalkthroughIcon(painter, "pause", rect.center(), 15, token("text"));
        }
    } else if (key == QStringLiteral("cancel")) {
        const QColor ink = token("danger-text-soft");
        if (rect.width() > kFootIconW * 2.5) {
            paintLabel(painter, rect, "x", QStringLiteral("Cancel"), ink);
        } else {
            paintWalkthroughIcon(painter, "x", rect.center(), 16, ink);
        }
    } else if (key == QStringLiteral("done")) {
        paintLabel(painter, rect, "", QStringLiteral("Done"), token("success"));
    }
}

void WalkthroughPanel::paintLabel(QPainter& painter, const QRectF& rect, std::string_view icon,
                                  const QString& text, const QColor& color) {
    // text centred in rect, after icon when there is one.
    const QFontMetrics metrics = painter.fontMetrics();
    const double iconW = icon.empty() ? 0.0 : 16.0 + 6.0;
    const double width = iconW + metrics.horizontalAdvance(text);
    const double x = rect.center().x() - width / 2;
    if (!icon.empty()) {
        paintWalkthroughIcon(painter, icon, QPointF(x + 8, rect.center().y()), 16, color);
    }
    painter.setPen(color);
    painter.drawText(QRectF(x + iconW, rect.top(), width - iconW + 2, rect.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, text);
}

std::unique_ptr<WalkthroughPanel> makePanel(const Clock& clock) {
    return std::make_unique<WalkthroughPanel>(clock);
}

} // namespace fv
