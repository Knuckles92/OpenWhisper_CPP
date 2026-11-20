// Host Mode's connected-computer rows lead with the mirrored link glyph, the "serving" chip
// (ui_qt/widgets/host_dashboard.py _ClientRow; audit A18-39).

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <QHBoxLayout>

#include "base/json.h"
#include "ui_remote/host_dashboard.h"
#include "ui_remote/host_text.h"
#include "ui_widgets/remote_link_glyph.h"

using fv::Json;

TEST_CASE("Host Mode's connected rows start with the serving chip", "[host_mode][a18_rows]") {
    const auto* zone = std::chrono::locate_zone("UTC");
    const double now = 1'800'000'000.0;
    fv::host_dashboard::ClientRow row;
    REQUIRE(row.glyph != nullptr);
    CHECK(row.glyph->mirrored());
    CHECK(row.glyph->toolTip() ==
          QStringLiteral("This computer on the left, the paired computer on the right"));
    // First in the row, before the name and the state.
    auto* layout = qobject_cast<QHBoxLayout*>(row.layout());
    REQUIRE(layout != nullptr);
    CHECK(layout->itemAt(0)->widget() == row.glyph);
    CHECK(layout->itemAt(0)->alignment() == Qt::AlignVCenter);

    fv::host_text::GroupedClient client;
    client.device_id = "d1";
    client.name = "laptop";
    client.address = "100.64.0.9";
    client.since = now - 600;
    row.showClient(client, Json{{"transcriptions", 3}, {"audio_s", 60.0}}, now, *zone);
    CHECK(row.glyph->state() == QStringLiteral("connected"));
    CHECK_FALSE(row.glyph->busy());
    // The first answer draws the wire; nothing is on it yet.
    CHECK(row.glyph->isDrawingWire());
    CHECK(row.glyph->flightKinds().isEmpty());

    // While it transcribes the chip is busy; each transcription served is a reply coming home.
    client.busy = true;
    row.showClient(client, Json{{"transcriptions", 4}, {"audio_s", 70.0}}, now, *zone);
    CHECK(row.glyph->busy());
    CHECK(row.glyph->flightKinds() == QStringList{QStringLiteral("reply")});
    CHECK(row.state->text() == QStringLiteral("Transcribing…"));

    // Nothing new served: no new packet.
    row.showClient(client, Json{{"transcriptions", 4}, {"audio_s", 70.0}}, now, *zone);
    CHECK(row.glyph->flightKinds() == QStringList{QStringLiteral("reply")});
}
