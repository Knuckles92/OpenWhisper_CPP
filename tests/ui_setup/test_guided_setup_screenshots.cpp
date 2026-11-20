// The guided setup window against the references OpenWhisper rendered for it
// (tools/parity/builders/a16.py, tests/reference/guided_setup/<page>/): a fresh data root with
// the Windows defaults, no microphones, Parakeet downloaded and no cleanup provider, 720x690.
// The widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QCoreApplication>
#include <QPlainTextEdit>
#include <QPushButton>

#include "guided_setup_support.h"
#include "screenshot/screenshot.h"

namespace {

using fv::GuidedSetupDialog;
using fv::Json;

void shot(const std::string& theme, int scale, const char* name, int page) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fvtest::SetupRig rig(Json::object(), true);
    // The builder's Settings window lists no microphones.
    rig.pages().backend().set_layout({}, {}, -1);
    GuidedSetupDialog& setup = rig.setup();
    if (page != GuidedSetupDialog::Welcome) {
        setup.begin(false);
        if (page != GuidedSetupDialog::Record) {
            setup.showPage(page);
        }
    }
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(name, variant);
    fv::screenshot::Options options;
    options.focus = page == GuidedSetupDialog::Ready ? static_cast<QWidget*>(setup.tryBox())
                                                     : static_cast<QWidget*>(setup.nextButton());
    // render_reference.py dumps the tree after its grab, which also lays out the hidden pages
    // and rows, and the window's hotkey-window property is OpenWhisper_CPP's name for OpenWhisper's
    // (architecture 8.4). The tree is compared first without geometry and properties, then
    // whole, after the grab, with that one name mapped.
    options.ignore_keys = {"properties", "geometry"};
    fv::screenshot::expect_matches(setup, name, variant, options);
    Json expected = pins.at("tree");
    REQUIRE(expected.at("properties").contains("openwhisperHotkeyWindow"));
    // Properties keep their order (an ordered object), so the one name is replaced in place.
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

TEST_CASE("guided setup welcome matches OpenWhisper, dark", "[screenshot]") {
    shot("dark", 100, "guided_setup/welcome", GuidedSetupDialog::Welcome);
}
TEST_CASE("guided setup welcome matches OpenWhisper, light", "[screenshot]") {
    shot("light", 100, "guided_setup/welcome", GuidedSetupDialog::Welcome);
}
TEST_CASE("guided setup welcome matches OpenWhisper, dark at 130 %", "[screenshot]") {
    shot("dark", 130, "guided_setup/welcome", GuidedSetupDialog::Welcome);
}

TEST_CASE("guided setup record matches OpenWhisper, dark", "[screenshot]") {
    shot("dark", 100, "guided_setup/record", GuidedSetupDialog::Record);
}
TEST_CASE("guided setup record matches OpenWhisper, light", "[screenshot]") {
    shot("light", 100, "guided_setup/record", GuidedSetupDialog::Record);
}
TEST_CASE("guided setup record matches OpenWhisper, dark at 130 %", "[screenshot]") {
    shot("dark", 130, "guided_setup/record", GuidedSetupDialog::Record);
}

TEST_CASE("guided setup output matches OpenWhisper, dark", "[screenshot]") {
    shot("dark", 100, "guided_setup/output", GuidedSetupDialog::Output);
}
TEST_CASE("guided setup output matches OpenWhisper, light", "[screenshot]") {
    shot("light", 100, "guided_setup/output", GuidedSetupDialog::Output);
}
TEST_CASE("guided setup output matches OpenWhisper, dark at 130 %", "[screenshot]") {
    shot("dark", 130, "guided_setup/output", GuidedSetupDialog::Output);
}

TEST_CASE("guided setup ready matches OpenWhisper, dark", "[screenshot]") {
    shot("dark", 100, "guided_setup/ready", GuidedSetupDialog::Ready);
}
TEST_CASE("guided setup ready matches OpenWhisper, light", "[screenshot]") {
    shot("light", 100, "guided_setup/ready", GuidedSetupDialog::Ready);
}
TEST_CASE("guided setup ready matches OpenWhisper, dark at 130 %", "[screenshot]") {
    shot("dark", 130, "guided_setup/ready", GuidedSetupDialog::Ready);
}
