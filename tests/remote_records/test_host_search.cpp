// History's search over host-kept dictations (docs/port/audit/a32-checklist-shell.json HI-01):
// the sidebar hands its query to RecordSync::list_remote, and the host answers with
// search_history_entries over the owner's rows, the seven columns this computer's own search
// reads (database.py search_history_entries). Which app an entry was dictated into stays on
// this computer (remote_records/kinds.py: app_name is not in the record), so on the host that
// column is empty for every copy, as in OpenWhisper.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "sync_rig.h"

using namespace fv;
using records_rig::Paired;

namespace {

std::vector<std::string> found(Paired& rig, std::string query) {
    std::vector<std::string> ids;
    for (const auto& item : rig.sync->list_remote("dictation", query)) {
        ids.push_back(item.at("id").get<std::string>());
    }
    return ids;
}

} // namespace

TEST_CASE("a host-kept entry is found by its text, raw text, time, file name, title and owner",
          "[a32][HI-01]") {
    Paired rig;
    rig.sync->set_location("host");
    const auto other = rig.dictate("Lunch order");
    const auto entry = rig.dictate("Ship it Friday", "umm ship IT friday");
    rig.client_db.write([&](sql::Connection& connection) {
        auto statement = connection.prepare(
            "UPDATE transcription_history SET title = 'Quarterly Plan', app_name = 'Notepad' "
            "WHERE id = ?");
        statement.bind(1, entry.id);
        statement.step();
        auto source = connection.prepare(
            "UPDATE transcription_history SET source_name = 'Lunch.wav' WHERE id = ?");
        source.bind(1, other.id);
        source.step();
    });
    CHECK(rig.sync->send_existing() == 2);
    rig.run();
    REQUIRE(rig.on_host(entry.id));
    REQUIRE(rig.on_host(other.id));

    const std::vector<std::string> just{entry.id};
    // The sidebar sends the query lower-cased; the host compares case-insensitively.
    CHECK(found(rig, "ship it") == just);                                 // text
    CHECK(found(rig, "umm") == just);                                     // raw text
    CHECK(found(rig, "quarterly") == just);                               // title
    CHECK(found(rig, "lunch.wav") == std::vector<std::string>{other.id}); // file name
    CHECK(found(rig, entry.timestamp) == just);                           // time, as stored
    CHECK(found(rig, "quick record") == just);                            // file name
    CHECK(found(rig, "laptop").size() == 2); // the computer that kept them there
    CHECK(found(rig, "notepad").empty());    // the app stays on this computer
    CHECK(found(rig, "nowhere").empty());
    CHECK(found(rig, "").size() == 2);
}
