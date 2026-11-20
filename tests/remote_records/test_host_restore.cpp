// tests/test_host_backup_recovery.py's restored host: a host's data root carried by its own
// .fvbackup into a second data root, a second host service over it, and the records the laptop
// kept on the first offered back only by the host owner's assignment. A OpenWhisper_CPP backup
// carries the host database, recordings, meetings and remote_records folders but no paired devices
// or tokens (backup/sanitize.cpp drops remote_host_devices; the keyring and the host's certificate
// are not in it), so the laptop pairs again under a new id, as in OpenWhisper.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "backup/create.h"
#include "backup/restore.h"
#include "remote/connection.h"
#include "remote/pairing_settings.h"
#include "remote_host/host_settings.h"
#include "remote_host/registry.h"
#include "sync_rig.h"

using namespace fv;
using Catch::Matchers::ContainsSubstring;
using records_rig::Paired;

namespace {

namespace fs = std::filesystem;

// tests/test_remote_records.py `Host(restored_root)` with _wire_service: a host service over a
// data root that already holds records, keeping them for its paired computers.
struct RestoredHost {
    RestoredHost(const fs::path& root, Paired& rig)
        : db(Database::open_or_create(root / path_from_utf8(identity().database_file))),
          meetings(remote_records::meeting_records(root / "meetings", db, rig.clock)),
          store(std::make_shared<remote_records::HostRecordStore>(
              root / "remote_records",
              remote_records::HostRecordStore::Kinds{
                  {"dictation", remote_records::dictation_records(root / "recordings", db)},
                  {"meeting", meetings}},
              rig.clock, [](const fs::path&) { return 8ULL << 30; })),
          host(root, "devbox", rig.reactor, rig.clock, io, store) {
        host.service->set_host_enabled(true);
        host.service->set_keep_records(true);
        REQUIRE(host.service->host_state().running);
    }

    // DeviceRegistry.pair through a pairing code, as the computer named `computer` asks.
    remote::ClientPairing pair(records_rig::Computer& computer) {
        const auto code = host.service->open_pairing();
        REQUIRE(code);
        return computer.service->pair(
            remote::format_address("127.0.0.1", host.service->host_state().port), *code, false);
    }

    records_rig::InlineExecutor io;
    Database db;
    std::shared_ptr<remote_records::RecordKind> meetings;
    std::shared_ptr<remote_records::HostRecordStore> store;
    records_rig::Computer host;
};

std::vector<std::string> ids_of(const std::vector<Json>& items) {
    std::vector<std::string> ids;
    for (const auto& item : items) {
        ids.push_back(item.at("id").get<std::string>());
    }
    return ids;
}

} // namespace

