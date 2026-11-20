// Records sent to a paired host over the real wire (tests/test_remote_records.py and the files
// that share its fixtures). FioriVoce shares records explicitly: nothing is queued when a record
// is saved, edited, deleted or cleared here (docs/port/differences.md a17-retention), and a
// transfer refused because a backup runs is retried by asking again (a17-backup-transfer). The
// tests of OpenWhisper behaviour those decisions replace check the FioriVoce behaviour instead.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <iterator>

#include "remote/connection.h"
#include "remote_records/gate.h"
#include "sync_rig.h"

using namespace fv;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using records_rig::Paired;

namespace {

std::size_t files_in(const std::filesystem::path& folder) {
    if (!std::filesystem::exists(folder)) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(folder),
                                                  std::filesystem::directory_iterator()));
}

} // namespace

TEST_CASE("test_local_mode_sends_nothing", "[test_remote_records]") {
    Paired rig;
    const auto entry = rig.dictate();
    CHECK(rig.sync->location() == "local");
    CHECK(rig.sync->send_existing() == 0);
    rig.sync->wake(true);
    rig.run();
    CHECK_FALSE(rig.on_host(entry.id));
    CHECK(rig.sync->status().pending == 0);
    CHECK(rig.history->get_entry(entry.id));
}

TEST_CASE("test_a_host_that_doesnt_keep_records_leaves_them_here", "[test_remote_records]") {
    Paired rig;
    rig.host.service->set_keep_records(false);
    rig.sync->set_location("host");
    const auto entry = rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    CHECK(rig.history->get_entry(entry.id));
    const auto status = rig.sync->status();
    CHECK(status.host_keeps == false);
    CHECK(status.pending == 1);
    CHECK_THAT(status.waiting, ContainsSubstring("Keep records for paired computers"));
    CHECK_THAT(status.error, ContainsSubstring("isn't keeping records for other computers yet"));

    rig.host.service->set_keep_records(true);
    rig.sync->wake(true);
    rig.run();
    CHECK_FALSE(rig.history->get_entry(entry.id));
    CHECK(rig.on_host(entry.id));
    CHECK(rig.sync->status().waiting.empty());
}

TEST_CASE("a copy kept on both stays on the host when it is deleted or cleared here",
          "[test_remote_records][a17-retention]") {
    // test_both_mode_copies_and_a_delete_here_deletes_the_host_copy and
    // test_clearing_history_here_clears_it_on_the_host: OpenWhisper queues the host copy's
    // deletion. Here what the user shared stays shared until it is deleted there.
    Paired rig;
    rig.sync->set_location("both");
    const auto one = rig.dictate("one");
    const auto two = rig.dictate("two");
    CHECK(rig.sync->send_existing() == 2);
    rig.run();
    CHECK(rig.history->get_entry(one.id));
    CHECK(rig.on_host(one.id));
    const std::vector<std::string> ids{one.id};
    CHECK(rig.sync->copies_on_host("dictation", std::span<const std::string>(ids)) ==
          std::set<std::string>{one.id});

    CHECK(rig.history->delete_entry(one.id, true));
    rig.sync->wake(true);
    rig.run();
    CHECK(rig.on_host(one.id));
    rig.history->clear_history();
    rig.sync->wake(true);
    rig.run();
    CHECK(rig.on_host(two.id));
    CHECK(rig.sync->list_remote("dictation").size() == 2);
}

TEST_CASE("test_an_unedited_upload_is_done_once", "[test_personalize_fix_f5_sync_edit_race]") {
    Paired rig;
    rig.sync->set_location("both");
    const auto entry = rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    CHECK(rig.on_host(entry.id));
    CHECK(rig.sync->status().pending == 0);
    // Asked again, nothing changed: nothing is queued or sent.
    CHECK(rig.sync->send_existing() == 0);
    rig.sync->wake(true);
    rig.run();
    CHECK(rig.sync->status().pending == 0);
    CHECK(rig.sync->status().error.empty());
}

