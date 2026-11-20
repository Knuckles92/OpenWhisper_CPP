// Snap's real Windows plumbing: SetWindowDisplayAffinity on a shown window, grabWindow(0) of the
// real screen, EnumWindows targets and foreground forcing. [interactive]: it shows a window on
// the user's desktop, captures their screen and takes the focus, so only windows-interactive
// runs it, through a helper process on Qt's windows platform.
#include <QProcess>
#include <QString>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("snap's capture exclusion, screen grab, window edges and foreground work on Windows",
          "[interactive][snap_platform]") {
#ifndef FV_UI_SNAP_PLATFORM_HELPER
    SKIP("Qt's windows platform and the Win32 desktop exist only on Windows");
#else
    QProcess helper;
    helper.setProcessChannelMode(QProcess::MergedChannels);
    helper.start(QStringLiteral(FV_UI_SNAP_PLATFORM_HELPER), {});
    REQUIRE(helper.waitForFinished(60'000));
    INFO(helper.readAll().toStdString());
    CHECK(helper.exitStatus() == QProcess::NormalExit);
    CHECK(helper.exitCode() == 0);
#endif
}
