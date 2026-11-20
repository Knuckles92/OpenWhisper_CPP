// The resolved application stylesheet and QPalette against the dumps of OpenWhisper's
// (tools/parity/render_reference.py --dump-stylesheet), made the way QtApplication applies them:
// ThemeManager(theme), applyUiTheme(theme), applyUiFontScale(scale).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMetaEnum>
#include <QPalette>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "base/json.h"
#include "testkit/testkit.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

using namespace fv::ui;

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::filesystem::path referenceDir() {
    return fv::testkit::reference_path() / "app_stylesheet";
}

QByteArray sha1Hex(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
}

// Sheets of other directories are appended after theme.qss; the reference covers theme.qss.
bool hasFeatureSheets() {
    const QStringList dirs =
        QDir(QStringLiteral(":/")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& dir : dirs) {
        if (dir != QStringLiteral("ui_theme") &&
            QFile::exists(QStringLiteral(":/%1/%1.qss").arg(dir))) {
            return true;
        }
    }
    return false;
}

void applyAsOpenWhisper(ThemeManager& manager, const QString& theme, int scale) {
    QApplication& app = *qApp;
    applyUiTheme(theme, app, manager);
    applyUiFontScale(scale, app, manager);
}

} // namespace

TEST_CASE("the application stylesheet matches the reference dump", "[ui_theme][reference]") {
    const QString theme = GENERATE(QStringLiteral("dark"), QStringLiteral("light"));
    const int scale = GENERATE(90, 100, 115, 130);
    const std::string stem = theme.toStdString() + "-s" + std::to_string(scale);
    DYNAMIC_SECTION(stem) {
        ThemeManager manager(theme);
        applyAsOpenWhisper(manager, theme, scale);
        const QByteArray sheet = qApp->styleSheet().toUtf8();
        const std::string expectedSha = readFile(referenceDir() / (stem + ".sha1"));
        const std::string reference = readFile(referenceDir() / (stem + ".qss"));
        const QByteArray compared =
            hasFeatureSheets() ? sheet.left(static_cast<qsizetype>(reference.size())) : sheet;
        const std::string sha = sha1Hex(compared).toStdString();
        if (sha + "\n" != expectedSha) {
            const auto out = fv::testkit::artifact_path() / "app_stylesheet" / (stem + ".qss");
            std::filesystem::create_directories(out.parent_path());
            std::ofstream(out, std::ios::binary).write(sheet.constData(), sheet.size());
            UNSCOPED_INFO("the resolved sheet is in " << out.string());
        }
        CHECK(sha + "\n" == expectedSha);
    }
}

// The acceptance check of the theme package, spelled as its spec does.
TEST_CASE("light at 130 percent gives the light-s130 stylesheet", "[ui_theme][reference]") {
    QApplication& app = *qApp;
    ThemeManager manager;
    applyUiTheme(QStringLiteral("light"), app, manager);
    applyUiFontScale(130, app, manager);
    const std::string reference = readFile(referenceDir() / "light-s130.qss");
    QByteArray sheet = app.styleSheet().toUtf8();
    if (hasFeatureSheets()) {
        sheet = sheet.left(static_cast<qsizetype>(reference.size()));
    }
    CHECK(sha1Hex(sheet).toStdString() + "\n" == readFile(referenceDir() / "light-s130.sha1"));
}

TEST_CASE("the application palette matches the reference dump", "[ui_theme][reference]") {
    const QString theme = GENERATE(QStringLiteral("dark"), QStringLiteral("light"));
    DYNAMIC_SECTION(theme.toStdString()) {
        ThemeManager manager(theme);
        applyAsOpenWhisper(manager, theme, 100);
        const fv::Json expected = fv::Json::parse(
            readFile(referenceDir() / ("palette-" + theme.toStdString() + ".json")));
        const QPalette palette = qApp->palette();
        const QMetaEnum groups = QMetaEnum::fromType<QPalette::ColorGroup>();
        const QMetaEnum roles = QMetaEnum::fromType<QPalette::ColorRole>();
        REQUIRE(expected.size() == 3);
        for (const auto& [groupName, groupRoles] : expected.items()) {
            bool known = false;
            const auto group =
                static_cast<QPalette::ColorGroup>(groups.keyToValue(groupName.c_str(), &known));
            REQUIRE(known);
            for (const auto& [roleName, colour] : groupRoles.items()) {
                const auto role =
                    static_cast<QPalette::ColorRole>(roles.keyToValue(roleName.c_str(), &known));
                REQUIRE(known);
                INFO(groupName << " " << roleName);
                CHECK(palette.color(group, role).name(QColor::HexArgb).toStdString() ==
                      colour.get<std::string>());
            }
        }
    }
}
