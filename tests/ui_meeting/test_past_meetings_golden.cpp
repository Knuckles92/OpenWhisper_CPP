// Golden vectors of ui_qt.widgets.past_meetings_panel (tools/parity/cases/<module>.py): the
// panel's list rules and a card's texts, as OpenWhisper's widgets show them for the same rows.
// Every started_at that is shown is naive, so the clock's zone does not matter.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QLabel>
#include <QVariant>

#include "appkit/qt_bridge.h"
#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "meeting_state/time_utils.h"
#include "ui_meeting/past_meeting_item.h"
#include "ui_meeting/past_meetings_model.h"

namespace golden = fv::golden;
namespace pm = fv::past_meetings;
using fv::Json;

namespace {

constexpr const char* kModule = "ui_qt.widgets.past_meetings_panel";

std::vector<Json> rowsOf(const Json& array) {
    std::vector<Json> rows;
    for (const Json& row : array) {
        rows.push_back(row);
    }
    return rows;
}

Json idsOf(const std::vector<Json>& meetings) {
    Json ids = Json::array();
    for (const Json& meeting : meetings) {
        ids.push_back(meeting["id"]);
    }
    return ids;
}

// The `from_repository` summary the case's fake repository gives a row that has none.
Json summaryFor(const std::string& id) {
    return Json{{"has_audio", true}, {"has_transcript", false}, {"from_repository", id}};
}

// What the card's pill reads as a tone: the pillTone property, null when never set.
Json property(const QWidget* widget, const char* name) {
    const QVariant value = widget->property(name);
    return value.isValid() ? Json(fv::qt::toUtf8(value.toString())) : Json(nullptr);
}

} // namespace

TEST_CASE("past meetings _format_duration matches OpenWhisper", "[golden][past_meetings_panel]") {
    golden::for_each(kModule, "_format_duration", [](const golden::Case& c) {
        CHECK(fv::meeting_state::format_meeting_duration(c.arg<Json>(0)) ==
              c.result<std::string>());
    });
}

TEST_CASE("past meetings load_meetings matches OpenWhisper", "[golden][past_meetings_panel]") {
    golden::for_each(kModule, "load_meetings", [](const golden::Case& c) {
        const std::vector<Json> meetings =
            pm::historical_meetings(rowsOf(c.arg<Json>(0)), summaryFor);
        const Json expected = c.result<Json>();
        CHECK(Json(meetings) == expected[0]);
        // The repository filters nothing: a provider's rows are filtered by the panel.
        CHECK(expected[1] == false);
    });
}

TEST_CASE("past meetings filter matches OpenWhisper", "[golden][past_meetings_panel]") {
    const fv::FakeClock clock;
    golden::for_each(kModule, "filter_matches", [&clock](const golden::Case& c) {
        const std::vector<Json> kept =
            pm::filter_meetings(rowsOf(c.arg<Json>(0)), c.arg<std::string>(1), clock);
        CHECK(idsOf(kept) == c.result<Json>());
    });
}

TEST_CASE("past meetings list view matches OpenWhisper", "[golden][past_meetings_panel]") {
    const fv::FakeClock clock;
    golden::for_each(kModule, "list_view", [&clock](const golden::Case& c) {
        const pm::ListView view = pm::list_view(rowsOf(c.arg<Json>(0)), c.arg<bool>(2),
                                                c.arg<bool>(3), c.arg<std::string>(1), clock);
        Json placeholders = Json::array();
        if (!view.placeholder.empty()) {
            placeholders.push_back(view.placeholder);
        }
        if (!view.footer.empty()) {
            placeholders.push_back(view.footer);
        }
        const Json actual = {{"count", view.count},
                             {"count_visible", view.count_visible},
                             {"placeholders", placeholders},
                             {"cards", idsOf(view.cards)}};
        CHECK(actual == c.result<Json>());
    });
}

TEST_CASE("past meetings card matches OpenWhisper", "[golden][past_meetings_panel][widget]") {
    const fv::FakeClock clock;
    golden::for_each(kModule, "card", [&clock](const golden::Case& c) {
        const fv::PastMeetingItem item(c.arg<Json>(0), clock);
        const Json actual = {
            {"id", fv::qt::toUtf8(item.meetingId())},
            {"title", fv::qt::toUtf8(item.titleLabel()->text())},
            {"date", fv::qt::toUtf8(item.dateLabel()->text())},
            {"preview", fv::qt::toUtf8(item.previewLabel()->text())},
            {"preview_visible", !item.previewLabel()->isHidden()},
            {"note", fv::qt::toUtf8(item.contentLabel()->text())},
            {"note_visible", !item.contentLabel()->isHidden()},
            {"detail", fv::qt::toUtf8(item.detailLabel()->text())},
            {"pill", fv::qt::toUtf8(item.insightsPill()->text())},
            {"pill_visible", !item.insightsPill()->isHidden()},
            {"pill_tone", property(item.insightsPill(), "pillTone")},
            {"location", fv::qt::toUtf8(item.locationPill()->text())},
            {"location_visible", !item.locationPill()->isHidden()},
            {"location_tip", fv::qt::toUtf8(item.locationPill()->toolTip())},
            {"stored_on", fv::qt::toUtf8(item.storedOn())},
        };
        CHECK(actual == c.result<Json>());
    });
}

TEST_CASE("past meetings has_transcript matches OpenWhisper", "[golden][past_meetings_panel]") {
    golden::for_each(kModule, "has_transcript", [](const golden::Case& c) {
        CHECK(pm::has_transcript(c.arg<Json>(0)) == c.result<bool>());
    });
}

TEST_CASE("past meetings meeting_has_audio matches OpenWhisper", "[golden][past_meetings_panel]") {
    golden::for_each(kModule, "meeting_has_audio", [](const golden::Case& c) {
        CHECK(pm::has_audio(c.arg<Json>(0)) == c.result<bool>());
    });
}

TEST_CASE("past meetings meeting_location matches OpenWhisper", "[golden][past_meetings_panel]") {
    golden::for_each(kModule, "meeting_location", [](const golden::Case& c) {
        const auto [pill, tip] = pm::meeting_location(c.arg<Json>(0));
        CHECK(Json::array({pill, tip}) == c.result<Json>());
    });
}
