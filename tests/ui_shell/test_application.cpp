// The Qt application as ui_qt/app.py QtApplication sets it up (map row A01-40): the names, the
// icon, the snappy tooltip proxy over the platform style, and the saved theme and font scale
// before anything paints.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QStyle>

#include "base/identity.h"
#include "base/json.h"
#include "settings/store.h"
#include "testkit/testkit.h"
#include "ui_shell/application.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_theme/tooltip_style.h"

TEST_CASE("the application carries the product's names, icon, tooltip style and saved look",
          "[widget]") {
    fv::testkit::TempDataRoot root;
    fv::SettingsStore settings(root.path() / std::string(fv::identity().settings_file));
    settings.update(fv::Json{{"ui_theme", "light"}, {"ui_font_scale", 130}});
    int argc = 1;
    char program[] = "fiorivoce";
    char* argv[] = {program, nullptr};

    fv::Application application(settings, argc, argv);

    CHECK(QApplication::applicationName() == QStringLiteral("FioriVoce"));
    CHECK(QApplication::applicationDisplayName() == QStringLiteral("FioriVoce"));
    CHECK(QApplication::organizationName() == QStringLiteral("FioriVoce"));
    CHECK_FALSE(QApplication::windowIcon().isNull());
    // Wrapped, so tooltips show at once (SnappyTooltipStyle), over the platform's style; the
    // theme's stylesheet style answers the hint from it.
    CHECK(QApplication::style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay) ==
          fv::ui::SnappyTooltipStyle::kWakeUpDelayMs);
    CHECK_FALSE(fv::ui::currentPalette().isDark());
    CHECK(fv::ui::currentUiFontScalePercent() == 130);

    // Settings' theme choice goes through the same manager: back to dark changes the palette.
    CHECK(application.setTheme(QStringLiteral("dark")));
    CHECK(fv::ui::currentPalette().isDark());
}
