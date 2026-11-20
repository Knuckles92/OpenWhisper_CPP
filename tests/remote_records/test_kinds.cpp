// The record format and the host's store, driven through HostRecordStore::handle as the host's
// connection drives it: tests/test_remote_records.py (the store half) and
// tests/test_personalize_s9_remote_records.py (an entry's newer fields).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "base/atomic_file.h"
#include "base/hash.h"
#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "history/repository.h"
#include "meeting_state/sql_repository.h"
#include "pyport/errors.h"
#include "remote_records/dictation_records.h"
#include "remote_records/meeting_records.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::remote_records;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

namespace fs = std::filesystem;

std::string wav(std::size_t extra = 16) {
    std::string riff("RIFF");
    riff.append(4, char{0});
    riff += "WAVE";
    riff.append(extra, char{1});
    return riff;
}

std::span<const std::byte> bytes_of(std::string_view data) {
    return std::as_bytes(std::span(data.data(), data.size()));
}

fs::path made(const fs::path& folder) {
    fs::create_directories(folder);
    return folder;
}

// A host: its own database, recordings and meetings, and the store over them.
struct Host {
    explicit Host(const fs::path& root)
        : root(made(root)), db(Database::open_or_create(root / "host.db")),
          dictations(dictation_records(root / "recordings", db)),
          meetings(meeting_records(root / "meetings", db, clock)),
          store(root / "remote_records", {{"dictation", dictations}, {"meeting", meetings}}, clock,
                [this](const fs::path&) { return free_space.load(); }) {}

    Json call(std::string_view op, const remote::PairedDevice& device, Json fields,
              std::string_view payload = {}) {
        return store.handle(op, fields, bytes_of(payload), device);
    }

    // records_begin, one records_put per file and records_commit, as RecordSync::upload sends a
    // small record.
    Json upload(const remote::PairedDevice& device, std::string_view kind, std::string_view id,
                const std::map<std::string, std::string>& files) {
        const Json key{{"kind", kind}, {"record_id", id}};
        std::uint64_t total = 0;
        for (const auto& [name, data] : files) {
            total += data.size();
        }
        auto begin = key;
        begin["files"] = files.size();
        begin["bytes"] = total;
        call("records_begin", device, begin);
        for (const auto& [name, data] : files) {
            auto put = key;
            put["name"] = name;
            put["size"] = data.size();
            put["sha256"] = sha256_hex(data);
            put["offset"] = 0;
            REQUIRE(call("records_put", device, put, data)["done"] == true);
        }
        return call("records_commit", device, key);
    }

    fs::path root;
    // What the store's free-space probe answers.
    std::atomic<std::uint64_t> free_space{8ULL << 30};
    FakeClock clock;
    Database db;
    std::shared_ptr<RecordKind> dictations;
    std::shared_ptr<RecordKind> meetings;
    HostRecordStore store;
};

const remote::PairedDevice laptop{.id = "laptop_1", .name = "laptop"};

// The bundle this computer's DictationRecords makes of one entry: record.json and its files.
std::map<std::string, std::string> bundle_of(RecordKind& records, std::string_view id,
                                             const fs::path& folder) {
    fs::create_directories(folder);
    std::map<std::string, std::string> out;
    for (const auto& [name, path] : records.export_record("", id, folder)) {
        std::ifstream in(path, std::ios::binary);
        out[name] = std::string(std::istreambuf_iterator<char>(in), {});
    }
    std::ifstream manifest(folder / "record.json", std::ios::binary);
    out["record.json"] = std::string(std::istreambuf_iterator<char>(manifest), {});
    return out;
}

Json manifest_of(const std::map<std::string, std::string>& bundle) {
    return Json::parse(bundle.at("record.json"));
}

// _dictate: an entry with its recording, in a client's database.
HistoryEntry dictate(HistoryRepository& history, const fs::path& recordings, std::string id,
                     HistoryEntry entry = {}) {
    fs::create_directories(recordings);
    write_file_atomic(recordings / (id + ".wav"), wav());
    entry.id = id;
    if (entry.text.empty()) {
        entry.text = "Hello from the laptop.";
    }
    entry.raw_text = entry.raw_text.value_or("hello from the laptop um");
    entry.timestamp = entry.timestamp.empty() ? "2026-10-06T10:00:00+00:00" : entry.timestamp;
    entry.model = "parakeet (cuda)";
    entry.audio_file = id + ".wav";
    history.put_history_entry(entry);
    return entry;
}

