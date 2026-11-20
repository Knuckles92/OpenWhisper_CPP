#include "ui_walkthrough/panel_port.h"

#include <QImage>

#include "appkit/qt_bridge.h"
#include "ui_walkthrough/walkthrough_panel.h"

namespace fv {
WalkthroughPanelPort::WalkthroughPanelPort(WalkthroughPanel& panel) : panel_(panel) {
    connect(&panel, &WalkthroughPanel::snapRequested, this, [this] { snap_requested.emit(); });
    connect(&panel, &WalkthroughPanel::snapModeChosen, this,
            [this](const QString& mode) { snap_mode_chosen.emit(qt::toUtf8(mode)); });
    connect(&panel, &WalkthroughPanel::noteHeld, this, [this](bool on) { note_held.emit(on); });
    connect(&panel, &WalkthroughPanel::pauseRequested, this,
            [this](bool on) { pause_requested.emit(on); });
    connect(&panel, &WalkthroughPanel::undoRequested, this, [this] { undo_requested.emit(); });
    connect(&panel, &WalkthroughPanel::cancelRequested, this, [this] { cancel_requested.emit(); });
    connect(&panel, &WalkthroughPanel::finishRequested, this, [this] { finish_requested.emit(); });
    connect(&panel, &WalkthroughPanel::moveRequested, this,
            [this](int n, int step) { move_requested.emit(n, step); });
    connect(&panel, &WalkthroughPanel::removeRequested, this,
            [this](int n) { remove_requested.emit(n); });
}
walkthrough::PanelState WalkthroughPanelPort::state() const {
    switch (panel_.state()) {
    case WalkthroughPanel::State::Recording:
        return walkthrough::PanelState::Recording;
    case WalkthroughPanel::State::Writing:
        return walkthrough::PanelState::Writing;
    case WalkthroughPanel::State::Done:
        return walkthrough::PanelState::Done;
    case WalkthroughPanel::State::Hidden:
        return walkthrough::PanelState::Hidden;
    }
    return walkthrough::PanelState::Hidden;
}
bool WalkthroughPanelPort::paused() const {
    return panel_.paused();
}
void WalkthroughPanelPort::begin() {
    panel_.begin();
}
void WalkthroughPanelPort::hear(double value) {
    panel_.hear(value);
}
void WalkthroughPanelPort::add_shot(int n, walkthrough::ShotImagePtr image, double at,
                                    std::optional<walkthrough::Rect> source) {
    const auto bytes = image->png_bytes();
    QImage picture = QImage::fromData(bytes.data(), static_cast<int>(bytes.size()), "PNG");
    picture.setDevicePixelRatio(image->device_pixel_ratio());
    std::optional<QRect> rect;
    if (source)
        rect = QRect(source->x, source->y, source->width, source->height);
    panel_.addShot(n, picture, at, rect);
}
void WalkthroughPanelPort::end_capture_animation() {
    panel_.endCaptureAnimation();
}
void WalkthroughPanelPort::remove_shot(int n) {
    panel_.removeShot(n);
}
void WalkthroughPanelPort::move_shot(int n, int step) {
    panel_.moveShot(n, step);
}
bool WalkthroughPanelPort::shows_recording() const {
    return panel_.showsRecording();
}
void WalkthroughPanelPort::set_snap_mode(const std::string& mode,
                                         std::optional<std::string> shortcut) {
    if (shortcut)
        panel_.setSnapMode(qt::toQString(mode), qt::toQString(*shortcut));
    else
        panel_.setSnapMode(qt::toQString(mode));
}
void WalkthroughPanelPort::set_auto_mode(bool on) {
    panel_.setAutoMode(on);
}
void WalkthroughPanelPort::set_countdown(int seconds) {
    panel_.setCountdown(seconds);
}
void WalkthroughPanelPort::set_paused(bool on) {
    panel_.setPaused(on);
}
void WalkthroughPanelPort::set_notes_on(bool on) {
    panel_.setNotesOn(on);
}
std::vector<std::string> WalkthroughPanelPort::snap_modes() const {
    std::vector<std::string> modes;
    for (const auto& mode : panel_.snapModes())
        modes.push_back(qt::toUtf8(mode));
    return modes;
}
void WalkthroughPanelPort::finish() {
    panel_.finish();
}
void WalkthroughPanelPort::complete(int shots, double duration) {
    panel_.complete(shots, duration);
}
void WalkthroughPanelPort::dismiss() {
    panel_.dismiss();
}
} // namespace fv
