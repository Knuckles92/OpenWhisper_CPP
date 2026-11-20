#include "ui_walkthrough/walkthrough_presenter.h"

#include <utility>

#include "ui_overlay/waveform_overlay.h"
#include "ui_walkthrough/keep_out.h"
#include "ui_walkthrough/walkthrough_panel.h"

namespace fv {

WalkthroughPresenter::WalkthroughPresenter(Parts parts) : parts_(std::move(parts)) {}

WalkthroughPresenter::~WalkthroughPresenter() = default;

void WalkthroughPresenter::setWalkthroughActive(bool active) {
    if (parts_.setTrayActive) {
        parts_.setTrayActive(active);
    }
    if (parts_.setWindowActive) {
        parts_.setWindowActive(active);
    }
    parts_.overlay->setWalkthroughControls(active);
    showPanel(active);
    // Already in the tray: nothing to minimize. Finishing leaves the window where it is.
    if (active && parts_.windowVisible && parts_.windowVisible() && parts_.minimizeTicked &&
        parts_.minimizeTicked() && parts_.minimizeToTray) {
        parts_.minimizeToTray();
    }
}

void WalkthroughPresenter::showPanel(bool recording) {
    WalkthroughPanel* panel = parts_.panel.data();
    if (!recording) {
        if (panel != nullptr) {
            panel->finish();
        }
        return;
    }
    if (panel == nullptr) {
        return;
    }
    panel->begin();
    // After show(): the exclusion is set on the native window. Where it can't be, a screenshot
    // hides the column for a moment (snapKeepOut).
    if (parts_.keepOutOfScreenshots) {
        parts_.keepOutOfScreenshots(panel, true);
    } else {
        setKeepOutOfScreenshots(panel, true);
    }
    // The column shows the recording, so the recording overlay steps aside.
    parts_.overlay->setStandIn([this] { return panelStandsIn(); });
}

bool WalkthroughPresenter::panelStandsIn() const {
    const WalkthroughPanel* panel = parts_.panel.data();
    // During the AI write-up the walkthrough's job is over: a recording or transcription then
    // is a dictation, which the overlay shows.
    return panel != nullptr && panel->showsRecording() && !(parts_.writingUp && parts_.writingUp());
}

void WalkthroughPresenter::dismissPanelForJob() {
    // A dictation made during the write-up is not the walkthrough's: the column stays, with its
    // Cancel, until the write-up ends.
    if (parts_.panel && !(parts_.writingUp && parts_.writingUp())) {
        parts_.panel->dismiss();
    }
}

void WalkthroughPresenter::hear(float level) {
    if (parts_.panel) {
        parts_.panel->hear(level);
    }
}

QList<QPointer<QWidget>> WalkthroughPresenter::snapKeepOut() {
    QList<QPointer<QWidget>> keepOut;
    // Embedded in the main window (Wayland), the overlay must not hide that window.
    if (parts_.overlay->isWindow()) {
        keepOut.append(parts_.overlay);
    }
    if (parts_.panel && parts_.panel->isVisible()) {
        parts_.panel->endCaptureAnimation();
        keepOut.append(parts_.panel.data());
    }
    if (parts_.drawingWindows) {
        for (QWidget* window : parts_.drawingWindows()) {
            keepOut.append(window);
        }
    }
    return keepOut;
}

std::vector<ui_snap::Parkable> WalkthroughPresenter::snapFloatAbove() const {
    std::vector<ui_snap::Parkable> floating;
    WaveformOverlay* overlay = parts_.overlay;
    if (overlay->isWindow()) {
        QPointer<WaveformOverlay> guarded = overlay;
        floating.push_back({[guarded](QRect area) {
                                if (guarded) {
                                    guarded->park(area);
                                }
                            },
                            [guarded] {
                                if (guarded) {
                                    guarded->unpark();
                                }
                            },
                            overlay});
    }
    return floating;
}

ui_snap::QtSnapPort::Hooks WalkthroughPresenter::snapHooks() {
    return {[this] { return snapKeepOut(); }, [this] { return snapFloatAbove(); }, {}};
}

void WalkthroughPresenter::onWindowWalkthrough() {
    if (parts_.onWalkthrough) {
        parts_.onWalkthrough("ui");
    }
}

void WalkthroughPresenter::onOverlaySnap() {
    // "ui" is the window and tray's source (catalog.SOURCES); the overlay is the same user at
    // the same desk.
    if (parts_.onScreenshot) {
        parts_.onScreenshot("ui");
    }
}

void WalkthroughPresenter::onOverlayFinish() {
    if (parts_.onWalkthrough) {
        parts_.onWalkthrough("ui");
    }
}

void WalkthroughPresenter::onTrayWalkthrough() {
    if (parts_.onWalkthrough) {
        parts_.onWalkthrough("tray");
    }
}

void WalkthroughPresenter::onTrayScreenshot() {
    if (parts_.onScreenshot) {
        parts_.onScreenshot("tray");
    }
}

} // namespace fv