const std::map<std::string, std::optional<std::string>> kExt{
    {"entry_kind", "dictation"},
    {"cleanup_level", "medium"},
    {"cleaned_text", "Hello from the laptop."},
    {"language", "en"},
};

std::map<std::string, std::optional<std::string>> ext_of(const HistoryEntry& entry) {
    return {{"entry_kind", entry.entry_kind},
            {"cleanup_level", entry.cleanup_level},
            {"cleaned_text", entry.cleaned_text},
            {"language", entry.language}};
}

} // namespace

TEST_CASE("test_the_newer_fields_travel_beside_the_entry_but_the_app_stays_here",
          "[test_personalize_s9_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    auto client_db = Database::open_or_create(data.path() / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client-recordings", client_db);
    HistoryEntry fields;
    fields.app_id = "slack.exe";
    fields.app_name = "Slack";
    fields.app_category = "work";
    fields.cleanup_level = "medium";
    fields.entry_kind = "dictation";
    fields.language = "en";
    fields.cleaned_text = "Hello from the laptop.";
    dictate(history, data.path() / "client-recordings", "e1", fields);

    const auto bundle = bundle_of(*mine, "e1", data.path() / "bundle");
    const auto record = manifest_of(bundle);
    CHECK(record["entry_ext"] == Json(Json{{"entry_kind", "dictation"},
                                           {"cleanup_level", "medium"},
                                           {"cleaned_text", "Hello from the laptop."},
                                           {"language", "en"}}));
    for (const auto& [name, value] : kExt) {
        CHECK_FALSE(record["entry"].contains(name));
    }
    CHECK(bundle.at("record.json").find("app_name") == std::string::npos);

    CHECK(host.upload(laptop, "dictation", "e1", bundle)["stored"] == true);
    const auto stored = HistoryRepository(host.db).get_history_entry_by_id("e1");
    REQUIRE(stored);
    CHECK(ext_of(*stored) == kExt);
    CHECK_FALSE(stored->app_name);
    CHECK_FALSE(stored->app_id);
    const auto listed = host.call("records_list", laptop, {{"kind", "dictation"}})["records"];
    REQUIRE(listed.size() == 1);
    for (const auto& [name, value] : kExt) {
        CHECK(listed[0][name] == *value);
    }
    CHECK_FALSE(listed[0].contains("app_name"));

    // Brought back: the host's export of it, imported here again as this computer's own.
    const auto exported = data.path() / "back";
    fs::create_directories(exported);
    const auto sent = host.dictations->export_record(laptop.id, "e1", exported);
    REQUIRE(sent.size() == 1);
    fs::create_directories(exported / "audio");
    fs::copy_file(sent.begin()->second, exported / "audio" / "recording.wav");
    std::ifstream sent_back(exported / "record.json", std::ios::binary);
    const auto returned = Json::parse(sent_back);
    CHECK(returned["entry_ext"] == record["entry_ext"]);
    auto home_db = Database::open_or_create(data.path() / "home.db");
    const auto home = dictation_records(data.path() / "home-recordings", home_db);
    home->import_record("e1", exported, returned, {});
    const auto back = HistoryRepository(home_db).get_history_entry_by_id("e1");
    REQUIRE(back);
    CHECK(ext_of(*back) == kExt);
    CHECK_FALSE(back->origin_device_id);
}

TEST_CASE("test_an_entry_from_an_older_computer_still_imports",
          "[test_personalize_s9_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    auto client_db = Database::open_or_create(data.path() / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client-recordings", client_db);
    HistoryEntry fields;
    fields.entry_kind = "dictation";
    dictate(history, data.path() / "client-recordings", "e1", fields);
    auto bundle = bundle_of(*mine, "e1", data.path() / "bundle");
    auto record = manifest_of(bundle);
    record.erase("entry_ext");
    bundle["record.json"] = record.dump();

    CHECK(host.upload(laptop, "dictation", "e1", bundle)["stored"] == true);
    const auto stored = HistoryRepository(host.db).get_history_entry_by_id("e1");
    REQUIRE(stored);
    CHECK(stored->text == "Hello from the laptop.");
    for (const auto& [name, value] : ext_of(*stored)) {
        CHECK_FALSE(value);
    }
}

