// The column's model without any painting (ui_qt/widgets/walkthrough_panel.py:184-318), from
// tests/test_walkthrough_panel.py, and the golden scripts of
// tools/parity/cases/ui_qt.widgets.walkthrough_panel.py. Qt-free: no Qt header is included.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "golden/golden.h"
#include "testkit/testkit.h"
#include "ui_walkthrough/walkthrough_column.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace fvtest {
namespace {

using namespace fv::ui_walkthrough;
using fv::Json;

// About -26 dBFS: speech on a desk microphone; -66 dBFS: under the speech gate.
constexpr double kLoud = 0.05;
constexpr double kRoom = 0.0005;

fv::walkthrough::ShotImagePtr image(int width = 1600, int height = 900) {
    return std::make_shared<const fv::walkthrough::FakeShotImage>(width, height, 0x336699);
}

void speak(WalkthroughColumn& column, double level, double seconds, double now = 0.0) {
    const auto count = static_cast<int>(std::nearbyint(seconds / kTickS));
    for (int i = 0; i < count; ++i) {
        column.hear(level);
        column.advance(kTickS, now);
    }
}

const Paragraph& paragraphAt(const WalkthroughColumn& column, std::size_t index) {
    return std::get<Paragraph>(column.items.at(index));
}

const Paragraph& lastParagraph(const WalkthroughColumn& column) {
    return std::get<Paragraph>(column.items.back());
}

// Ticks per paragraph, "shot N" per screenshot.
Json shape(const WalkthroughColumn& column) {
    Json out = Json::array();
    for (const Item& item : column.items) {
        if (const auto* shot = std::get_if<ShotItem>(&item)) {
            out.push_back("shot " + std::to_string(shot->number));
        } else {
            out.push_back(std::get<Paragraph>(item).ticks.size());
        }
    }
    return out;
}

// ["words", ticks] or ["note", ticks] per paragraph, "shot N" per screenshot.
Json kinds(const WalkthroughColumn& column) {
    Json out = Json::array();
    for (const Item& item : column.items) {
        if (const auto* shot = std::get_if<ShotItem>(&item)) {
            out.push_back("shot " + std::to_string(shot->number));
        } else {
            const auto& paragraph = std::get<Paragraph>(item);
            out.push_back(Json::array({paragraph.note ? "note" : "words", paragraph.ticks.size()}));
        }
    }
    return out;
}

// Words (4 ticks), shot 1, words (8), shot 2, then words being spoken (2).
WalkthroughColumn board() {
    WalkthroughColumn column;
    speak(column, kLoud, 4 * kTickS);
    column.add_shot(1, image(), 0.0, 2.0);
    speak(column, kLoud, 8 * kTickS);
    column.add_shot(2, image(), 0.0, 4.0);
    speak(column, kLoud, 2 * kTickS);
    return column;
}

} // namespace

// --- the column's contents ---

// from tests/test_walkthrough_panel.py::test_speech_writes_ticks_and_the_room_writes_none
TEST_CASE("speech writes ticks and the room writes none", "[test_walkthrough_panel]") {
    WalkthroughColumn column;

    speak(column, kRoom, 1.5);
    CHECK(lastParagraph(column).ticks.empty());

    speak(column, kLoud, 1.5);
    const auto& ticks = lastParagraph(column).ticks;
    CHECK(static_cast<double>(ticks.size()) == Catch::Approx(1.5 / kTickS).margin(1));
    for (const double height : ticks) {
        CHECK(0.0 < height);
        CHECK(height <= 1.0);
    }
    CHECK(column.speech_s == Catch::Approx(1.5).margin(kTickS));
}

// from tests/test_walkthrough_panel.py::test_louder_speech_writes_taller_ticks
TEST_CASE("louder speech writes taller ticks", "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    speak(column, 0.01, 0.3);
    speak(column, 0.1, 0.3);
    const auto& ticks = lastParagraph(column).ticks;
    CHECK(ticks.back() > ticks.front());
}

