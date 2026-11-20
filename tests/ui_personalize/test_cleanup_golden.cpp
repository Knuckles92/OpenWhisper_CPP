// The golden vectors of the AI cleanup pages: the Cleanup model's text_summary
// (ui_qt.dialogs.settings_models) and the AI cleanup rail value (ui_qt.dialogs.settings_dialog),
// from tools/parity/cases.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/json.h"
#include "golden/golden.h"
#include "personalize_support.h"
#include "ui_personalize/cleanup_model_section.h"
#include "ui_personalize/cleanup_page.h"

TEST_CASE("golden ui_qt.dialogs.settings_models text_summary", "[golden]") {
    fv::golden::for_each("ui_qt.dialogs.settings_models", "text_summary",
                         [](const fv::golden::Case& c) {
                             const QString summary = fv::CleanupModelSection::textSummary(
                                 c.arg<std::string>(0), c.arg<std::string>(1), c.arg<fv::Json>(2));
                             CHECK(fv::qt::toUtf8(summary) == c.result<std::string>());
                         });
}

TEST_CASE("golden ui_qt.dialogs.settings_dialog cleanup_rail_value", "[golden]") {
    fv::golden::for_each("ui_qt.dialogs.settings_dialog", "cleanup_rail_value",
                         [](const fv::golden::Case& c) {
                             const QString value = fv::CleanupPage::railValue(c.arg<fv::Json>(0));
                             CHECK(fv::qt::toUtf8(value) == c.result<std::string>());
                         });
}
