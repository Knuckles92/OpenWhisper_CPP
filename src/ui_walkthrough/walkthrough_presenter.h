#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QList>
#include <QPointer>
#include <QWidget>

#include "ui_snap/session.h"
#include "ui_snap/snap_port.h"

// The UI controller's walkthrough glue (ui_qt/ui_controller.py:1067-1218): what a walkthrough
// starting or ending shows (tray, window, overlay controls, the column), the column standing in
// for the recording overlay, what a snap keeps out of the shot and parks above it, and the
// window, tray and overlay buttons that reach the runtime as the user. The feature builds it over
// the app's windows; tests build it over offscreen widgets and plain functions. Affinity: GUI
// thread.
namespace fv {

class WaveformOverlay;
class WalkthroughPanel;

class WalkthroughPresenter final {
public:
    struct Parts {
        // The recording overlay; required, and must outlive the presenter.
        WaveformOverlay* overlay = nullptr;
        // The column that writes the walkthrough; nullptr where there is none (Wayland).
        QPointer<WalkthroughPanel> panel;
        // Each may be empty: nothing is done or asked.
        std::function<void(bool)> setTrayActive;
        std::function<void(bool)> setWindowActive;
        std::function<bool()> windowVisible;
        std::function<void()> minimizeToTray;
        // walkthrough_minimize_to_tray, read when a walkthrough starts.
        std::function<bool()> minimizeTicked;
        // Whether a finished walkthrough's AI write-up is running (TranscriptionRuntime).
        std::function<bool()> writingUp;
        // ui_snap's capture exclusion; empty uses setKeepOutOfScreenshots.
        std::function<bool(QWidget*, bool)> keepOutOfScreenshots;
        // The live drawing's windows, while one is open.
        std::function<QList<QWidget*>()> drawingWindows;
        // The runtime's toggle and snap, with catalog.SOURCES' source ("ui", "tray").
        std::function<void(const std::string&)> onWalkthrough;
        std::function<void(const std::string&)> onScreenshot;
    };

    explicit WalkthroughPresenter(Parts parts);
    ~WalkthroughPresenter();
    WalkthroughPresenter(const WalkthroughPresenter&) = delete;
    WalkthroughPresenter& operator=(const WalkthroughPresenter&) = delete;

    // set_walkthrough_active: the tray and the window finish it, the overlay offers Snap and
    // Done, the column opens or finishes writing, and a start minimizes a shown window to the
    // tray when ticked.
    void setWalkthroughActive(bool active);
    // _show_walkthrough_panel: begin (kept out of screenshots once shown, standing in for the
    // overlay) or finish.
    void showPanel(bool recording);
    // Whether the column shows what the overlay would: the walkthrough's own job, not a
    // dictation made during the write-up.
    bool panelStandsIn() const;
    // A job was cancelled or failed: close the column, unless a write-up holds it.
    void dismissPanelForJob();
    // The column's sound meter: the loudest of levels.
    void hear(float level);

    // start_snap's extras. keepOut: the overlay when it is its own window, the column while
    // shown (its flying screenshot landed first), the live drawing's windows. floatAbove: the
    // overlay when it is its own window, parked above the snap.
    QList<QPointer<QWidget>> snapKeepOut();
    std::vector<ui_snap::Parkable> snapFloatAbove() const;
    ui_snap::QtSnapPort::Hooks snapHooks();

    // The buttons: the window's and the overlay's are the user's ("ui"), as are the tray's
    // ("tray").
    void onWindowWalkthrough();
    void onOverlaySnap();
    void onOverlayFinish();
    void onTrayWalkthrough();
    void onTrayScreenshot();

    WalkthroughPanel* panel() const { return parts_.panel.data(); }

private:
    Parts parts_;
};

} // namespace fv