// from tests/test_walkthrough_panel.py::test_a_screenshot_closes_the_paragraph_and_opens_the_next
TEST_CASE("a screenshot closes the paragraph and opens the next", "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    speak(column, kLoud, 0.5);
    column.add_shot(1, image(), 1.0);

    REQUIRE(column.items.size() == 3);
    CHECK_FALSE(paragraphAt(column, 0).ticks.empty());
    REQUIRE(std::holds_alternative<ShotItem>(column.items[1]));
    CHECK(std::get<ShotItem>(column.items[1]).number == 1);
    CHECK(paragraphAt(column, 2).ticks.empty());
    CHECK(column.shots() == 1);
}

// from tests/test_walkthrough_panel.py::
//     test_screenshots_with_nothing_said_between_them_leave_no_empty_paragraph
TEST_CASE("screenshots with nothing said between them leave no empty paragraph",
          "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    column.add_shot(1, image(), 0.0);
    column.add_shot(2, image(), 0.1);

    REQUIRE(column.items.size() == 3);
    CHECK(std::holds_alternative<ShotItem>(column.items[0]));
    CHECK(std::holds_alternative<ShotItem>(column.items[1]));
    CHECK(std::holds_alternative<Paragraph>(column.items[2]));
}

// --- moving and removing screenshots ---

// from tests/test_walkthrough_panel.py::test_the_board_starts_in_the_order_kept
TEST_CASE("the board starts in the order kept", "[test_walkthrough_panel]") {
    const WalkthroughColumn column = board();
    CHECK(shape(column) == Json::parse(R"([4, "shot 1", 8, "shot 2", 2])"));
    CHECK(column.place(1) == 1);
    CHECK(column.place(2) == 2);
    CHECK(column.place(9) == 0);
}

// from tests/test_walkthrough_panel.py::
//     test_a_moved_screenshot_takes_its_words_and_the_words_being_spoken_stay_last
TEST_CASE("a moved screenshot takes its words and the words being spoken stay last",
          "[test_walkthrough_panel]") {
    WalkthroughColumn column = board();

    CHECK(column.move_shot(2, -1));
    CHECK(shape(column) == Json::parse(R"([8, "shot 2", 4, "shot 1", 2])"));
    CHECK(column.place(2) == 1);
    CHECK_FALSE(column.move_shot(1, 1));
    CHECK_FALSE(column.move_shot(2, -1));
}

// from tests/test_walkthrough_panel.py::test_a_removed_screenshots_words_run_into_the_next_section
TEST_CASE("a removed screenshots words run into the next section", "[test_walkthrough_panel]") {
    WalkthroughColumn column = board();

    CHECK(column.remove_shot(1));
    CHECK(shape(column) == Json::parse(R"([12, "shot 2", 2])"));
    CHECK(column.remove_shot(2));
    // Into the section being spoken, which the voice keeps writing.
    CHECK(shape(column) == Json::parse("[14]"));
    speak(column, kLoud, kTickS);
    CHECK(shape(column) == Json::parse("[15]"));
    CHECK(column.shots() == 0);
}

// --- Hold for note ---

// from tests/test_walkthrough_panel.py::test_words_said_while_held_are_a_note_of_their_own
TEST_CASE("words said while held are a note of their own", "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    speak(column, kLoud, 2 * kTickS);
    column.hold_note(true, 1.0);
    speak(column, kLoud, 3 * kTickS);
    column.hold_note(false, 2.0);
    speak(column, kLoud, kTickS);

    CHECK(kinds(column) == Json::parse(R"([["words", 2], ["note", 3], ["words", 1]])"));
    CHECK(paragraphAt(column, 1).started_s == 1.0);
    CHECK(paragraphAt(column, 1).ended_s == 2.0);
}