TEST_CASE("test_a_moved_entry_stays_here_until_the_edit_is_sent",
          "[test_personalize_fix_f5_sync_edit_race]") {
    // The original chosen while the AI version is on its way: the edited entry stays here.
    // FioriVoce does not send the edit on its own (a17-retention); both copies are kept and
    // the transfer says so.
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate("Hello from the laptop.", "hello from the laptop um");
    CHECK(rig.sync->send_existing() == 1);
    // On the host's thread, as it answers records_begin: no assertion there.
    std::atomic<bool> edited{false};
    rig.on_begin = [&] {
        if (!edited.exchange(true)) {
            (void)rig.history->use_version(entry.id, ORIGINAL_VERSION);
        }
    };
    rig.run();
    CHECK(edited);
    const auto here = rig.history->get_entry(entry.id);
    REQUIRE(here);
    CHECK(here->text == "hello from the laptop um");
    CHECK(rig.on_host(entry.id)->text == "Hello from the laptop.");
    const auto status = rig.sync->status();
    CHECK_THAT(status.error, ContainsSubstring("Both copies were kept"));
    CHECK(status.pending == 1);
}

TEST_CASE("test_an_edit_to_a_record_never_sent_queues_nothing",
          "[test_personalize_s9_remote_records]") {
    Paired rig;
    const auto entry = rig.dictate("Hello from the laptop.", "hello from the laptop um");
    CHECK(rig.history->use_version(entry.id, ORIGINAL_VERSION));
    CHECK(rig.sync->status().pending == 0);
}

TEST_CASE("test_an_interrupted_upload_resumes_where_it_stopped", "[test_remote_records]") {
    Paired rig;
    rig.sync->set_location("host");
    // Five chunks of audio; the pairing goes away as the second is about to be sent. The sync
    // asks for the pairing before every chunk, so its asks count the chunks it sends.
    const auto first = rig.dictate("long one", std::nullopt, 4 * remote::RECORD_CHUNK_BYTES);
    CHECK(rig.sync->send_existing() == 1);
    rig.unpaired_at = rig.pairing_asks + 5;
    rig.run();
    CHECK(rig.history->get_entry(first.id));
    CHECK_FALSE(rig.on_host(first.id));

    rig.unpaired_at = 0;
    int asks = rig.pairing_asks;
    rig.sync->wake(true);
    rig.run();
    const int resumed = rig.pairing_asks - asks;
    REQUIRE(rig.on_host(first.id));
    CHECK_FALSE(rig.history->get_entry(first.id));

    // The same record from the start, for comparison: one chunk more.
    const auto second = rig.dictate("long two", std::nullopt, 4 * remote::RECORD_CHUNK_BYTES);
    CHECK(rig.sync->send_existing() == 1);
    asks = rig.pairing_asks;
    rig.run();
    REQUIRE(rig.on_host(second.id));
    CHECK(rig.pairing_asks - asks == resumed + 1);
}

TEST_CASE("test_an_interrupted_download_is_not_kept_as_the_cache",
          "[test_personalize_fix3_h3_audio_fetch]") {
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate("kept there", std::nullopt, 2 * remote::RECORD_CHUNK_BYTES);
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    REQUIRE(rig.on_host(entry.id));

    // The pairing changes once the recording has arrived, before it is kept.
    rig.unpaired_at = rig.pairing_asks + 4;
    CHECK_THROWS_AS(rig.sync->audio_for(entry.id), remote::RecordsUnavailable);
    const auto transfers = rig.root() / "client" / "remote_records";
    for (const auto& item : std::filesystem::recursive_directory_iterator(transfers)) {
        CHECK_FALSE(item.is_regular_file());
    }
    const auto audio = rig.sync->audio_for(entry.id);
    CHECK(std::filesystem::file_size(audio) == 2 * remote::RECORD_CHUNK_BYTES + 12);
    CHECK(files_in(audio.parent_path()) == 1);
    CHECK(rig.sync->audio_for(entry.id) == audio);
}

