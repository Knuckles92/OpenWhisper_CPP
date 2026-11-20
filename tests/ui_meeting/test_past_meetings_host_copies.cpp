// Past Meetings marks where each meeting is kept (ui_qt/widgets/past_meetings_panel.py
// _meeting_location and _start_remote_listing's merge): "On <host>" for one only the paired host
// keeps, "Also on <host>" for one kept here whose copy the host also holds, "From <computer>" for
// one this host keeps for a paired computer. The listing marks a host copy with _host_copy_name;
// the panel shows it as Also on (audit A31-31).

#include <catch2/catch_test_macros.hpp>

#include <QLabel>
#include <QString>

#include "past_meetings_support.h"
#include "ui_meeting/past_meeting_item.h"
#include "ui_meeting/past_meetings_panel.h"

using fv::Json;
using fvtest::meetingRow;
using fvtest::PanelFixture;

TEST_CASE("Past Meetings says where each meeting is kept", "[past_meetings_panel][widget]") {
    Json hosted = meetingRow("m_host", "ended", "Kept there");
    hosted["stored_on"] = "Studio PC";
    hosted["started_at"] = "2025-01-04T09:30:00";
    Json both = meetingRow("m_both", "ended", "Kept on both");
    both["started_at"] = "2025-01-03T09:30:00";
    both["_host_copy_name"] = "Studio PC";
    both["_host_copy_fingerprint"] = "AB";
    both["_host_copy_revision"] = "r1";
    Json visitor = meetingRow("m_from", "ended", "Kept for a peer");
    visitor["origin_device_name"] = "Laptop";
    visitor["started_at"] = "2025-01-02T09:30:00";
    Json here = meetingRow("m_here", "ended", "Only here");
    here["started_at"] = "2025-01-01T09:30:00";
    PanelFixture fixture({hosted, both, visitor, here});
    fixture.refresh();

    const auto cards = fixture.panel->cards();
    REQUIRE(cards.size() == 4);
    CHECK(cards[0]->locationPill()->text() == QStringLiteral("On Studio PC"));
    CHECK(cards[0]->locationPill()->toolTip() ==
          QStringLiteral("Kept on Studio PC; opening it downloads a copy"));
    CHECK(cards[1]->locationPill()->text() == QStringLiteral("Also on Studio PC"));
    CHECK(cards[1]->locationPill()->toolTip() == QStringLiteral("Kept here and on Studio PC"));
    CHECK_FALSE(cards[1]->locationPill()->isHidden());
    CHECK(cards[2]->locationPill()->text() == QStringLiteral("From Laptop"));
    CHECK(cards[2]->locationPill()->toolTip() ==
          QStringLiteral("Kept here for Laptop, a paired computer"));
    CHECK(cards[3]->locationPill()->isHidden());

    // Kept here, so it is exported; the host-only one is not.
    const auto offered = fixture.panel->exportableMeetings();
    CHECK(offered.size() == 3);

    // The host no longer holds the copy: the pill goes with the next listing.
    fixture.rows[1].erase("_host_copy_name");
    fixture.refresh();
    REQUIRE(fixture.panel->cards().size() == 4);
    CHECK(fixture.panel->cards()[1]->locationPill()->isHidden());
}