// from tests/test_walkthrough_panel.py::test_a_note_held_in_silence_leaves_nothing
TEST_CASE("a note held in silence leaves nothing", "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    column.hold_note(true, 1.0);
    column.hold_note(false, 1.5);

    CHECK(kinds(column) == Json::parse(R"([["words", 0]])"));
}

// from tests/test_walkthrough_panel.py::
//     test_a_screenshot_while_held_closes_the_section_and_the_note_goes_on
TEST_CASE("a screenshot while held closes the section and the note goes on",
          "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    column.hold_note(true, 1.0);
    speak(column, kLoud, 2 * kTickS);
    column.add_shot(1, image(), 0.0, 3.0);
    speak(column, kLoud, kTickS);

    CHECK(kinds(column) == Json::parse(R"([["note", 2], "shot 1", ["note", 1]])"));
    CHECK(paragraphAt(column, 0).ended_s == 3.0);
}

// from tests/test_walkthrough_panel.py::test_notes_never_run_into_the_words_beside_them
TEST_CASE("notes never run into the words beside them", "[test_walkthrough_panel]") {
    WalkthroughColumn column;
    speak(column, kLoud, 2 * kTickS);
    column.add_shot(1, image(), 0.0, 1.0);
    column.hold_note(true, 1.0);
    speak(column, kLoud, kTickS);
    column.hold_note(false, 2.0);

    column.remove_shot(1);

    CHECK(kinds(column) == Json::parse(R"([["words", 2], ["note", 1], ["words", 0]])"));
}

// --- the model stays Qt-free ---

TEST_CASE("the column model includes no Qt header", "[walkthrough_panel]") {
    for (const char* file : {"walkthrough_column.h", "walkthrough_column.cpp"}) {
        const auto path = fv::testkit::source_root() / "src" / "ui_walkthrough" / file;
        std::ifstream in(path);
        REQUIRE(in);
        std::string line;
        while (std::getline(in, line)) {
            INFO(file << ": " << line);
            CHECK(line.find("#include <Q") == std::string::npos);
            CHECK(line.find("#include <q") == std::string::npos);
        }
    }
}

// --- what each control says ---

TEST_CASE("each control explains itself, and Auto Mode's Snap draws", "[walkthrough_panel]") {
    CHECK(tooltip_text("snap", "Whole screen", false, 0) == "Take a screenshot: whole screen");
    CHECK(tooltip_text("snap-menu", "Region", false, 0) == "Choose what Snap takes");
    CHECK(tooltip_text("cancel-writing", "Region", false, 0) ==
          "Cancel: nothing is kept, unless the AI is already writing it up; then it's kept as "
          "spoken");
    CHECK(tooltip_text("remove", "Region", false, 0) ==
          "Remove the screenshot; its words join the next one's");
    CHECK(tooltip_text("snap", "Region", true, 0) ==
          "Draw on the live screen with the pen. Release to save a screenshot.");
    // Counting down, Snap still says what it takes.
    CHECK(tooltip_text("snap", "In 3 seconds", true, 2) == "Take a screenshot: in 3 seconds");
    CHECK(tooltip_text("snap-menu", "Region", true, 0) == "Take a screenshot manually");
    CHECK(tooltip_text("", "Region", true, 0).empty());
    CHECK(tooltip_text("slot", "Region", false, 0).empty());
}

// --- golden scripts ---

