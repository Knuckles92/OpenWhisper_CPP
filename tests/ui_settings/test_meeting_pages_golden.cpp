// The After the meeting and Dashboard pages against OpenWhisper's MeetingAfterSection and rail
// values (tests/golden/ui_qt.dialogs.settings_meeting_after.json, written by
// tools/parity/cases/ui_qt.dialogs.settings_meeting_after.py): each vector builds the page in a
// Settings window over the case's saved settings, acts as the case does, and compares the
// switches, what is enabled and shown, the question asked, and what was saved.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app_pages_support.h"
#include "golden/golden.h"
#include "ui_meeting/after_end_page.h"
#include "ui_meeting/dashboard_page.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fvtest::AppPagesRig;
namespace dest = fv::destinations;
namespace golden = fv::golden;

namespace {

constexpr const char* kModule = "ui_qt.dialogs.settings_meeting_after";

// Every key the page saves.
const std::vector<std::string> kPageKeys{
    "meeting_end_report",    "meeting_report_ribbon",  "meeting_report_brief",
    "meeting_report_signal", "meeting_insight_review", "meeting_insight_review_consent"};

QAbstractButton* viewCheck(fv::AfterEndPage& page, const std::string& view) {
    if (view == "ribbon") {
        return page.ribbonTile()->checkbox();
    }
    if (view == "brief") {
        return page.briefTile()->checkbox();
    }
    return page.signalTile()->checkbox();
}

// Builds the page over `settings`, runs `act`, and compares with the vector's result.
void replay(const Json& settings, const std::function<void(fv::AfterEndPage&)>& act,
            const Json& expected, bool answer = false) {
    AppPagesRig rig(settings,
                    [](AppPagesRig& r) { fv::registerMeetingSettingsPages(r.context()); });
    auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
    Json asked = Json::array();
    page.setConfirm([&](QWidget*, const QString& title, const QString& text) {
        asked.push_back(Json::array({fv::qt::toUtf8(title), fv::qt::toUtf8(text)}));
        return answer;
    });
    act(page);

    CHECK(page.reportTile()->checkbox()->isChecked() == expected["report"].get<bool>());
    CHECK(page.ribbonTile()->checkbox()->isChecked() == expected["ribbon"].get<bool>());
    CHECK(page.briefTile()->checkbox()->isChecked() == expected["brief"].get<bool>());
    CHECK(page.signalTile()->checkbox()->isChecked() == expected["signal"].get<bool>());
    CHECK(page.reviewTile()->checkbox()->isChecked() == expected["review"].get<bool>());
    const bool viewsEnabled = page.ribbonTile()->isEnabled() && page.briefTile()->isEnabled() &&
                              page.signalTile()->isEnabled();
    CHECK(viewsEnabled == expected["views_enabled"].get<bool>());
    CHECK((page.viewsTitle()->isEnabled() && page.viewsInfo()->isEnabled()) ==
          expected["title_enabled"].get<bool>());
    CHECK(!page.viewsHint()->isHidden() == expected["hint_visible"].get<bool>());
    if (expected.contains("asked")) {
        CHECK(asked == expected["asked"]);
    }

    // What was saved: each key the vector saved holds its last value; the others are as the
    // case left them.
    std::set<std::string> written;
    Json last = Json::object();
    for (const Json& pair : expected["persisted"]) {
        last[pair[0].get<std::string>()] = pair[1];
    }
    for (const Json& update : expected["persisted_many"]) {
        for (const auto& [key, value] : update.items()) {
            last[key] = value;
        }
    }
    for (const auto& [key, value] : last.items()) {
        INFO(key);
        CHECK(rig.saved(key) == value);
        written.insert(key);
    }
    for (const std::string& key : kPageKeys) {
        if (written.contains(key)) {
            continue;
        }
        INFO(key << " was not saved");
        CHECK(rig.has(key) == settings.contains(key));
        if (settings.contains(key)) {
            CHECK(rig.saved(key) == settings[key]);
        }
    }
}

} // namespace

TEST_CASE("golden settings_meeting_after load", "[golden][widget]") {
    golden::for_each(kModule, "MeetingAfterSection.load", [](const golden::Case& c) {
        replay(c.arg<Json>(0), [](fv::AfterEndPage&) {}, c.result<Json>());
    });
}

TEST_CASE("golden settings_meeting_after report view toggled", "[golden][widget]") {
    golden::for_each(
        kModule, "MeetingAfterSection._on_report_view_toggled", [](const golden::Case& c) {
            const std::string view = c.arg<std::string>(1);
            const bool checked = c.arg<bool>(2);
            replay(
                c.arg<Json>(0),
                [&](fv::AfterEndPage& page) { viewCheck(page, view)->setChecked(checked); },
                c.result<Json>());
        });
}

TEST_CASE("golden settings_meeting_after end report toggled", "[golden][widget]") {
    golden::for_each(
        kModule, "MeetingAfterSection._on_end_report_toggled", [](const golden::Case& c) {
            const bool checked = c.arg<bool>(1);
            replay(
                c.arg<Json>(0),
                [&](fv::AfterEndPage& page) { page.reportTile()->checkbox()->setChecked(checked); },
                c.result<Json>());
        });
}

TEST_CASE("golden settings_meeting_after review toggled", "[golden][widget]") {
    golden::for_each(kModule, "MeetingAfterSection._on_review_toggled", [](const golden::Case& c) {
        replay(
            c.arg<Json>(0),
            [](fv::AfterEndPage& page) { page.reviewTile()->checkbox()->setChecked(true); },
            c.result<Json>(), c.arg<bool>(1));
    });
}

TEST_CASE("golden settings_meeting_after rail values", "[golden]") {
    golden::for_each(kModule, "_meeting_after_rail_value", [](const golden::Case& c) {
        CHECK(fv::after_end::rail_value(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each(kModule, "_meeting_dashboard_rail_value", [](const golden::Case& c) {
        CHECK(fv::dashboard::rail_value(c.arg<Json>(0)) == c.result<std::string>());
    });
}
