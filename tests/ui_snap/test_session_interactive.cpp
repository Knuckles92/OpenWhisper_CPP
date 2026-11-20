// A real snap over the desktop: grabWindow(0) of the real screens and the overlay shown full
// screen on each. [interactive]: it captures the user's screens and covers them, so only
// windows-interactive runs it, through a helper process on Qt's windows platform.
#include <QProcess>
#include <QString>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("a real snap keeps the screen and covers every screen with an overlay",
          "[interactive][snap_session]") {
#ifndef FV_UI_SNAP_SESSION_HELPER
    SKIP("Qt's windows platform and the Win32 desktop exist only on Windows");
#else
    QProcess helper;
    helper.setProcessChannelMode(QProcess::MergedChannels);
    helper.start(QStringLiteral(FV_UI_SNAP_SESSION_HELPER), {});
    REQUIRE(helper.waitForFinished(60'000));
    INFO(helper.readAll().toStdString());
    CHECK(helper.exitStatus() == QProcess::NormalExit);
    CHECK(helper.exitCode() == 0);
#endif
}
