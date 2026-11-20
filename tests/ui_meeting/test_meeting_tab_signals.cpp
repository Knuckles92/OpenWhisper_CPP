// The Meeting Mode tab's and Past Meetings' controls emit what the presenter forwards
// (meeting_mode_tab.py's pause_requested, resume_requested, end_requested,
// open_dashboard_requested, copy_guest_link_requested and setup_requested).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "base/json.h"
#include "past_meetings_support.h"
#include "tab_support.h"
#include "ui_history/calendar_glyph_button.h"
#include "ui_meeting/finalization_card.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fvtest::TabFixture;

TEST_CASE("the live controls emit pause, resume, end, dashboard and guest link", "[widget]") {
    TabFixture tab;
    std::vector<std::string> emitted;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::pauseRequested,
                     [&] { emitted.push_back("pause"); });
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::resumeRequested,
                     [&] { emitted.push_back("resume"); });
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::endRequested,
                     [&] { emitted.push_back("end"); });
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::openDashboardRequested,
                     [&] { emitted.push_back("dashboard"); });
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::copyGuestLinkRequested,
                     [&] { emitted.push_back("guest"); });
    tab->setMeetingState(
        Json{{"active", true}, {"status", "active"}, {"dashboard_available", true}});
    QApplication::processEvents();
    tab->pauseButton()->click();
    tab->setMeetingState(Json{{"paused", true}, {"status", "paused"}});
    QApplication::processEvents();
    CHECK(tab->pauseButton()->text() == QStringLiteral("Resume"));
    tab->pauseButton()->click();
    tab->setMeetingState(Json{{"paused", false}, {"status", "active"}});
    tab->dashboardButton()->click();
    REQUIRE(tab->guestLinkButton()->isEnabled());
    tab->guestLinkButton()->click();
    tab->endButton()->click();
    CHECK(emitted == std::vector<std::string>{"pause", "resume", "dashboard", "guest", "end"});
}

TEST_CASE("the setup link asks for meeting setup where setup is offered", "[widget]") {
    TabFixture tab;
    REQUIRE(tab->setupLink() != nullptr);
    CHECK(tab->setupLink()->toolTip() ==
          QStringLiteral("Check the microphone, system audio and speech model"));
    int asked = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::setupRequested, [&] { ++asked; });
    tab->setupLink()->click();
    CHECK(asked == 1);
}

TEST_CASE("the Past Meetings header names its menu, calendar and search", "[widget]") {
    fvtest::PanelFixture fixture;
    auto& panel = *fixture.panel;
    REQUIRE(panel.menuButton() != nullptr);
    CHECK(panel.menuButton()->text() == QStringLiteral("☰"));
    CHECK(panel.menuButton()->size() == QSize(28, 28));
    CHECK(panel.calendarButton()->toolTip() ==
          QStringLiteral("See meetings and history on a calendar (Ctrl+Shift+D)"));
    int calendars = 0;
    QObject::connect(&panel, &fv::PastMeetingsPanel::calendarRequested, [&] { ++calendars; });
    panel.calendarButton()->click();
    CHECK(calendars == 1);
    CHECK(panel.searchInput()->placeholderText() == QStringLiteral("Search meetings..."));
    CHECK(panel.searchInput()->isClearButtonEnabled());
    CHECK(fv::PastMeetingsPanel::kSearchDelayMs == 250);
}

TEST_CASE("a result without a title reads Meeting result", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(
        Json{{"active", false},
             {"meeting_id", "m-1"},
             {"finalization", {{"status", "completed"}, {"steps", Json::array()}}}});
    QApplication::processEvents();
    auto* card = tab->finalizationCard();
    REQUIRE(card != nullptr);
    CHECK(card->identityTitle()->text() == QStringLiteral("Meeting result"));
    tab->setMeetingState(Json{{"display_title", "Vendor review"}});
    QApplication::processEvents();
    CHECK(card->identityTitle()->text() == QStringLiteral("Vendor review"));
}
