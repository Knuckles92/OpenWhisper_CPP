#include "ui_shell/self_test.h"

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QPixmap>
#include <QStyle>
#include <QStyleFactory>

#include "base/identity.h"
#include "ui_theme/app_icon.h"
#include "ui_theme/tooltip_style.h"

namespace fv {
namespace {

constexpr char kOffscreen[] = "offscreen";
const QString kCheckSvg = QStringLiteral(":/ui_theme/assets/check.svg");
const QString kThemeSheet = QStringLiteral(":/ui_theme/theme.qss");

} // namespace

std::vector<std::string> selfTestGuiProblems() {
    std::vector<std::string> problems;
    if (QGuiApplication::platformName() != QLatin1String(kOffscreen)) {
        problems.emplace_back("Qt did not start on the offscreen platform plugin.");
    }
    if (!QImageReader::supportedImageFormats().contains("svg")) {
        problems.emplace_back("Qt could not load the bundled SVG image plugin.");
    }
    // A QIcon of an .svg file draws through the SVG icon engine plugin; without it the pixmap
    // is empty.
    if (QIcon(kCheckSvg).pixmap(16, 16).isNull()) {
        problems.emplace_back("Qt could not draw the bundled check.svg icon.");
    }
    QFile sheet(kThemeSheet);
    if (!sheet.open(QIODevice::ReadOnly) || sheet.size() == 0) {
        problems.emplace_back("Missing bundled asset: ui_theme/theme.qss");
    }
    const std::unique_ptr<QStyle> style(QStyleFactory::create(QLatin1String(ui::kBaseStyle)));
    if (style == nullptr) {
        problems.emplace_back("Qt has no windows11 style plugin.");
    }
    if (ui::appIcon().isNull()) {
        problems.emplace_back("The application icon could not be drawn.");
    }
    return problems;
}

int runSelfTestGui(int argc, char** argv) {
    // Before any QApplication: the platform plugin is chosen at construction, and a check must
    // never put a window on the screen.
    qputenv("QT_QPA_PLATFORM", kOffscreen);
    try {
        std::unique_ptr<QApplication> owned;
        if (QApplication::instance() == nullptr) {
            owned = std::make_unique<QApplication>(argc, argv);
        }
        const std::vector<std::string> problems = selfTestGuiProblems();
        for (const std::string& problem : problems) {
            std::fprintf(stderr, "%s\n", problem.c_str());
        }
        if (!problems.empty()) {
            return 1;
        }
        std::printf("%s package self-test passed\n", std::string(identity().product).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

} // namespace fv