// OpenWhisper's last part (checking out the recovered meeting, renaming it and letting the next
// start send it back; bring_back returning the meeting and removing it from the host) is
// docs/port/differences.md a17-retention: a meeting is shared as a copy that never leaves this
// computer, edits are not sent on their own, and bring back moves dictations only. Here the
// shared meeting stays on both, and bring back counts the two dictations.
TEST_CASE("test_restored_host_recovery_requires_local_assignment_and_preserves_both_owners",
          "[test_host_backup_recovery]") {
    Paired rig;
    rig.sync->set_location("host");
    const auto entry = rig.dictate("Before host restore");
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    REQUIRE(rig.on_host(entry.id));
    meeting_state::SqlMeetingRepository local(rig.client_db, rig.clock);
    local.create_meeting(
        {{"id", "m_0123456789ab"},
         {"title", "Planning"},
         {"status", "ended"},
         {"started_at", "2026-02-01T10:00:00Z"},
         {"host_token", "local-token"},
         {"guest_token", "local-guest"},
         {"spool_dir", path_to_utf8(records_rig::made(rig.root() / "client" / "meetings" / "m"))}});
    rig.sync->share_meeting(rig.paired.fingerprint, "m_0123456789ab", false, {});
    const std::string original_owner = rig.paired.device_id;

    // The host stops, backs up its data root and that backup is restored into another one.
    rig.host.service->set_host_enabled(false);
    const fs::path archive = rig.root() / "host.fvbackup";
    backup::CreateOptions options;
    options.data_root = rig.host_root();
    options.app_version = "0.1.0";
    backup::create_backup(archive, options);
    const fs::path restored_root =
        fs::weakly_canonical(records_rig::made(rig.root() / "restored-host"));
    backup::prepare_restore(archive, restored_root);
    REQUIRE(backup::apply_pending_restore(restored_root));

    RestoredHost restored(restored_root, rig);
    // The laptop pairs with the restored host again; a second computer named the same pairs too.
    const auto paired = restored.pair(rig.client);
    records_rig::Computer stranger_computer(rig.root() / "stranger", "laptop", rig.reactor,
                                            rig.clock, rig.client_io);
    const auto other = restored.pair(stranger_computer);
    REQUIRE(rig.client.service->client_pairing()->fingerprint == paired.fingerprint);
    CHECK(paired.device_id != original_owner);
    auto& service = *restored.host.service;

    // What the old pairing stored is not this pairing's until the host owner assigns it.
    CHECK(rig.sync->list_remote("dictation").empty());
    const auto new_entry = rig.dictate("After re-pairing");
    CHECK(rig.sync->send_existing() == 1);
    rig.run();
    CHECK(ids_of(rig.sync->list_remote("dictation")) == std::vector<std::string>{new_entry.id});
    CHECK(service.recoverable_record_owners() ==
          std::vector<remote::RecordOwnerGroup>{
              {original_owner, "laptop", {{"dictation", 1}, {"meeting", 1}}}});

    {
        auto stranger = stranger_computer.service->client_connection();
        stranger->connect();
        // Owner ids a client names are not a grant: the host uses its own registry's.
        CHECK_THROWS_AS(stranger->request("records_open", Json{{"kind", "dictation"},
                                                               {"record_id", entry.id},
                                                               {"record_owner_ids",
                                                                Json::array({original_owner})}}),
                        remote::RemoteRequestError);
        CHECK(
            stranger
                ->request("records_list", Json{{"kind", "dictation"},
                                               {"record_owner_ids", Json::array({original_owner})}})
                .at("records")
                .empty());
        // Recovery is the host owner's action, never a client's.
        CHECK_THROWS_AS(stranger->request("records_recover", Json{{"kind", "dictation"},
                                                                  {"record_id", entry.id},
                                                                  {"owner_id", original_owner}}),
                        remote::RemoteRequestError);

        service.recover_device_records(original_owner, paired.device_id);
        const auto both = ids_of(rig.sync->list_remote("dictation"));
        CHECK(std::set<std::string>(both.begin(), both.end()) ==
              std::set<std::string>{entry.id, new_entry.id});
        CHECK(ids_of(rig.sync->list_remote("meeting")) ==
              std::vector<std::string>{"m_0123456789ab"});
        CHECK(service.records_summary(paired.device_id)["dictation"]["count"] == 2);
        CHECK(service.recoverable_record_owners().empty());
        CHECK_THROWS_WITH(service.recover_device_records(original_owner, other.device_id),
                          ContainsSubstring("no longer available"));
        CHECK(stranger->request("records_list", Json{{"kind", "dictation"}}).at("records").empty());
        CHECK_THROWS_AS(
            stranger->request("records_open", Json{{"kind", "dictation"}, {"record_id", entry.id}}),
            remote::RemoteRequestError);
    }

    // Reloading the registry keeps the grant, and the laptop's token still carries it.
    const auto registry = remote_host::settings_device_registry(restored.host.settings, rig.clock);
    const auto devices = registry->list();
    const auto saved = std::ranges::find(devices, paired.device_id, &remote::PairedDevice::id);
    REQUIRE(saved != devices.end());
    CHECK(saved->record_owner_ids == std::vector<std::string>{original_owner});
    const auto token =
        rig.client.credentials.get(remote::token_credential(rig.client.paths, identity()));
    REQUIRE(token);
    const auto authenticated = registry->authenticate(*token);
    REQUIRE(authenticated);
    CHECK(authenticated->id == paired.device_id);
    CHECK(authenticated->record_owner_ids == std::vector<std::string>{original_owner});

    // Both owners' dictations come home with their recordings; the shared meeting was a copy.
    rig.sync->set_location("local");
    CHECK(rig.sync->bring_back() == 2);
    const auto back = rig.history->get_entry(entry.id);
    REQUIRE(back);
    CHECK(back->text == "Before host restore");
    REQUIRE(back->audio_file);
    CHECK(rig.history->get_recording_path(*back->audio_file));
    CHECK(rig.history->get_entry(new_entry.id));
    const auto meeting = local.get_meeting("m_0123456789ab");
    REQUIRE(meeting);
    CHECK(meeting->value("origin_device_id", Json()).is_null());
    CHECK(HistoryRepository(restored.db).get_history_entries().empty());
}