TEST_CASE("a transfer refused while the host backs up is sent when asked again",
          "[test_host_backup_recovery][a17-backup-transfer]") {
    // test_backup_busy_is_retryable_and_never_exhausts_client_records: OpenWhisper's worker
    // keeps the record pending and retries by itself. Here the refusal is reported and the
    // record stays here until the user asks again.
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate();
    {
        auto* gate = rig.host.service->record_gate();
        REQUIRE(gate != nullptr);
        const auto pause = gate->paused();
        auto connection = rig.client.service->client_connection(rig.paired);
        connection->connect({});
        try {
            connection->request("records_list", Json{{"kind", "dictation"}}, 10s);
            FAIL("records_list answered during the host's backup");
        } catch (const remote::RemoteRequestError& error) {
            CHECK(error.retryable());
            CHECK(error.code == "busy");
        }
        for (int attempt = 0; attempt < 6; ++attempt) {
            CHECK(rig.sync->send_existing() >= 0);
            rig.run();
        }
        CHECK(rig.history->get_entry(entry.id));
        CHECK_FALSE(rig.on_host(entry.id));
        CHECK(rig.sync->status().pending == 1);
    }
    rig.sync->wake(true);
    rig.run();
    CHECK_FALSE(rig.history->get_entry(entry.id));
    CHECK(rig.on_host(entry.id));
}

TEST_CASE("a backup here turns away new transfers and bring back until it ends",
          "[test_backup_record_sync][a17-backup-transfer]") {
    // test_backup_pauses_upload_and_bring_back_and_resumes: OpenWhisper's worker waits for the
    // backup and resumes. Here both are refused as busy and work when asked again.
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate();
    {
        const auto pause = rig.sync->paused_for_backup();
        CHECK_THROWS_AS(rig.sync->send_existing(), remote_records::RecordsBusy);
        CHECK_THROWS_AS(rig.sync->bring_back(), remote_records::RecordsBusy);
    }
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    CHECK(rig.on_host(entry.id));
    rig.sync->set_location("local");
    CHECK(rig.sync->bring_back() == 1);
    CHECK(rig.history->get_entry(entry.id));
    CHECK_FALSE(rig.on_host(entry.id));
}

TEST_CASE("a transfer that fails is recorded as failed in the outbox", "[remote_records][outbox]") {
    // Outbox::fail used to leave the row untouched (its action was never bound).
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate("Hello from the laptop.", "hello from the laptop um");
    CHECK(rig.sync->send_existing() == 1);
    std::atomic<bool> edited{false};
    rig.on_begin = [&] {
        if (!edited.exchange(true)) {
            (void)rig.history->use_version(entry.id, ORIGINAL_VERSION);
        }
    };
    rig.run();
    const auto state = rig.client_db.read([&](sql::Connection& connection) {
        auto query = connection.prepare(
            "SELECT state, attempts, last_error FROM record_sync WHERE record_id=?");
        query.bind(1, entry.id);
        REQUIRE(query.step());
        return std::make_tuple(std::get<std::string>(query.column(0)),
                               std::get<std::int64_t>(query.column(1)),
                               std::get<std::string>(query.column(2)));
    });
    CHECK(std::get<0>(state) == "failed");
    CHECK(std::get<1>(state) == 1);
    CHECK_THAT(std::get<2>(state), ContainsSubstring("Both copies were kept"));
}

TEST_CASE("list_remote answers quietly for a while after the host was unreachable",
          "[remote_records][test_history_calendar]") {
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    CHECK(rig.sync->list_remote("dictation").size() == 1);

    rig.host.service->set_host_enabled(false);
    CHECK_THROWS_WITH(rig.sync->list_remote("dictation"), "devbox isn't reachable right now.");
    // Not asked again within the quiet time: only the pairing is read.
    int asks = rig.pairing_asks;
    CHECK_THROWS_WITH(rig.sync->list_remote("dictation"), "devbox isn't reachable right now.");
    CHECK(rig.pairing_asks == asks + 1);
    // Past it, the host is tried again.
    rig.clock.advance(remote_records::RecordSync::kUnreachableQuiet + 1s);
    asks = rig.pairing_asks;
    CHECK_THROWS_WITH(rig.sync->list_remote("dictation"), "devbox isn't reachable right now.");
    CHECK(rig.pairing_asks > asks + 1);
    (void)entry;
}

