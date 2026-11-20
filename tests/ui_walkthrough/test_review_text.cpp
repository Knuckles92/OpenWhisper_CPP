// The storyboard's words against OpenWhisper's (tools/parity/cases/
// ui_qt.widgets.walkthrough_review.py): _day, _time, _shots, _started, _written_at, the header,
// the write-up version picker, the copy notices and the hints.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "pyport/datetime.h"
#include "review_support.h"
#include "testkit/testkit.h"
#include "ui_walkthrough/review_text.h"
#include "walkthrough/document.h"

namespace fvtest {
namespace {

namespace text = fv::walkthrough_review;
using namespace std::chrono;

constexpr const char* kModule = "ui_qt.widgets.walkthrough_review";

// isoformat text as the wall time it names, its offset ignored as Python's formatting does.
local_seconds wall(const std::string& iso) {
    const auto when = fv::py::fromisoformat(iso);
    return local_days{year{when.year} / when.month / when.day} + hours{when.hour} +
           minutes{when.minute} + seconds{when.second};
}

year_month_day date(const std::string& iso) {
    const auto when = fv::py::date_fromisoformat(iso);
    return year{when.year} / when.month / when.day;
}

std::string isoWall(local_seconds when) {
    return std::format("{:%Y-%m-%dT%H:%M:%S}", when);
}

} // namespace

TEST_CASE("the storyboard's day and time read as OpenWhisper's", "[golden][walkthrough_review]") {
    fv::golden::for_each(kModule, "_day", [](const fv::golden::Case& c) {
        CHECK(text::day(wall(c.arg<std::string>(0)), date(c.arg<std::string>(1))) ==
              c.result<std::string>());
    });
    fv::golden::for_each(kModule, "_time", [](const fv::golden::Case& c) {
        CHECK(text::time(wall(c.arg<std::string>(0))) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "_shots", [](const fv::golden::Case& c) {
        CHECK(text::shots(c.arg<int>(0)) == c.result<std::string>());
    });
}

TEST_CASE("a walkthrough's start comes from its document, its draft or its folder's name",
          "[golden][walkthrough_review]") {
    fv::golden::for_each(kModule, "_started", [](const fv::golden::Case& c) {
        fv::testkit::TempDataRoot scratch;
        const auto folder = scratch.path() / fv::path_from_utf8(c.arg<std::string>(0));
        std::filesystem::create_directories(folder);
        if (const auto draft = c.arg<std::optional<std::string>>(2)) {
            writeFile(folder / "draft.json", *draft);
        }
        std::optional<fv::walkthrough::Walkthrough> document;
        if (const auto started = c.arg<std::optional<std::string>>(1)) {
            document =
                fv::walkthrough::Walkthrough{zoned_seconds(current_zone(), wall(*started)), 0, {}};
        }
        const auto when = text::started(folder, document ? &*document : nullptr);
        const auto expected = c.result<std::optional<std::string>>();
        CHECK(expected.has_value() == when.has_value());
        if (expected && when) {
            CHECK(isoWall(*when) == *expected);
        }
    });
}

TEST_CASE("a write-up's time reads as the version menu says it", "[golden][walkthrough_review]") {
    fv::golden::for_each(kModule, "_written_at", [](const fv::golden::Case& c) {
        const auto modified = c.arg<std::optional<std::string>>(0);
        const auto when = modified ? std::optional(wall(*modified)) : std::nullopt;
        CHECK(text::written_at(when, date(c.arg<std::string>(1))) == c.result<std::string>());
    });
}

TEST_CASE("the header says what OpenWhisper's says", "[golden][walkthrough_review]") {
    fv::golden::for_each(kModule, "header", [](const fv::golden::Case& c) {
        // As the case does: a folder in a new temporary folder, never made, so no draft.json
        // there can say when it started.
        fv::testkit::TempDataRoot scratch;
        text::HeaderInput input;
        if (const auto name = c.arg<std::optional<std::string>>(0)) {
            input.folder = scratch.path() / fv::path_from_utf8(*name);
        }
        const auto shots = c.arg<std::optional<std::vector<bool>>>(2);
        if (input.folder) {
            if (shots) {
                if (const auto started = c.arg<std::optional<std::string>>(1)) {
                    input.started = wall(*started);
                }
            } else {
                // No document: the folder's name says when, as _started reads it.
                input.started = text::started(*input.folder, nullptr);
            }
        }
        input.sections = shots;
        input.duration_s = c.arg<double>(3);
        input.draft = c.arg<bool>(4);
        input.written = c.arg<bool>(5);
        input.spoken = c.arg<bool>(6);
        const auto shown = text::header(input);
        const fv::Json expected = c.result<fv::Json>();
        CHECK(shown.title == expected.at("title").get<std::string>());
        CHECK(shown.title_shown == expected.at("title_shown").get<bool>());
        CHECK(shown.meta == expected.at("meta").get<std::string>());
        CHECK(shown.pill == expected.at("pill").get<std::string>());
        CHECK(shown.pill_tone == expected.at("pill_tone").get<std::string>());
        CHECK(shown.pill_shown == expected.at("pill_shown").get<bool>());
        CHECK(shown.version_bar_shown == expected.at("version_bar_shown").get<bool>());
        CHECK(shown.version_bar_index == expected.at("version_bar_index").get<int>());
        CHECK(shown.retry == expected.at("retry").get<std::string>());
        CHECK(shown.style_menu == expected.at("style_menu").get<std::string>());
        CHECK(shown.prompt == expected.at("prompt").get<std::string>());
        CHECK(shown.notice == expected.at("notice").get<std::string>());
    });
}

TEST_CASE("the version picker and its menu read as OpenWhisper's", "[golden][walkthrough_review]") {
    fv::golden::for_each(kModule, "versions", [](const fv::golden::Case& c) {
        const auto earlier = c.arg<std::vector<int>>(0);
        const fv::Json written = c.arg<fv::Json>(4);
        const auto shown = text::versions(
            earlier, c.arg<std::optional<int>>(1), c.arg<bool>(2), c.arg<bool>(3),
            [&written](std::optional<int> version) {
                return written.at(version ? std::to_string(*version) : std::string("None"))
                    .get<std::string>();
            });
        const fv::Json expected = c.result<fv::Json>();
        CHECK(shown.shown == expected.at("shown").get<bool>());
        CHECK(shown.text == expected.at("text").get<std::string>());
        fv::Json rows = fv::Json::array();
        for (const auto& row : shown.rows) {
            rows.push_back(fv::Json::array({row.label, row.checked}));
        }
        CHECK(rows == expected.at("rows"));
    });
}

TEST_CASE("the copy notices and the hints are OpenWhisper's, word for word",
          "[golden][walkthrough_review]") {
    fv::golden::for_each(kModule, "copied", [](const fv::golden::Case& c) {
        const auto notice = text::copied(c.arg<std::string>(0));
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_FALSE(notice.has_value());
            return;
        }
        REQUIRE(notice.has_value());
        CHECK(std::string(*notice) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "constant", [](const fv::golden::Case& c) {
        const auto name = c.arg<std::string>(0);
        if (name == "VERSIONS") {
            fv::Json versions = fv::Json::array();
            for (const auto& choice : text::VERSIONS) {
                versions.push_back(
                    fv::Json::array({std::string(choice.title), std::string(choice.tip)}));
            }
            CHECK(versions == c.result<fv::Json>());
            return;
        }
        std::string_view value;
        if (name == "SPOKEN_HINT") {
            value = text::SPOKEN_HINT;
        } else if (name == "WRITE_UP_HINT") {
            value = text::WRITE_UP_HINT;
        } else if (name == "SAVED") {
            value = text::SAVED;
        } else {
            FAIL("no C++ constant for " << name);
        }
        CHECK(std::string(value) == c.result<std::string>());
    });
}

} // namespace fvtest
