// TemporaryClipboard on the real Windows clipboard. [interactive]: it replaces the user's
// clipboard for about half a second (and puts it back), so only windows-interactive runs it.

#include <QProcess>
#include <QString>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("staged text reaches Windows readers and the previous clipboard comes back after 250 ms",
          "[interactive][temporary_clipboard]") {
#ifndef FV_CLIPBOARD_PLATFORM_HELPER
    SKIP("Qt's windows platform and the Win32 clipboard exist only on Windows");
#else
    QProcess helper;
    helper.setProcessChannelMode(QProcess::MergedChannels);
    helper.start(QStringLiteral(FV_CLIPBOARD_PLATFORM_HELPER), {});
    REQUIRE(helper.waitForFinished(30'000));
    INFO(helper.readAll().toStdString());
    CHECK(helper.exitStatus() == QProcess::NormalExit);
    CHECK(helper.exitCode() == 0);
#endif
}
