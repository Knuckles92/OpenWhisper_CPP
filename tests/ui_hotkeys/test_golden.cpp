// Replays tests/golden/ui_qt.widgets.hotkey_capture.json's M2.17 vectors: qt_modifier_names, the
// Hotkeys page's row table and the record row's descriptions, as OpenWhisper computes them.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QString>

#include "golden/golden.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/standard_hotkeys.h"

using fv::Json;
namespace golden = fv::golden;

namespace {

Qt::KeyboardModifiers flags(const std::vector<std::string>& names) {
    Qt::KeyboardModifiers out = Qt::NoModifier;
    for (const auto& name : names) {
        if (name == "control") {
            out |= Qt::ControlModifier;
        } else if (name == "meta") {
            out |= Qt::MetaModifier;
        } else if (name == "alt") {
            out |= Qt::AltModifier;
        } else if (name == "shift") {
            out |= Qt::ShiftModifier;
        } else if (name == "keypad") {
            out |= Qt::KeypadModifier;
        } else {
            FAIL("unknown modifier " << name);
        }
    }
    return out;
}

} // namespace

TEST_CASE("golden qt_modifier_names", "[golden]") {
    golden::for_each(
        "ui_qt.widgets.hotkey_capture", "qt_modifier_names", [&](const golden::Case& c) {
            const auto platform = c.arg<std::string>(1);
            const auto names =
                fv::qtModifierNames(flags(c.arg<std::vector<std::string>>(0)), platform);
            INFO(c.args().dump());
            CHECK(std::vector<std::string>(names.begin(), names.end()) ==
                  c.result<std::vector<std::string>>());
        });
}

TEST_CASE("golden record_hotkey_description", "[golden]") {
    golden::for_each(
        "ui_qt.widgets.hotkey_capture", "record_hotkey_description", [&](const golden::Case& c) {
            INFO(c.args().dump());
            CHECK(fv::standard_hotkeys::recordHotkeyDescription(
                      c.arg<std::string>(0), c.arg<bool>(1)) == c.result<std::string>());
        });
}

TEST_CASE("golden hotkey_rows", "[golden]") {
    using Kind = fv::standard_hotkeys::Item::Kind;
    golden::for_each("ui_qt.widgets.hotkey_capture", "hotkey_rows", [&](const golden::Case& c) {
        const Json expected = c.result<Json>();
        const auto items = fv::standard_hotkeys::items();
        REQUIRE(items.size() == expected.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Json& want = expected[i];
            const auto& item = items[i];
            INFO(i << ": " << want.dump());
            if (want.contains("group")) {
                CHECK(item.kind == Kind::Group);
                CHECK(golden::equal_rebranded(want["group"].get<std::string>(), item.title));
            } else if (want.contains("card")) {
                CHECK(item.kind == Kind::RecordModeCard);
            } else {
                CHECK(item.kind == Kind::Row);
                CHECK(item.key == want["key"].get<std::string>());
                CHECK(item.title == want["title"].get<std::string>());
                CHECK(golden::equal_rebranded(want["description"].get<std::string>(),
                                              item.description));
                CHECK(item.required == want["required"].get<bool>());
            }
        }
    });
}
