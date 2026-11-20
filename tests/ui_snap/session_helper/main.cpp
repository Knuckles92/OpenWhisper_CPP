// The other process of test_session_interactive.cpp: real snaps on Qt's windows platform, which
// the offscreen test executable cannot load. It keeps the pointer's screen at once through
// grabWindow(0), then opens the overlay on every screen over the real desktop and keeps it.
// Exit code 0 when every check held; each failure is printed.
#include <cstdio>
#include <optional>

#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QScreen>
#include <QString>
#include <QThread>

#include "ui_snap/capture.h"
#include "ui_snap/overlay.h"
#include "ui_snap/session.h"

namespace {

int failures = 0;

void check(bool held, const char* what) {
    std::printf("%s: %s\n", held ? "ok" : "FAILED", what);
    failures += held ? 0 : 1;
}

// Runs the event loop until done() or the timeout.
template <class Done> bool pumpUntil(Done done, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

struct Outcome {
    std::optional<QImage> kept;
    bool dropped = false;
    std::optional<QString> failed;
    bool decided() const { return kept || dropped || failed; }
};

void watch(fv::ui_snap::SnapSession* session, Outcome& outcome) {
    using fv::ui_snap::SnapSession;
    QObject::connect(session, &SnapSession::kept,
                     [&outcome](const QImage& image) { outcome.kept = image; });
    QObject::connect(session, &SnapSession::dropped, [&outcome] { outcome.dropped = true; });
    QObject::connect(session, &SnapSession::failed,
                     [&outcome](const QString& reason) { outcome.failed = reason; });
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication app(argc, argv);
    using namespace fv::ui_snap;
    QScreen* pointed = screenUnderCursor();

    Outcome whole;
    SnapSession* atOnce = startSnap({.take = SnapTake::Screen});
    watch(atOnce, whole);
    check(!whole.decided(), "nothing is reported inside startSnap");
    check(pumpUntil([&] { return whole.decided(); }, 5000), "a whole-screen snap reports");
    check(whole.kept.has_value(), "the whole screen is kept");
    if (whole.kept) {
        check(whole.kept->deviceIndependentSize().toSize() == pointed->geometry().size(),
              "the kept image covers the pointer's screen");
    }

    Outcome region;
    SnapSession* covering = startSnap({.hint = QStringLiteral("Interactive test")});
    watch(covering, region);
    QCoreApplication::processEvents();
    const QList<SnapOverlay*> overlays = covering->overlays();
    check(overlays.size() == QGuiApplication::screens().size(), "every screen is covered");
    for (SnapOverlay* overlay : overlays) {
        check(overlay->isVisible() && overlay->isFullScreen(), "an overlay covers its screen");
        check(overlay->geometry() == overlay->snapScreen()->geometry(),
              "the overlay is on its own screen");
    }
    covering->keep();
    check(region.kept.has_value(), "keep with no selection keeps a whole screen");
    if (region.kept && covering->captureRect()) {
        check(region.kept->deviceIndependentSize().toSize() == covering->captureRect()->size(),
              "the kept image is the captured rectangle");
    }
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
