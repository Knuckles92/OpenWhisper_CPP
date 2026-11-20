// The "New in OpenWhisper_CPP" note (ui_qt/whats_new.py): OpenWhisper's tests of
// tests/test_whats_new.py and the golden vectors of ui_qt.whats_new.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include "appcore/whats_new.h"
#include "base/json.h"
#include "engine/version.h"
#include "golden/golden.h"
#include "pyport/datetime.h"

using fv::Json;
using fv::appcore::is_seen;
using fv::appcore::whats_new;

// from tests/test_whats_new.py::test_note_describes_the_current_release_line
TEST_CASE("note describes the current release line", "[test_whats_new]") {
    const std::string_view version = fv::kVersion;
    const auto first_dot = version.find('.');
    const auto second_dot = version.find('.', first_dot + 1);
    const std::string current(version.substr(0, second_dot));
    CHECK(whats_new().release_line == current);
}

// from tests/test_whats_new.py::test_updated_is_a_real_date_not_in_the_future
TEST_CASE("updated is a real date not in the future", "[test_whats_new]") {
    const auto updated = fv::py::date_fromisoformat(whats_new().updated);
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const std::chrono::year_month_day note{std::chrono::year(updated.year),
                                           std::chrono::month(static_cast<unsigned>(updated.month)),
                                           std::chrono::day(static_cast<unsigned>(updated.day))};
    CHECK(note.ok());
    CHECK(std::chrono::sys_days(note) <= std::chrono::sys_days(today));
}

// from tests/test_whats_new.py::test_only_the_current_note_counts_as_seen
TEST_CASE("only the current note counts as seen", "[test_whats_new]") {
    CHECK(is_seen(Json(whats_new().updated)));
    for (const Json& stored : {Json(""), Json("2000-01-01"), Json(true), Json(nullptr)}) {
        CHECK_FALSE(is_seen(stored));
    }
}

TEST_CASE("is_seen replays the golden vectors", "[golden][whats_new]") {
    fv::golden::for_each("ui_qt.whats_new", "is_seen", [](const fv::golden::Case& c) {
        Json stored = c.arg<Json>(0);
        if (stored == "<current>") {
            stored = whats_new().updated;
        }
        CHECK(is_seen(stored) == c.result<bool>());
    });
}

TEST_CASE("the note is the lead's text and fits the tile's limit", "[whats_new]") {
    CHECK(whats_new().text ==
          "OpenWhisper_CPP 0.1 is the C++ rewrite of OpenWhisper: the same dictation, uploads, "
          "history "
          "and settings, with Parakeet running in-process for faster starts and lower memory.");
    CHECK(fv::appcore::kWhatsNewMaxTextLength == 280);
}