TEST_CASE("test_a_newer_computers_extra_fields_are_left_out_not_refused",
          "[test_personalize_s9_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    auto client_db = Database::open_or_create(data.path() / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client-recordings", client_db);
    dictate(history, data.path() / "client-recordings", "e1");
    auto bundle = bundle_of(*mine, "e1", data.path() / "bundle");
    auto record = manifest_of(bundle);
    record["entry"]["mood"] = "cheerful";
    record["entry_ext"].update(Json{{"language", 7}, {"cleanup_level", "high"}, {"sparkle", true}});
    bundle["record.json"] = record.dump();

    CHECK(host.upload(laptop, "dictation", "e1", bundle)["stored"] == true);
    const auto stored = HistoryRepository(host.db).get_history_entry_by_id("e1");
    REQUIRE(stored);
    CHECK(stored->cleanup_level == "high");
    CHECK_FALSE(stored->language);
}

TEST_CASE("test_known_entry_fields_are_still_checked", "[test_personalize_s9_remote_records]") {
    testkit::TempDataRoot data;
    auto db = Database::open_or_create(data.path() / "host.db");
    const auto records = dictation_records(data.path() / "recordings", db);
    Json record{{"kind", "dictation"},
                {"format", 1},
                {"audio", nullptr},
                {"entry",
                 {{"id", "e1"},
                  {"text", 42},
                  {"timestamp", "2026-10-06T10:00:00+00:00"},
                  {"model", "base"}}}};
    const remote::PairedDevice device{.id = "device-1", .name = "laptop"};
    CHECK_THROWS_MATCHES(records->import_record("e1", data.path(), record, device), py::ValueError,
                         MessageMatches(ContainsSubstring("text")));
    record["entry"]["text"] = "fine";
    record["entry_ext"] = Json::array({"not", "an", "object"});
    records->import_record("e1", data.path(), record, device);
    CHECK(HistoryRepository(db).get_history_entry_by_id("e1")->text == "fine");
}

TEST_CASE("test_a_damaged_file_is_refused_and_sent_again", "[test_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    auto client_db = Database::open_or_create(data.path() / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client-recordings", client_db);
    dictate(history, data.path() / "client-recordings", "e1");
    const auto bundle = bundle_of(*mine, "e1", data.path() / "bundle");
    const auto& [name, audio] = *bundle.begin();
    REQUIRE(name == "audio/recording.wav");

    const Json key{{"kind", "dictation"}, {"record_id", "e1"}};
    auto begin = key;
    begin["files"] = 2;
    begin["bytes"] = audio.size() + bundle.at("record.json").size();
    host.call("records_begin", laptop, begin);
    auto put = key;
    put.update(Json{
        {"name", name}, {"size", audio.size()}, {"sha256", std::string(64, '0')}, {"offset", 0}});
    CHECK_THROWS_MATCHES(host.call("records_put", laptop, put, audio), py::ValueError,
                         MessageMatches(ContainsSubstring("damaged")));
    CHECK_THROWS_MATCHES(host.call("records_commit", laptop, key), py::ValueError,
                         MessageMatches(ContainsSubstring("isn't complete")));
    CHECK_FALSE(HistoryRepository(host.db).get_history_entry_by_id("e1"));

    // Sent again whole, it is kept. The damaged file's declaration stays, so the next put with
    // the right hash is refused as "bad_request", and RecordSync::upload aborts and starts over.
    auto right = put;
    right["sha256"] = sha256_hex(audio);
    CHECK_THROWS_MATCHES(host.call("records_put", laptop, right, audio), py::ValueError,
                         MessageMatches(ContainsSubstring("start again")));
    host.call("records_abort", laptop, key);
    CHECK(host.upload(laptop, "dictation", "e1", bundle)["stored"] == true);
    CHECK(HistoryRepository(host.db).get_history_entry_by_id("e1"));
}

