#include "ui_snap/snap_port.h"

#include <stdexcept>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QTimer>

#include "ui_snap/export.h"
#include "ui_snap/pointed.h"
#include "ui_snap/qimage_shot.h"
#include "ui_snap/support.h"

namespace fv::ui_snap {
namespace {

using walkthrough::Rect;

SnapTake takeOf(walkthrough::SnapTake take) {
    switch (take) {
    case walkthrough::SnapTake::Screen:
        return SnapTake::Screen;
    case walkthrough::SnapTake::Window:
        return SnapTake::Window;
    case walkthrough::SnapTake::Draw:
        return SnapTake::Draw;
    case walkthrough::SnapTake::Region:
        break;
    }
    return SnapTake::Region;
}

Rect rectOf(const QRect& rect) {
    return Rect{rect.x(), rect.y(), rect.width(), rect.height()};
}

// A started snap: the session while it runs, and where its image came from once it is kept.
// The session deletes itself after its outcome, so the rectangle is copied then.
class QtSnapHandle final : public walkthrough::SnapHandle {
public:
    explicit QtSnapHandle(SnapSession* session) : session_(session) {}

    bool active() const override { return session_ && session_->isActive(); }

    void keep() override {
        if (active()) {
            session_->keep();
        }
    }

    void drop() override {
        if (active()) {
            session_->drop();
        }
    }

    std::optional<Rect> capture_rect() const override {
        if (session_) {
            if (const std::optional<QRect> rect = session_->captureRect()) {
                return rectOf(*rect);
            }
            return std::nullopt;
        }
        return settled_;
    }

    // The outcome came: remember the kept area before the session goes.
    void settle() {
        if (session_) {
            if (const std::optional<QRect> rect = session_->captureRect()) {
                settled_ = rectOf(*rect);
            }
        }
    }

private:
    QPointer<SnapSession> session_;
    std::optional<Rect> settled_;
};

} // namespace

walkthrough::ScreenRef screenRef(QScreen* screen) {
    if (screen == nullptr) {
        return {};
    }
    return {screen->name().toStdString(),
            static_cast<int>(QGuiApplication::screens().indexOf(screen))};
}

QScreen* screenFor(const walkthrough::ScreenRef& ref) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    const QString name = QString::fromStdString(ref.name);
    if (ref.index >= 0 && ref.index < screens.size() && screens[ref.index]->name() == name) {
        return screens[ref.index];
    }
    for (QScreen* screen : screens) {
        if (screen->name() == name) {
            return screen;
        }
    }
    return nullptr;
}

walkthrough::MarksPtr marksPtr(std::vector<Annotation> marks) {
    if (marks.empty()) {
        return nullptr;
    }
    return std::make_shared<const std::vector<Annotation>>(std::move(marks));
}

std::vector<Annotation> marksOf(const walkthrough::MarksPtr& marks) {
    if (!marks) {
        return {};
    }
    return *static_cast<const std::vector<Annotation>*>(marks.get());
}

QtSnapPort::QtSnapPort(SettingsStore* settings, Hooks hooks)
    : settings_(settings), hooks_(std::move(hooks)) {}

QtSnapPort::~QtSnapPort() = default;

std::pair<bool, std::string> QtSnapPort::support() const {
    const auto [supported, reason] = snapSupport();
    return {supported, reason.toStdString()};
}

void QtSnapPort::request_access() {
    requestAccess();
}

std::shared_ptr<walkthrough::SnapHandle>
QtSnapPort::start(walkthrough::SnapRequest request, Kept kept, Dropped dropped, Failed failed) {
    if (hooks_.beforeStart) {
        hooks_.beforeStart();
    }
    SnapOptions options;
    if (hooks_.keepOut) {
        options.keepOut = hooks_.keepOut();
    }
    if (hooks_.floatAbove) {
        options.floatAbove = hooks_.floatAbove();
    }
    options.hint = QString::fromStdString(request.hint);
    options.take = takeOf(request.take);
    options.front = request.front;
    if (request.click_at) {
        options.clickAt = QPoint(request.click_at->first, request.click_at->second);
    }
    if (request.screen) {
        options.screen = screenFor(*request.screen);
    }
    options.settings = settings_;
    if (request.screen && options.screen == nullptr) {
        // The monitor asked for has gone. startSnap would read a null screen as "the pointer's",
        // so it fails here as session.py's _start does for a screen no longer in screens(): the
        // desktop's own reason first, else "no screen", reported from the event loop.
        const auto [supported, reason] = snapSupport();
        const std::string why =
            supported ? QString::fromUtf16(kNoScreen).toStdString() : reason.toStdString();
        QTimer::singleShot(0, QCoreApplication::instance(), [failed = std::move(failed), why] {
            if (failed) {
                failed(why);
            }
        });
        return std::make_shared<QtSnapHandle>(nullptr);
    }
    SnapSession* session = startSnap(std::move(options));
    auto handle = std::make_shared<QtSnapHandle>(session);
    QObject::connect(session, &SnapSession::kept, session,
                     [handle, kept = std::move(kept)](const QImage& image) {
                         handle->settle();
                         if (kept) {
                             kept(std::make_shared<QImageShot>(image));
                         }
                     });
    QObject::connect(session, &SnapSession::dropped, session,
                     [handle, dropped = std::move(dropped)] {
                         handle->settle();
                         if (dropped) {
                             dropped();
                         }
                     });
    QObject::connect(session, &SnapSession::failed, session,
                     [handle, failed = std::move(failed)](const QString& reason) {
                         handle->settle();
                         if (failed) {
                             failed(reason.toStdString());
                         }
                     });
    return handle;
}

walkthrough::ShotImagePtr QtSnapPort::render(walkthrough::ShotImagePtr frozen,
                                             std::optional<walkthrough::Rect> crop,
                                             walkthrough::MarksPtr marks) {
    const auto* shot = dynamic_cast<const QImageShot*>(frozen.get());
    if (shot == nullptr) {
        throw std::invalid_argument("Only a screenshot taken by Snap can be rendered");
    }
    const QImage& image = shot->image();
    const QRect selection =
        crop ? QRect(crop->x, crop->y, crop->width, crop->height) : logicalRect(image);
    const std::vector<Annotation> items = marksOf(marks);
    return std::make_shared<QImageShot>(renderKept(image, selection, items));
}

bool QtSnapPort::window_mode_supported() const {
    return pointedSupported();
}

} // namespace fv::ui_snap
