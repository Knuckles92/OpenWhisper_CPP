#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QPointer>
#include <QWidget>

#include "ui_snap/live.h"
#include "walkthrough/runtime_ports.h"

// The walkthrough runtime's live drawing port over LiveAnnotations: every call forwarded, every
// signal re-emitted with screens as ScreenRef and marks as MarksPtr (ui_snap/snap_port.h).
// Affinity: GUI thread.
namespace fv::ui_snap {

class QtLiveDrawingPort final : public walkthrough::LiveDrawingPort {
public:
    // Adopts live: close() closes it, and destroying the port closes it if still open.
    explicit QtLiveDrawingPort(LiveAnnotations* live);
    // A new LiveAnnotations for one walkthrough (RuntimeServices' live factory): panel is the
    // column, markShortcuts the shortcuts as shown ("keep_marks", "clear").
    QtLiveDrawingPort(QWidget* panel, const std::map<std::string, std::string>& markShortcuts,
                      LiveAnnotations::Options options = {});
    ~QtLiveDrawingPort() override;
    QtLiveDrawingPort(const QtLiveDrawingPort&) = delete;
    QtLiveDrawingPort& operator=(const QtLiveDrawingPort&) = delete;

    void arm() override;
    void disarm() override;
    void set_enabled(bool enabled) override;
    void set_busy(bool busy) override;
    void set_capture_pending(bool pending, bool captured = false,
                             std::optional<walkthrough::ScreenRef> screen = std::nullopt) override;
    void set_capture_countdown(double remaining, double total = 0.0) override;
    std::optional<walkthrough::CaptureRegion> capture_region() const override;
    walkthrough::MarksPtr pinned_at(std::pair<int, int> position) const override;
    walkthrough::MarksPtr pinned_on(std::optional<walkthrough::ScreenRef> screen) const override;
    void handle_mark_shortcut(const std::string& action) override;
    bool interacting() const override;
    bool armed() const override;
    void close() override;

    // The live drawing, until it is closed; its windows() go to the Snap port's keepOut.
    LiveAnnotations* live() const { return live_; }

private:
    void connectSignals();

    QPointer<LiveAnnotations> live_;
    QList<QMetaObject::Connection> connections_;
};

} // namespace fv::ui_snap