namespace {

Json itemJson(const Item& item) {
    if (const auto* shot = std::get_if<ShotItem>(&item)) {
        return {{"kind", "shot"},
                {"number", shot->number},
                {"born", shot->born},
                {"kept_at_s", shot->kept_at_s}};
    }
    const auto& paragraph = std::get<Paragraph>(item);
    return {{"kind", paragraph.note ? "note" : "words"},
            {"ticks", paragraph.ticks},
            {"born", paragraph.born},
            {"started_s", paragraph.started_s},
            {"ended_s", paragraph.ended_s ? Json(*paragraph.ended_s) : Json(nullptr)}};
}

std::string kindOf(const Item& item) {
    if (const auto* shot = std::get_if<ShotItem>(&item)) {
        return "shot " + std::to_string(shot->number);
    }
    return std::get<Paragraph>(item).note ? "note" : "words";
}

Json runScript(const Json& ops) {
    WalkthroughColumn column;
    Json results = Json::array();
    for (const Json& op : ops) {
        const std::string name = op.at(0).get<std::string>();
        if (name == "hear") {
            column.hear(op.at(1).get<double>());
            results.push_back(nullptr);
        } else if (name == "advance") {
            column.advance(op.at(1).get<double>(), op.at(2).get<double>());
            results.push_back(nullptr);
        } else if (name == "speak") {
            speak(column, op.at(1).get<double>(), op.at(2).get<double>(), op.at(3).get<double>());
            results.push_back(nullptr);
        } else if (name == "add") {
            column.add_shot(op.at(1).get<int>(), nullptr, op.at(2).get<double>(),
                            op.at(3).get<double>());
            results.push_back(nullptr);
        } else if (name == "hold") {
            column.hold_note(op.at(1).get<bool>(), op.at(2).get<double>());
            results.push_back(nullptr);
        } else if (name == "move") {
            results.push_back(column.move_shot(op.at(1).get<int>(), op.at(2).get<int>()));
        } else if (name == "remove") {
            results.push_back(column.remove_shot(op.at(1).get<int>()));
        } else if (name == "place") {
            results.push_back(column.place(op.at(1).get<int>()));
        } else {
            FAIL("unknown op " << name);
        }
    }
    Json items = Json::array();
    for (const Item& item : column.items) {
        items.push_back(itemJson(item));
    }
    Json sections = Json::array();
    for (const auto& section : column.sections()) {
        Json names = Json::array();
        for (const Item& item : section) {
            names.push_back(kindOf(item));
        }
        sections.push_back(names);
    }
    return {{"ops", results},
            {"items", items},
            {"sections", sections},
            {"shots", column.shots()},
            {"holding", column.holding},
            {"speech_s", column.speech_s}};
}

} // namespace

TEST_CASE("the column replays OpenWhisper's scripts", "[golden][walkthrough_panel]") {
    fv::golden::for_each(
        "ui_qt.widgets.walkthrough_panel", "_column", [](const fv::golden::Case& c) {
            const Json actual = runScript(c.args().at(0));
            INFO(c.label() << "\n" << actual.dump());
            CHECK(fv::golden::matches(c.result<Json>(), actual, c.float_tolerance()));
        });
}

TEST_CASE("the column eases as OpenWhisper's does", "[golden][walkthrough_panel]") {
    fv::golden::for_each("ui_qt.widgets.walkthrough_panel", "ease_out",
                         [](const fv::golden::Case& c) {
                             CHECK(fv::golden::near(c.result<double>(), ease_out(c.arg<double>(0)),
                                                    c.float_tolerance()));
                         });
    fv::golden::for_each(
        "ui_qt.widgets.walkthrough_panel", "ease_out_back", [](const fv::golden::Case& c) {
            CHECK(fv::golden::near(c.result<double>(), ease_out_back(c.arg<double>(0)),
                                   c.float_tolerance()));
        });
}

TEST_CASE("rows of ticks fit as OpenWhisper's do", "[golden][walkthrough_panel]") {
    fv::golden::for_each(
        "ui_qt.widgets.walkthrough_panel", "_per_row",
        [](const fv::golden::Case& c) { CHECK(per_row(c.arg<double>(0)) == c.result<int>()); });
    fv::golden::for_each("ui_qt.widgets.walkthrough_panel", "_rows_of",
                         [](const fv::golden::Case& c) {
                             Paragraph paragraph;
                             paragraph.ticks.assign(c.arg<std::size_t>(0), 0.5);
                             CHECK(rows(paragraph, c.arg<double>(1)) == c.result<int>());
                         });
}

} // namespace fvtest