TEST_CASE("test_removing_recovery_pairing_keeps_or_deletes_all_granted_records",
          "[test_host_backup_recovery]") {
    const bool delete_records = GENERATE(false, true);
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    REQUIRE(rig.on_host(entry.id));
    const Json meeting{{"kind", "meeting"},
                       {"format", 1},
                       {"session",
                        {{"id", "m_0123456789ab"},
                         {"title", "Planning"},
                         {"status", "ended"},
                         {"started_at", "2026-02-01T10:00:00Z"}}},
                       {"files", Json::object()}};
    rig.host_meetings->import_record("m_0123456789ab", records_rig::made(rig.root() / "meeting"),
                                     meeting, {.id = rig.paired.device_id, .name = "laptop"});
    meeting_state::SqlMeetingRepository kept(rig.host_db, rig.clock);

    records_rig::Computer replacement(rig.root() / "replacement", "new laptop", rig.reactor,
                                      rig.clock, rig.client_io);
    const auto code = rig.host.service->open_pairing();
    REQUIRE(code);
    const auto fresh = replacement.service->pair(
        remote::format_address("127.0.0.1", rig.host.service->host_state().port), *code, false);
    CHECK(rig.host.service->remove_device(rig.paired.device_id, false));
    rig.host.service->recover_device_records(rig.paired.device_id, fresh.device_id);
    CHECK(rig.host.service->recoverable_record_owners().empty());
    CHECK_THROWS_WITH(
        rig.host.service->recover_device_records(rig.paired.device_id, fresh.device_id),
        "These records are no longer available for recovery.");
    CHECK(rig.host.service->remove_device(fresh.device_id, delete_records));
    if (delete_records) {
        CHECK_FALSE(rig.on_host(entry.id));
        CHECK_FALSE(kept.get_meeting("m_0123456789ab"));
        CHECK(rig.host.service->recoverable_record_owners().empty());
    } else {
        CHECK(rig.on_host(entry.id));
        CHECK(kept.get_meeting("m_0123456789ab"));
        const auto owners = rig.host.service->recoverable_record_owners();
        REQUIRE(owners.size() == 1);
        CHECK(owners[0].id == rig.paired.device_id);
        CHECK(owners[0].name == "laptop");
        CHECK(owners[0].counts ==
              std::map<std::string, std::int64_t>{{"dictation", 1}, {"meeting", 1}});
    }
}

TEST_CASE("a checked out dictation is kept on both and not sent back unchanged",
          "[test_remote_records][A17-60]") {
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    REQUIRE_FALSE(rig.history->get_entry(entry.id));
    std::vector<std::pair<std::int64_t, std::int64_t>> progress;
    rig.sync->check_out("dictation", entry.id, [&](std::int64_t got, std::int64_t all) {
        progress.emplace_back(got, all);
    });
    CHECK_FALSE(progress.empty());
    CHECK(rig.history->get_entry(entry.id));
    CHECK(rig.on_host(entry.id));
    const std::vector<std::string> ids{entry.id};
    CHECK(rig.sync->copies_on_host("dictation", std::span<const std::string>(ids)) ==
          std::set<std::string>{entry.id});
    // Asked to move everything again, the unchanged copy is not queued.
    CHECK(rig.sync->send_existing() == 0);
    CHECK(rig.sync->status().pending == 0);
}

TEST_CASE("bringing back counts each record and leaves one that can't come home",
          "[test_remote_records][A17-61]") {
    Paired rig;
    rig.sync->set_location("host");
    const auto first = rig.dictate("first");
    const auto second = rig.dictate("second");
    CHECK(rig.sync->send_existing() == 2);
    rig.run();
    REQUIRE(rig.on_host(first.id));
    REQUIRE(rig.on_host(second.id));
    // A different version of the second is here now.
    HistoryRepository(rig.client_db)
        .put_history_entry({.id = second.id,
                            .text = "edited here",
                            .timestamp = second.timestamp,
                            .model = "base"});
    std::vector<std::string> lines;
    rig.sync->set_location("local");
    CHECK(rig.sync->bring_back([&](std::string line) { lines.push_back(std::move(line)); }) == 1);
    CHECK(lines == std::vector<std::string>{"Bringing back 1…", "Bringing back 1…"});
    CHECK(rig.history->get_entry(first.id));
    CHECK_FALSE(rig.on_host(first.id));
    CHECK(rig.on_host(second.id));
    CHECK(rig.history->get_entry(second.id)->text == "edited here");
    CHECK_THAT(rig.sync->status().error, ContainsSubstring("Both copies were kept"));
}

