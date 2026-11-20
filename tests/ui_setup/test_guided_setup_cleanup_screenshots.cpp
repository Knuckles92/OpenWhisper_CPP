// The guided setup window's Clean up step with its provider row against the references
// OpenWhisper rendered for it (tools/parity/builders/a16.py, tests/reference/guided_setup/
// cleanup/): AI cleanup on at Medium through OpenRouter's free model with its key saved, the
// provider row and privacy sentence drawn from providerSummary, 720x690. The widget tree first,
// then the pixels, with the application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QPushButton>

#include "guided_setup_support.h"
#include "screenshot/screenshot.h"
#include "ui_setup/cleanup_provider_row.h"

namespace {

using fv::GuidedSetupDialog;
using fv::Json;

void shot(const std::string& theme, int scale) {
    const char* name = "guided_setup/cleanup";
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fvtest::SetupRig rig(Json{{"transcript_cleanup_enabled", true},
                              {"transcript_cleanup_level", "medium"},
                              {"transcript_cleanup_provider", "openrouter"},
                              {"transcript_cleanup_model", "openrouter/free"}},
                         true);
    // The builder's Settings window lists no microphones.
    rig.pages().backend().set_layout({}, {}, -1);
    // The builder's resolve_credential finds OpenRouter's key.
    rig.ports.providerSummary = [](const Json& settings) {
        return fv::providerSummary(settings, [](const std::string&) { return true; });
    };
    rig.rebuild(false);
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::CleanUp);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(name, variant);
    fv::screenshot::Options options;
    options.focus = setup.nextButton();
    // As test_guided_setup_screenshots.cpp: the tree without geometry and properties first, then
    // whole after the grab, with the hotkey-window property's name mapped (architecture 8.4).
    options.ignore_keys = {"properties", "geometry"};
    fv::screenshot::expect_matches(setup, name, variant, options);
    Json expected = pins.at("tree");
    REQUIRE(expected.at("properties").contains("openwhisperHotkeyWindow"));
    Json properties = Json::object();
    for (const auto& [key, value] : expected.at("properties").items()) {
        properties[key == "openwhisperHotkeyWindow" ? "openwhisperCppHotkeyWindow" : key] = value;
    }
    expected["properties"] = std::move(properties);
    for (const auto& difference :
         fv::screenshot::compare_trees(expected, fv::screenshot::dump_tree(setup))) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
}

} // namespace

TEST_CASE("guided setup clean up with its provider row matches OpenWhisper, dark", "[screenshot]") {
    shot("dark", 100);
}
TEST_CASE("guided setup clean up with its provider row matches OpenWhisper, light",
          "[screenshot]") {
    shot("light", 100);
}
TEST_CASE("guided setup clean up with its provider row matches OpenWhisper, dark at 130 %",
          "[screenshot]") {
    shot("dark", 130);
}
