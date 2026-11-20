#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include "ui_snap/annotations.h"
#include "ui_snap/session.h"
#include "walkthrough/runtime_ports.h"

class QScreen;

namespace fv {
class SettingsStore;
}

// The walkthrough runtime's Snap port over startSnap, renderKept and QImageShot. Screens cross
// the port as walkthrough::ScreenRef, marks as walkthrough::MarksPtr holding a
// std::vector<Annotation>, images as QImageShot. Affinity: GUI thread.
namespace fv::ui_snap {

// A screen as the ports name it: its name and its place in QGuiApplication::screens().
walkthrough::ScreenRef screenRef(QScreen* screen);
// The screen ref names: at its index when the name matches there, else the first of that name;
// nullptr when it has gone.
QScreen* screenFor(const walkthrough::ScreenRef& ref);

// marks as the ports carry them; nullptr when there are none.
walkthrough::MarksPtr marksPtr(std::vector<Annotation> marks);
// The marks a MarksPtr made by marksPtr holds; empty for nullptr.
std::vector<Annotation> marksOf(const walkthrough::MarksPtr& marks);

class QtSnapPort final : public walkthrough::SnapPort {
public:
    // What each snap adds, read when it starts (ui_controller.start_snap): our windows that
    // must not appear in the shot (the recording overlay, the column, the live drawing's
    // windows), the windows parked above the overlay meanwhile, and what to do first (the
    // column lands a flying screenshot). Each may be empty.
    struct Hooks {
        std::function<QList<QPointer<QWidget>>()> keepOut;
        std::function<std::vector<Parkable>()> floatAbove;
        std::function<void()> beforeStart;
    };

    // settings is where snap_to_edges and snap_box_infill are read and saved (nullptr: the
    // defaults, nothing saved).
    explicit QtSnapPort(SettingsStore* settings = nullptr, Hooks hooks = {});
    ~QtSnapPort() override;

    std::pair<bool, std::string> support() const override;
    void request_access() override;
    // A request.screen that has gone fails ("There's no screen to capture.") without a session.
    std::shared_ptr<walkthrough::SnapHandle> start(walkthrough::SnapRequest request, Kept kept,
                                                   Dropped dropped, Failed failed) override;
    // frozen must be a QImageShot (std::invalid_argument otherwise).
    walkthrough::ShotImagePtr render(walkthrough::ShotImagePtr frozen,
                                     std::optional<walkthrough::Rect> crop,
                                     walkthrough::MarksPtr marks) override;
    bool window_mode_supported() const override;

private:
    SettingsStore* settings_ = nullptr;
    Hooks hooks_;
};

} // namespace fv::ui_snap