TEST_CASE("records waiting to go follow a new storage choice", "[test_remote_records][A17-57]") {
    Paired rig;
    CHECK_THROWS_WITH(rig.sync->set_location("cloud"), "Unknown storage location: 'cloud'");
    rig.host.service->set_keep_records(false);
    rig.sync->set_location("host");
    rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    const auto action = [&] {
        return rig.client_db.read([](sql::Connection& connection) {
            auto query = connection.prepare("SELECT action FROM record_sync");
            return query.step() ? std::get<std::string>(query.column(0)) : std::string();
        });
    };
    CHECK(action() == "move");
    rig.sync->set_location("both");
    CHECK(action() == "copy");
    rig.sync->set_location("local");
    CHECK(action().empty());
    CHECK(rig.sync->status().pending == 0);
}

TEST_CASE("the host's summary and shared meetings are read back", "[test_remote_records][A17-58]") {
    Paired rig;
    rig.sync->set_location("host");
    rig.dictate();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    rig.sync->refresh_summary();
    const auto stored = rig.sync->status().stored;
    REQUIRE(stored.contains("dictation"));
    CHECK(stored.at("dictation").count == 1);
    CHECK(stored.at("dictation").bytes > 0);

    meeting_state::SqlMeetingRepository local(rig.client_db, rig.clock);
    const auto spool = records_rig::made(rig.root() / "client" / "meetings" / "local");
    local.create_meeting({{"id", "m_local"},
                          {"title", "Local agenda"},
                          {"status", "ended"},
                          {"started_at", "2026-02-01T10:00:00Z"},
                          {"host_token", "local-token"},
                          {"guest_token", "local-guest"},
                          {"spool_dir", path_to_utf8(spool)}});
    rig.sync->share_meeting(rig.paired.fingerprint, "m_local", false, {});
    const std::vector<std::string> ids{"m_local"};
    CHECK(rig.sync->copies_on_host("meeting", std::span<const std::string>(ids)) ==
          std::set<std::string>{"m_local"});
    CHECK(rig.sync->status().pending == 0);
}

TEST_CASE("an entry the host keeps without its recording says so",
          "[test_remote_records][A17-59]") {
    Paired rig;
    rig.sync->set_location("host");
    AddEntry fields;
    fields.text = "no audio";
    fields.model = "base";
    const auto entry = rig.history->add_entry(fields).value();
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    REQUIRE(rig.on_host(entry.id));
    CHECK_THROWS_WITH(rig.sync->audio_for(entry.id), "That entry has no recording on the host.");
}

TEST_CASE("records left by removed computers are offered by name",
          "[test_host_backup_recovery][A17-50]") {
    Paired rig;
    const auto meeting = [&](std::string id, std::string owner, std::string name) {
        const Json record{{"kind", "meeting"},
                          {"format", 1},
                          {"session",
                           {{"id", id},
                            {"title", "Planning"},
                            {"status", "ended"},
                            {"started_at", "2026-02-01T10:00:00Z"}}},
                          {"files", Json::object()}};
        rig.host_meetings->import_record(id, records_rig::made(rig.root() / "meeting"), record,
                                         {.id = owner, .name = name});
    };
    meeting("m_one", "z_device", "Zeta laptop");
    meeting("m_two", "a_device", "Alpha laptop");
    meeting("m_three", "b_device", "Alpha laptop");
    std::vector<std::pair<std::string, std::string>> order;
    for (const auto& owner : rig.host.service->recoverable_record_owners()) {
        order.emplace_back(owner.name, owner.id);
    }
    CHECK(order == std::vector<std::pair<std::string, std::string>>{{"Alpha laptop", "a_device"},
                                                                    {"Alpha laptop", "b_device"},
                                                                    {"Zeta laptop", "z_device"}});
}

TEST_CASE("test_history_channel_requires_authentication", "[test_client_history]") {
    // The direct-history channel FioriVoce's explicit history session uses
    // (a17-direct-history) is refused to a computer that isn't paired, as the speech one is.
    Paired rig;
    remote::ClientEnv env{rig.reactor};
    remote::RemoteConnection connection(env, "127.0.0.1", rig.host.service->host_state().port,
                                        "invalid-token", rig.paired.fingerprint, {}, true);
    CHECK_THROWS_WITH(connection.connect(), ContainsSubstring("paired"));
}
