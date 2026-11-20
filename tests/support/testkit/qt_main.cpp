// fv_testkit_qt: the Catch2 main of every widget test executable (fv_test(NO_MAIN ...)). One
// offscreen QApplication with the pins the reference screenshots were made with (a33 map,
// section 5), so a widget renders here as it rendered for the reference.
#include <QApplication>
#include <QStyle>

#include <catch2/catch_session.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    Catch::Session session;
    if (const int status = session.applyCommandLine(argc, argv); status != 0) {
        return status;
    }
    // Listing runs no test, so it needs no QApplication; the build lists tests to register them
    // with CTest, outside CTest's environment.
    const Catch::ConfigData& config = session.configData();
    if (config.listTests || config.listTags || config.listReporters || config.listListeners ||
        config.showHelp) {
        return session.run();
    }

    // Pinned, not defaulted: the references are offscreen renders, and a widget test must never
    // reach the screen, whatever platform the calling shell names.
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // On Windows the offscreen platform finds no fonts without a font folder and draws boxes
    // for text (6,905 pixels off in the a33 trial): fail rather than compare against that.
    // Elsewhere it finds fonts through fontconfig, and pixels are not compared off Windows.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        std::fprintf(stderr, "fv_testkit_qt: QT_QPA_FONTDIR is not set. Run the widget tests "
                             "through ctest, which points it at the system font folder.\n");
        return 2;
    }
#endif
    qputenv("QT_FONT_DPI", "96");
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "1");
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        qputenv("QT_SCALE_FACTOR", "1");
    }
    qunsetenv("QT_SCREEN_SCALE_FACTORS");
    qunsetenv("QT_STYLE_OVERRIDE");

    QApplication app(argc, argv);
    // After construction: windows11 set before QApplication exists crashes the offscreen
    // platform (Qt 6.11.0).
    if (QApplication::setStyle(QStringLiteral("windows11")) == nullptr) {
#ifdef Q_OS_WIN
        std::fprintf(stderr, "fv_testkit_qt: the windows11 style plugin is missing.\n");
        return 2;
#else
        // Only Windows has it; elsewhere screenshot tests skip their pixels (a different kit),
        // and the widget tests run on Qt's default style.
        std::fprintf(stderr, "fv_testkit_qt: no windows11 style here; using %s.\n",
                     qPrintable(QApplication::style()->name()));
#endif
    }
    return session.run();
}