TEST_CASE("test_a_finished_upload_leaves_no_record_lock_behind", "[test_remote_records]") {
    // OpenWhisper counts the store's per-upload locks; this store serializes uploads on one
    // mutex and counts requests in its gate. After a finished upload, nothing is held: a
    // backup's pause gets the gate at once, and the next upload goes through.
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    auto client_db = Database::open_or_create(data.path() / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client-recordings", client_db);
    dictate(history, data.path() / "client-recordings", "e1");
    CHECK(host.upload(laptop, "dictation", "e1",
                      bundle_of(*mine, "e1", data.path() / "b1"))["stored"] == true);
    CHECK_FALSE(HistoryRepository(host.db).get_history_entries().empty());
    { const auto pause = host.store.gate().paused(0ms); }
    dictate(history, data.path() / "client-recordings", "e2");
    CHECK(host.upload(laptop, "dictation", "e2",
                      bundle_of(*mine, "e2", data.path() / "b2"))["stored"] == true);
    CHECK_FALSE(fs::exists(data.path() / "host" / "remote_records" / "staging" / laptop.id /
                           "dictation" / "e1"));
}

TEST_CASE("test_host_retention_follows_the_devices_limits", "[test_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    auto client_db = Database::open_or_create(data.path() / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client-recordings", client_db);
    const auto send = [&](std::string id, std::string when) {
        HistoryEntry fields;
        fields.timestamp = std::move(when);
        dictate(history, data.path() / "client-recordings", id, fields);
        auto bundle = bundle_of(*mine, id, data.path() / ("bundle-" + id));
        auto record = manifest_of(bundle);
        // RecordSync adds this computer's limits to each record it sends.
        record["retention"] = {{"max_recordings", 1}, {"max_bytes", nullptr}};
        bundle["record.json"] = record.dump();
        REQUIRE(host.upload(laptop, "dictation", id, bundle)["stored"] == true);
    };
    send("first", "2026-10-06T10:00:00+00:00");
    send("second", "2026-10-06T10:00:02+00:00");
    HistoryRepository kept(host.db);
    CHECK_FALSE(kept.get_history_entry_by_id("first")->audio_file);
    CHECK(kept.get_history_entry_by_id("second")->audio_file);
    CHECK(kept.get_history_entry_by_id("first")->text == "Hello from the laptop.");
}

TEST_CASE("test_a_meeting_record_with_traversal_is_refused", "[test_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    const Json record{{"kind", "meeting"},
                      {"format", 1},
                      {"session", {{"id", "m_0123456789ab"}}},
                      {"files", {{"../../evil.wav", 4}}}};
    const auto body = record.dump();
    const Json key{{"kind", "meeting"}, {"record_id", "m_0123456789ab"}};
    auto begin = key;
    begin.update(Json{{"files", 1}, {"bytes", body.size()}});
    host.call("records_begin", laptop, begin);
    auto put = key;
    put.update(Json{{"name", "record.json"},
                    {"size", body.size()},
                    {"sha256", sha256_hex(body)},
                    {"offset", 0}});
    host.call("records_put", laptop, put, body);
    CHECK_THROWS(host.call("records_commit", laptop, key));
    put.update(
        Json{{"name", "../x.wav"}, {"size", 1}, {"sha256", std::string(64, '0')}, {"offset", 0}});
    CHECK_THROWS_MATCHES(host.call("records_put", laptop, put, "x"), py::ValueError,
                         MessageMatches(ContainsSubstring("Invalid file name")));
    meeting_state::SqlMeetingRepository meetings(host.db, host.clock);
    CHECK_FALSE(meetings.get_meeting("m_0123456789ab"));
    CHECK_FALSE(fs::exists(data.path() / "host" / "evil.wav"));
    CHECK_FALSE(fs::exists(data.path() / "evil.wav"));
}

