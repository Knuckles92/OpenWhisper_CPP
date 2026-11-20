// The Scratchpad window against the references OpenWhisper rendered for it
// (tools/parity/builders/a08.py scratchpad_*, tests/reference/scratchpad/<variant>/
// <theme>-windows11-dpr1-s<scale>.{png,json}): the widget tree first, then the pixels, with the
// application styled as OpenWhisper styles it. The window is built, never presented, so it keeps
// the platform's own place and shows no caret, as the builders leave it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <QApplication>
#include <QPlainTextEdit>
#include <QTimer>

#include "scratchpad_support.h"
#include "screenshot/screenshot.h"
#include "ui_record/scratchpad.h"

namespace {

using fv::Json;

// a08.py SCRATCHPAD_TEXT.
const QString kNotes = QStringLiteral("Groceries\n- milk\n- bread");

// The tree keys the mapping below compares itself.
const std::vector<std::string> kTreeKeys{
    "class", "qtClass",   "objectName", "geometry",  "visible",  "hasFocus",   "properties",
    "text",  "isChecked", "isDefault",  "alignment", "wordWrap", "styleSheet", "font"};

// The port's tree as the reference's schema has it. A widget hidden in the reference (the editor's
// scroll bars and their containers) has no geometry worth comparing: Qt sizes it when it first
// shows, and the minors differ in the placeholder it holds meanwhile. OpenWhisper's header is
// the private class _Header, which has no Qt class of its own, so the classes are not compared.
void normalize(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
    }
    const auto& want = expected["children"];
    auto& have = node["children"];
    for (std::size_t i = 0; i < std::min(want.size(), have.size()); ++i) {
        normalize(have[i], want[i]);
    }
}

void check(const std::string& widget, const std::string& theme, int scale, bool cleared) {
    fvtest::PadRig rig(fv::Json::object(), QString::fromStdString(theme), scale);
    fv::ScratchpadWindow window(rig.environment(), rig.copier());
    window.editor()->setPlainText(kNotes);
    if (cleared) {
        window.clearText();
    }
    // The autosave would write while the window waits for its grab.
    window.autosaveTimer()->stop();
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(widget, variant);
    window.setFixedSize(pins["logical_size"][0].get<int>(), pins["logical_size"][1].get<int>());
    window.show();
    QApplication::processEvents();
    Json actual = fv::screenshot::dump_tree(window);
    normalize(actual, pins["tree"]);
    for (const auto& difference : fv::screenshot::compare_trees(pins["tree"], actual, {"class"})) {
        FAIL_CHECK(widget << "/" << variant << " tree: " << difference);
    }

    fv::screenshot::Options options;
    options.ignore_keys = kTreeKeys;
    fv::screenshot::expect_matches(window, widget, variant, options);
}

} // namespace

TEST_CASE("reference screenshot scratchpad/notes dark", "[screenshot]") {
    check("scratchpad/notes", "dark", 100, false);
}

TEST_CASE("reference screenshot scratchpad/notes light", "[screenshot]") {
    check("scratchpad/notes", "light", 100, false);
}

TEST_CASE("reference screenshot scratchpad/notes dark 130 percent", "[screenshot]") {
    check("scratchpad/notes", "dark", 130, false);
}

TEST_CASE("reference screenshot scratchpad/undo dark", "[screenshot]") {
    check("scratchpad/undo", "dark", 100, true);
}
