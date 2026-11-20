// The History calendar's source over records this computer keeps on its host
// (tests/test_history_calendar.py): the host's listing as RecordSync hands it on, each
// dictation made an entry by RecordSync::remote_history_entry, meetings read by the meeting
// store's calendar source. The listing itself is scripted, as OpenWhisper's FakeRecords does.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "db/database.h"
#include "history/calendar_source.h"
#include "history/repository.h"
#include "meeting_state/calendar_meetings.h"
#include "meeting_state/sql_repository.h"
#include "remote_records/sync.h"
#include "settings/store.h"
#include "testkit/testkit.h"

using namespace fv;

namespace {

class FakeRecords final : public history::RemoteRecordSource {
public:
    explicit FakeRecords(remote_records::RecordSync& sync) : sync_(sync) {}

    std::map<std::string, std::vector<Json>> remote;
    std::map<std::string, std::set<std::string>> copies;

    bool listing_wanted(std::string_view kind) override {
        return remote.contains(std::string(kind));
    }
    std::vector<Json> list_remote(std::string_view kind, std::string_view, int) override {
        auto items = remote[std::string(kind)];
        for (auto& item : items) {
            item["stored_on"] = "jed";
        }
        return items;
    }
    std::vector<std::string> copies_on_host(std::string_view kind,
                                            const std::vector<std::string>& ids) override {
        std::vector<std::string> found;
        for (const auto& id : ids) {
            if (copies[std::string(kind)].contains(id)) {
                found.push_back(id);
            }
        }
        return found;
    }
    std::string host_name() override { return "jed"; }
    HistoryEntry remote_history_entry(const Json& item) override {
        return sync_.remote_history_entry(item);
    }

private:
    remote_records::RecordSync& sync_;
};

} // namespace

TEST_CASE("test_source_merges_host_kept_records_and_copies", "[test_history_calendar]") {
    testkit::TempDataRoot data;
    auto db = Database::open_or_create(data.path() / "fiorivoce.db");
    const FakeClock clock(FakeClock::default_utc(), "America/Los_Angeles");
    FakeClock sync_clock;
    ManualExecutor io;
    SettingsStore settings(data.path() / "settings.json");
    auto sync = std::make_shared<remote_records::RecordSync>(remote_records::SyncDeps{
        .db = db,
        .settings = settings,
        .clock = sync_clock,
        .io = io,
        .transfer_root = data.path() / "remote_records",
        .recordings = data.path() / "recordings",
        .meetings = data.path() / "meetings",
        .pairing = [] { return std::optional<remote::ClientPairing>(); },
        .connect = [](const remote::ClientPairing&) -> std::unique_ptr<remote::RemoteConnection> {
            FAIL("nothing connects");
            return nullptr;
        },
    });
    HistoryRepository history(db);
    for (const auto& [id, when] : {std::pair{"local", "2026-09-03T15:00:00+00:00"},
                                   std::pair{"both", "2026-09-04T15:00:00+00:00"}}) {
        history.put_history_entry(
            {.id = id, .text = "kept here", .timestamp = when, .model = "parakeet"});
    }
    FakeRecords records(*sync);
    records.remote["dictation"] = {
        {{"id", "hosted"},
         {"text", "kept on the host"},
         {"timestamp", "2026-09-05T15:00:00+00:00"},
         {"model", "parakeet"},
         {"audio_duration", 3.0},
         {"has_audio", true}},
        // Also here: listed once, as this computer's.
        {{"id", "both"},
         {"text", "dup"},
         {"timestamp", "2026-09-04T15:00:00+00:00"},
         {"model", "parakeet"}},
    };
    records.remote["meeting"] = {{{"id", "m_host"},
                                  {"title", "Planning"},
                                  {"status", "ended"},
                                  {"started_at", "2026-09-06T16:00:00Z"},
                                  {"ended_at", "2026-09-06T17:00:00Z"}}};
    records.copies["dictation"] = {"both"};
    meeting_state::SqlMeetingCalendarSource meetings(
        std::make_shared<meeting_state::SqlMeetingRepository>(db, clock), clock);
    history::CalendarSource source(db, clock, &meetings, &records);

    std::vector<std::string> local;
    for (const auto& mark : source.load_index()) {
        local.push_back(mark.record_id);
    }
    CHECK(source.remote_wanted());
    std::set<std::pair<std::string, history::Place>> remote;
    for (const auto& mark : source.load_remote(local)) {
        remote.emplace(mark.record_id, mark.place);
    }
    const history::Place on_jed{std::string(history::ON), "jed"};
    CHECK(remote ==
          std::set<std::pair<std::string, history::Place>>{{"hosted", on_jed}, {"m_host", on_jed}});
    CHECK(source.notice().empty());

    std::map<std::string, history::CalendarItem> items;
    for (auto& item : source.load_month(2026, 9)) {
        items.emplace(item.record_id, item);
    }
    CHECK(items.size() == 4);
    CHECK(items.contains("local"));
    CHECK(items.at("both").location() ==
          std::make_pair(std::string("Also on jed"), std::string("Kept here and on jed")));
    CHECK(items.at("hosted").location().first == "On jed");
    const auto& hosted = std::get<HistoryEntry>(items.at("hosted").source);
    CHECK(hosted.stored_on == "jed");
    CHECK(hosted.remote_audio);
    CHECK(items.at("m_host").title == "Planning");
    CHECK(items.at("local").location() == std::make_pair(std::string(), std::string()));
}