namespace {

// A finished meeting (the store's own test's manifest) and a dictation this device shared.
void share_both(Host& host, const fs::path& scratch, const remote::PairedDevice& device) {
    const Json meeting{
        {"kind", "meeting"},
        {"format", 1},
        {"session",
         {{"id", "m_0123456789ab"},
          {"title", "Planning"},
          {"status", "ended"},
          {"started_at", "2026-02-01T10:00:00Z"}}},
        {"files", Json::object()},
    };
    fs::create_directories(scratch);
    host.meetings->import_record("m_0123456789ab", scratch, meeting, device);
    auto client_db = Database::open_or_create(made(scratch) / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(scratch / "client-recordings", client_db);
    dictate(history, scratch / "client-recordings", "e1");
    REQUIRE(host.upload(device, "dictation", "e1",
                        bundle_of(*mine, "e1", scratch / "bundle"))["stored"] == true);
}

} // namespace

TEST_CASE("test_removing_a_device_can_delete_what_it_stored", "[test_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    share_both(host, data.path() / "client", laptop);
    CHECK(host.store.summary(laptop)["meeting"]["count"] == 1);
    CHECK(host.store.delete_device(laptop.id) == 2);
    CHECK_FALSE(
        meeting_state::SqlMeetingRepository(host.db, host.clock).get_meeting("m_0123456789ab"));
    CHECK_FALSE(HistoryRepository(host.db).get_history_entry_by_id("e1"));
}

TEST_CASE("test_renaming_a_device_rebadges_what_it_stored", "[test_remote_records]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    share_both(host, data.path() / "client", laptop);
    CHECK(host.store.rename_device(laptop.id, "Kitchen laptop") == 2);
    CHECK(HistoryRepository(host.db).get_history_entry_by_id("e1")->origin_device_name ==
          "Kitchen laptop");
    CHECK(meeting_state::SqlMeetingRepository(host.db, host.clock)
              .get_meeting("m_0123456789ab")
              ->at("origin_device_name") == "Kitchen laptop");
    CHECK(host.store.rename_device("someone-else", "Studio") == 0);

    // What it stores from now on carries the new name too: the host hands the store the
    // device as its registry now names it.
    auto renamed = laptop;
    renamed.name = "Kitchen laptop";
    auto client_db = Database::open_or_create(data.path() / "later.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "later-recordings", client_db);
    HistoryEntry later;
    later.text = "after the rename";
    dictate(history, data.path() / "later-recordings", "e2", later);
    REQUIRE(host.upload(renamed, "dictation", "e2",
                        bundle_of(*mine, "e2", data.path() / "b2"))["stored"] == true);
    CHECK(HistoryRepository(host.db).get_history_entry_by_id("e2")->origin_device_name ==
          "Kitchen laptop");
}

TEST_CASE("test_backup_drains_active_host_mutation_and_releases_gate_after_failure",
          "[test_host_backup_recovery]") {
    // The host's gate (HostRecordStore::handle holds a request for each record operation;
    // RemoteEngineService's backup context holds the pause).
    RecordGate gate;
    std::promise<void> entered;
    std::promise<void> finish;
    auto finished = finish.get_future().share();
    std::vector<std::string> calls;
    std::mutex calls_mutex;
    auto incoming = std::async(std::launch::async, [&] {
        const auto request = gate.request();
        entered.set_value();
        finished.wait();
        std::lock_guard lock(calls_mutex);
        calls.emplace_back("committed");
    });
    entered.get_future().wait();
    std::atomic<bool> backing_up{false};
    std::vector<std::string> seen_by_backup;
    bool busy_inside = false;
    // Assertions stay on the test's thread: the backup only notes what it saw.
    auto snapshot = std::async(std::launch::async, [&] {
        const auto pause = gate.paused(5s);
        {
            std::lock_guard lock(calls_mutex);
            seen_by_backup = calls;
        }
        backing_up = true;
        try {
            const auto request = gate.request();
        } catch (const RecordsBusy&) {
            busy_inside = true;
        }
        throw std::runtime_error("disk full");
    });
    // The backup waits for the commit in progress, and turns new requests away meanwhile.
    bool refused = false;
    for (int attempt = 0; attempt < 200 && !refused; ++attempt) {
        try {
            const auto request = gate.request();
            std::this_thread::sleep_for(5ms);
        } catch (const RecordsBusy&) {
            refused = true;
        }
    }
    CHECK(refused);
    CHECK_FALSE(backing_up);
    finish.set_value();
    incoming.get();
    CHECK_THROWS_WITH(snapshot.get(), "disk full");
    CHECK(backing_up);
    CHECK(seen_by_backup == std::vector<std::string>{"committed"});
    CHECK(busy_inside);
    const auto after = gate.request();
}

TEST_CASE("the host keeps a gibibyte free and says how much a record needs",
          "[remote_records][A17-46]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    host.free_space = (1ULL << 30) + (1ULL << 20);
    const Json begin{
        {"kind", "dictation"}, {"record_id", "e1"}, {"files", 2}, {"bytes", 3ULL << 20}};
    CHECK_THROWS_WITH(host.call("records_begin", laptop, begin),
                      "Not enough free space on the host to keep this record (3 MB needed).");
    host.free_space = (1ULL << 30) + (4ULL << 20);
    CHECK(host.call("records_begin", laptop, begin)["received"].empty());
}

TEST_CASE("stale uploads go after 7 days and exports after 15 minutes",
          "[remote_records][A17-46]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    const auto staging =
        data.path() / "host" / "remote_records" / "staging" / laptop.id / "dictation";
    const auto age = [](const fs::path& folder, auto by) {
        fs::last_write_time(folder, fs::file_time_type::clock::now() - by);
    };
    const auto begin = [&](std::string id) {
        host.call("records_begin", laptop,
                  {{"kind", "dictation"}, {"record_id", id}, {"files", 1}, {"bytes", 10}});
    };
    begin("old");
    begin("recent");
    age(staging / "old", std::chrono::hours(24 * 8));
    age(staging / "recent", std::chrono::hours(24 * 6));
    begin("next");
    CHECK_FALSE(fs::exists(staging / "old"));
    CHECK(fs::exists(staging / "recent"));
    CHECK(fs::exists(staging / "next"));

    auto client_db = Database::open_or_create(made(data.path() / "client") / "client.db");
    HistoryRepository history(client_db);
    const auto mine = dictation_records(data.path() / "client" / "recordings", client_db);
    for (const auto* id : {"e1", "e2", "e3"}) {
        dictate(history, data.path() / "client" / "recordings", id);
        REQUIRE(host.upload(laptop, "dictation", id,
                            bundle_of(*mine, id, data.path() / id))["stored"] == true);
    }
    const auto exports =
        data.path() / "host" / "remote_records" / "exports" / laptop.id / "dictation";
    const auto open = [&](std::string id) {
        host.call("records_open", laptop, {{"kind", "dictation"}, {"record_id", id}});
    };
    open("e1");
    open("e2");
    age(exports / "e1", 16min);
    age(exports / "e2", 14min);
    open("e3");
    CHECK_FALSE(fs::exists(exports / "e1"));
    CHECK(fs::exists(exports / "e2"));
    CHECK(fs::exists(exports / "e3"));
}

TEST_CASE("a listing holds 100 records unless asked for 1 to 500", "[remote_records][A17-46]") {
    testkit::TempDataRoot data;
    Host host(data.path() / "host");
    HistoryRepository kept(host.db);
    for (int index = 0; index < 520; ++index) {
        kept.put_history_entry(
            {.id = std::format("e{:03}", index),
             .text = "kept",
             .timestamp = std::format("2026-10-06T10:{:02}:{:02}+00:00", index / 60, index % 60),
             .model = "parakeet",
             .origin_device_id = laptop.id,
             .origin_device_name = laptop.name});
    }
    const auto listed = [&](Json limit) {
        Json fields{{"kind", "dictation"}};
        if (!limit.is_null()) {
            fields["limit"] = limit;
        }
        return host.call("records_list", laptop, fields)["records"].size();
    };
    CHECK(listed(nullptr) == 100);
    CHECK(listed("lots") == 100);
    CHECK(listed(0) == 1);
    CHECK(listed(-5) == 1);
    CHECK(listed(7) == 7);
    CHECK(listed(1000) == 500);
}
