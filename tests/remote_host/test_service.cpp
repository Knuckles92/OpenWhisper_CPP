#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <future>
#include <iterator>

#include "base/atomic_file.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "host_rig.h"
#include "meeting_state/sql_repository.h"
#include "remote/client.h"
#include "remote/errors.h"
#include "remote/meeting_route.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/activity.h"
#include "remote_host/service.h"
#include "remote_records/dictation_records.h"
#include "remote_records/meeting_records.h"
#include "remote_records/sync.h"
#include "settings/keys.h"

using namespace fv;
using namespace fv::remote_host;
using namespace std::chrono_literals;

namespace {

class RejectingCredentialBackend final : public MemoryCredentialBackend {
public:
    bool reject_write = false;

    void set(std::string_view service, std::string_view name, std::string_view value) override {
        if (reject_write) {
            throw CredentialStoreError("Credentials unavailable.");
        }
        MemoryCredentialBackend::set(service, name, value);
    }
};

MapEnvReader private_env(const std::filesystem::path& root) {
    return MapEnvReader{
        {std::string(identity().env_prefix) + "DATA_DIR", path_to_utf8(root)},
        {"LOCALAPPDATA", path_to_utf8(root / "local")},
        {"XDG_DATA_HOME", path_to_utf8(root / "local")},
        {"HOME", path_to_utf8(root / "home")},
        {"USERPROFILE", path_to_utf8(root / "home")},
    };
}

struct ServiceRig {
    MapEnvReader env;
    Paths paths;
    SettingsStore settings{paths.user_file(identity().settings_file)};
    std::shared_ptr<RejectingCredentialBackend> backend =
        std::make_shared<RejectingCredentialBackend>();
    CredentialStore credentials{backend, identity()};
    std::shared_ptr<remote::testing::FakeTailscaleCli> tailscale =
        std::make_shared<remote::testing::FakeTailscaleCli>();
    std::shared_ptr<RemoteEngineService> service;

    // `listen`: 0 for an ephemeral port; nullopt for the saved remote_host_port.
    ServiceRig(const std::filesystem::path& root, std::string name, net::Reactor& reactor,
               Clock& clock, Executor& io,
               std::shared_ptr<HostEngine> engine = unavailable_engine("No speech engine."),
               std::shared_ptr<remote_records::HostRecordStore> records = {},
               std::optional<int> listen = 0)
        : env(private_env(root)), paths(Paths::resolve(identity(), env)),
          service(std::make_shared<RemoteEngineService>(
              RemoteEngineServiceDeps{.paths = paths,
                                      .identity = identity(),
                                      .settings = settings,
                                      .credentials = credentials,
                                      .env = env,
                                      .clock = clock,
                                      .io = io,
                                      .client_env = {reactor},
                                      .computer_name = std::move(name),
                                      .tailscale = tailscale,
                                      .engine = [engine = std::move(engine)] { return engine; },
                                      .records = std::move(records),
                                      .bind = "127.0.0.1",
                                      .discovery_port = std::nullopt,
                                      .listen_port = listen})) {}
};

} // namespace

TEST_CASE("saved remote host settings start sharing and pair another service") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);

    host.settings.save_setting(keys::remote_host_enabled.key(), true);
    host.service->apply_host_settings();
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    REQUIRE_FALSE(state.fingerprint.empty());
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto saved =
        client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false);
    CHECK(saved.host_name == "Desk");
    CHECK(saved.fingerprint == state.fingerprint);
    CHECK(client.service->client_pairing() == saved);
    REQUIRE(host.service->host_state().devices.size() == 1);

    {
        auto connection = client.service->client_connection(saved);
        CHECK(connection->connect()["type"] == "ready");
    }

    const auto renamed = client.service->rename_host("Office");
    REQUIRE(renamed);
    CHECK(renamed->host_name == "Office");
    CHECK(host.service->remove_device(saved.device_id, false));
    CHECK_THROWS_AS(client.service->client_connection()->connect(), remote::RemoteEngineError);
    client.service->forget_host();
    CHECK_FALSE(client.service->client_pairing());
    CHECK_THROWS_AS(client.service->client_connection(), remote::RemoteEngineError);

    host.service->set_host_enabled(false);
    CHECK_FALSE(host.service->host_state().running);
}

TEST_CASE("credential retry saves a previously approved remote pairing without another code") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);

    host.service->set_host_enabled(true);
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    client.backend->reject_write = true;
    CHECK_THROWS_AS(
        client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false),
        CredentialStoreError);
    CHECK_FALSE(client.service->client_pairing());
    CHECK(client.service->pending_pairing_name() == "Desk");

    host.service->set_host_enabled(false);
    client.backend->reject_write = false;
    CHECK(client.service->retry_pairing_save().host_name == "Desk");
    CHECK(client.service->client_pairing().has_value());
    CHECK(client.service->pending_pairing_name().empty());
}

// from tests/test_host_mode.py::test_the_service_reads_activity_and_the_engine_while_not_sharing
TEST_CASE("test_the_service_reads_activity_and_the_engine_while_not_sharing") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io, nullptr);
    CHECK(host.service->host_activity() == empty_snapshot());
    const auto engine = host.service->engine_state();
    CHECK(engine.at("available") == false);
    CHECK(engine.at("status").get<std::string>().contains("No transcription engine"));
}

TEST_CASE("the remote service queues one background status refresh") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);

    CHECK_FALSE(host.service->tailscale_status());
    CHECK_FALSE(host.service->tailscale_status(true));
    CHECK(io.queued() == 1);
    CHECK(io.run_all() == 1);
    const auto status = host.service->tailscale_status();
    REQUIRE(status);
    CHECK(status->state == "not_installed");
    CHECK(io.queued() == 0);
}

TEST_CASE(
    "live meeting audio reaches the pinned model and stops when the host preference changes") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto engine = std::make_shared<remote_host::testing::FakeHostEngine>();
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io, engine);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto pairing = client.service->pair(
        remote::format_address("127.0.0.1", host.service->host_state().port), *code, false);
    client.settings.update(Json{{"meeting_speech_separate", true},
                                {"meeting_asr_source", "remote"},
                                {"fv_meeting_remote_fingerprint", pairing.fingerprint}});
    auto route = std::make_shared<Json>(remote::meeting_route(*client.settings.snapshot()));
    speech::SpeechService speech(
        [service = client.service, route](const speech::BackendConfig&) {
            return service->make_meeting_backend(route);
        },
        io);
    speech.select(speech::Slot::Meeting, {.backend = "remote"});
    REQUIRE(rig::wait_for([&] {
        return speech.readiness(speech::Slot::Meeting).state == speech::ReadyState::Ready;
    }));
    CHECK(route->at("model") == "parakeet-v3");
    CHECK(route->at("family") == "parakeet");
    speech::DecodeJob job;
    job.slot = speech::Slot::Meeting;
    job.lane = speech::Lane::Meeting;
    job.input = speech::AudioClip{
        .samples = std::make_shared<const std::vector<float>>(16000, 0.25f), .end = 16000};
    CHECK(speech.run(job).transcript.text == "heard 16000");
    client.settings.update(Json{{"meeting_asr_source", "local"}});
    CHECK_THROWS_AS(speech.run(job), EngineError);
    CHECK(engine->calls().size() == 1);
}

TEST_CASE("the remote speech backend decodes windows through the saved paired service") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto engine = std::make_shared<remote_host::testing::FakeHostEngine>();
    engine->set_accepts_phrases(true);
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io, engine);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false);

    speech::SpeechService speech({}, io);
    speech.set_backend_factory("remote", [client = client.service](const speech::BackendConfig&) {
        return client->make_backend();
    });
    speech.select(speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(rig::wait_for([&] {
        return speech.readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready;
    }));

    speech::DecodeJob job;
    constexpr std::size_t sample_count = 45 * 16000;
    job.input = speech::AudioClip{
        .samples = std::make_shared<const std::vector<float>>(sample_count, 0.25f),
        .end = sample_count,
    };
    job.options.language = "fr";
    job.phrases = {"FioriVoce"};
    const auto decoded = speech.run(job);
    const auto calls = engine->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].audio.size() + calls[1].audio.size() == sample_count);
    CHECK(calls[0].language == "fr");
    CHECK(calls[1].phrases == job.phrases);
    CHECK(decoded.transcript.text == "heard " + std::to_string(calls[0].audio.size()) + " heard " +
                                         std::to_string(calls[1].audio.size()));
    REQUIRE(decoded.transcript.segments.size() == 2);
    CHECK(decoded.transcript.segments[1].start == decoded.transcript.segments[0].end);

    job.input = speech::AudioClip{
        .samples = std::make_shared<const std::vector<float>>(16000, 0.0f),
        .end = 16000,
    };
    CHECK(speech.run(job).transcript.text.empty());
    CHECK(engine->calls().size() == 2);
}

namespace {

class GatedEngine final : public HostEngine {
public:
    explicit GatedEngine(std::shared_future<void> release) : release_(std::move(release)) {}
    std::vector<std::string> identity() const override { return engine_.identity(); }
    Json describe() const override { return engine_.describe(); }

    Json transcribe(std::span<const float> audio, std::optional<std::string> language,
                    std::span<const std::string> phrases) override {
        if (++calls == 2) {
            on_second.set_value();
            release_.wait_for(4s);
        }
        return engine_.transcribe(audio, std::move(language), phrases);
    }

    std::promise<void> on_second;
    std::atomic<int> calls{0};

private:
    std::shared_future<void> release_;
    remote_host::testing::FakeHostEngine engine_;
};

} // namespace

TEST_CASE("canceling the second remote speech window keeps the connection for the next job") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    std::promise<void> release;
    auto engine = std::make_shared<GatedEngine>(release.get_future().share());
    auto second = engine->on_second.get_future();
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io, engine);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false);

    speech::SpeechService speech({}, io);
    speech.set_backend_factory("remote", [client = client.service](const speech::BackendConfig&) {
        return client->make_backend();
    });
    speech.select(speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(rig::wait_for([&] {
        return speech.readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready;
    }));

    speech::DecodeJob job;
    constexpr std::size_t sample_count = 45 * 16000;
    job.input = speech::AudioClip{
        .samples = std::make_shared<const std::vector<float>>(sample_count, 0.25f),
        .end = sample_count,
    };
    using Result = std::expected<speech::DecodeResult, EngineError>;
    auto result = std::make_shared<std::promise<Result>>();
    auto finished = result->get_future();
    auto handle =
        speech.submit(job, io, [result](Result reply) { result->set_value(std::move(reply)); });
    REQUIRE(second.wait_for(4s) == std::future_status::ready);
    handle.cancel();
    const auto cancellation = finished.wait_for(500ms);
    release.set_value();
    REQUIRE(cancellation == std::future_status::ready);
    const auto stopped = finished.get();
    REQUIRE_FALSE(stopped);
    CHECK(std::string_view(stopped.error().what()) == "Transcription canceled");

    job.input = speech::AudioClip{
        .samples = std::make_shared<const std::vector<float>>(160, 0.25f),
        .end = 160,
    };
    CHECK(speech.run(job).transcript.text == "heard 160");
    CHECK(engine->calls == 3);
    CHECK(host.service->connected_clients().size() == 1);
}

TEST_CASE("manual dictation outbox moves only after explicit request and preserves recovery") {
    testkit::TempDataRoot folder;
    const auto root = folder.path();
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor host_io;
    ManualExecutor client_io;
    auto host_db = Database::open_or_create(root / "host.db");
    auto client_db = Database::open_or_create(root / "client.db");
    auto stored = std::make_shared<remote_records::HostRecordStore>(
        root / "host-transfers",
        remote_records::HostRecordStore::Kinds{
            {"dictation", remote_records::dictation_records(root / "host-audio", host_db)}},
        clock, [](const std::filesystem::path&) { return 8ULL << 30; });
    ServiceRig host(root / "host", "Desk", reactor, clock, host_io,
                    unavailable_engine("Synthetic records only."), stored);
    ServiceRig client(root / "client", "Laptop", reactor, clock, client_io);
    host.service->set_host_enabled(true);
    host.service->set_keep_records(true);
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto paired =
        client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false);

    auto sync = std::make_shared<remote_records::RecordSync>(remote_records::SyncDeps{
        .db = client_db,
        .settings = client.settings,
        .clock = clock,
        .io = client_io,
        .transfer_root = root / "client-transfers",
        .recordings = root / "client-audio",
        .pairing = [service = client.service] { return service->client_pairing(); },
        .connect =
            [service = client.service](const remote::ClientPairing& peer) {
                return service->client_connection(peer);
            },
    });
    HistoryRepository local(client_db);
    HistoryRepository remote_history(host_db);
    std::filesystem::create_directories(root / "client-audio");
    std::string riff("RIFF");
    riff.append(4, char{0});
    riff += "WAVE";
    riff.append(4 * remote::RECORD_CHUNK_BYTES, char{0});
    write_file_atomic(root / "client-audio" / "synthetic.wav", riff);
    local.put_history_entry({
        .id = "record-1",
        .text = "synthetic dictation",
        .timestamp = "2026-01-01T00:00:00+00:00",
        .model = "parakeet",
        .audio_file = "synthetic.wav",
    });
    sync->set_location("host");
    sync->record_saved("dictation", "record-1");
    sync->wake();
    client_io.run_all();
    CHECK_FALSE(remote_history.get_history_entry_by_id("record-1"));

    try {
        const auto pause = sync->paused_for_backup();
        CHECK_THROWS_AS(sync->send_existing(), remote_records::RecordsBusy);
        throw std::runtime_error("synthetic backup failure");
    } catch (const std::runtime_error&) {
    }

    CHECK(sync->send_existing() == 1);
    client_io.run_all();
    CAPTURE(sync->status().error);
    const auto held = remote_history.get_history_entry_by_id("record-1");
    REQUIRE(held);
    CHECK(held->origin_device_id == paired.device_id);
    CHECK_FALSE(local.get_history_entry_by_id("record-1"));
    const auto listing = sync->list_remote("dictation");
    REQUIRE(listing.size() == 1);
    CHECK(listing[0]["stored_on"] == "Desk");
    const auto received = [&] {
        return sync->audio_for("record-1");
    };
    auto first = std::async(std::launch::async, received);
    auto second = std::async(std::launch::async, received);
    const auto audio = first.get();
    CHECK(second.get() == audio);
    CHECK(std::filesystem::file_size(audio) == riff.size());
    CHECK(std::distance(std::filesystem::directory_iterator(audio.parent_path()),
                        std::filesystem::directory_iterator()) == 1);

    sync->set_location("local");
    CHECK(sync->bring_back() == 1);
    CHECK_FALSE(remote_history.get_history_entry_by_id("record-1"));
    const auto restored = local.get_history_entry_by_id("record-1");
    REQUIRE(restored);
    CHECK(restored->text == "synthetic dictation");
    CHECK_FALSE(restored->origin_device_id);

    sync->set_location("both");
    CHECK(sync->send_existing() == 1);
    client_io.run_all();
    auto edit = *restored;
    edit.text = "kept local edit";
    local.put_history_entry(edit);
    CHECK(sync->send_existing() == 1);
    client_io.run_all();
    CHECK(sync->status().error.find("different version") != std::string::npos);
    // The conflicting record stays on the host and the run goes on (sync.py _bring_back).
    CHECK(sync->bring_back() == 0);
    CHECK(sync->status().error.find("Both copies were kept") != std::string::npos);
    REQUIRE(local.get_history_entry_by_id("record-1"));
    CHECK(local.get_history_entry_by_id("record-1")->text == "kept local edit");
    REQUIRE(remote_history.get_history_entry_by_id("record-1"));
    CHECK(remote_history.get_history_entry_by_id("record-1")->text == "synthetic dictation");
    sync.reset();
    CHECK_FALSE(std::filesystem::exists(audio.parent_path()));
}

TEST_CASE("Manual meeting preview reads a paired host manifest without fetching recordings") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto db = Database::open_or_create(folder.path() / "records.db");
    auto host_db = Database::open_or_create(folder.path() / "host-records.db");
    const auto meetings =
        remote_records::meeting_records(folder.path() / "host-meetings", host_db, clock);
    auto stored = std::make_shared<remote_records::HostRecordStore>(
        folder.path() / "host-transfers",
        remote_records::HostRecordStore::Kinds{{"meeting", meetings}}, clock);
    ServiceRig host(folder.path() / "host", "Synthetic desk", reactor, clock, io,
                    unavailable_engine("Synthetic records only."), stored);
    ServiceRig client(folder.path() / "client", "Synthetic laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto paired = client.service->pair(
        remote::format_address("127.0.0.1", host.service->host_state().port), *code, false);
    const auto manifest = Json{
        {"kind", "meeting"},
        {"format", 1},
        {"session",
         {{"id", "meeting-1"},
          {"title", "Synthetic meeting"},
          {"status", "ended"},
          {"started_at", "2026-02-01T10:00:00Z"}}},
        {"segments", Json::array({
                         {{"id", "part-2"},
                          {"channel", "mic"},
                          {"start_s", 0.0},
                          {"end_s", 1.0},
                          {"text", "One"},
                          {"created_at", "2026-02-01T10:00:00Z"}},
                         {{"id", "part-1"},
                          {"channel", "mic"},
                          {"start_s", 1.0},
                          {"end_s", 2.0},
                          {"text", "Two"},
                          {"created_at", "2026-02-01T10:00:01Z"}},
                     })},
        {"files", Json::object()},
    };
    (void)meetings->import_record("meeting-1", folder.path() / "incoming", manifest,
                                  {.id = paired.device_id, .name = "Synthetic laptop"});
    const auto sync = std::make_shared<remote_records::RecordSync>(remote_records::SyncDeps{
        .db = db,
        .settings = client.settings,
        .clock = clock,
        .io = io,
        .transfer_root = folder.path() / "client-transfers",
        .recordings = folder.path() / "client-audio",
        .meetings = folder.path() / "client-meetings",
        .pairing = [service = client.service] { return service->client_pairing(); },
        .connect =
            [service = client.service](const remote::ClientPairing& peer) {
                return service->client_connection(peer);
            },
    });
    CHECK_THROWS_AS(sync->list_held_meetings("different host", 20), remote::RecordsUnavailable);
    const auto records = sync->list_held_meetings(paired.fingerprint, 20);
    REQUIRE(records.size() == 1);
    CHECK(records[0].at("title") == "Synthetic meeting");
    const auto preview = sync->read_held_meeting(paired.fingerprint, "meeting-1");
    CHECK(preview.at("transcript") == "One\nTwo");
    CHECK_FALSE(std::filesystem::exists(folder.path() / "client-transfers"));
    CHECK_FALSE(std::filesystem::exists(folder.path() / "client-audio"));

    // This is a separate, explicitly selected local copy. No recovery or startup sends it.
    host.service->set_keep_records(true);
    meeting_state::SqlMeetingRepository local(db, clock);
    meeting_state::SqlMeetingRepository kept(host_db, clock);
    CHECK_THROWS_AS(sync->check_out_from("meeting", "meeting-1", "another host", {}),
                    remote::RecordsUnavailable);
    CHECK_FALSE(local.get_meeting("meeting-1"));
    sync->check_out_from("meeting", "meeting-1", paired.fingerprint, {});
    const auto copied_local = local.get_meeting("meeting-1");
    const auto original = kept.get_meeting("meeting-1");
    REQUIRE(copied_local);
    REQUIRE(original);
    CHECK(copied_local->at("origin_device_id").is_null());
    CHECK(copied_local->at("host_token") != original->at("host_token"));
    CHECK(kept.get_meeting("meeting-1").has_value());
    const auto revision = records.front().at("revision").get<std::string>();
    REQUIRE_FALSE(revision.empty());
    CHECK_THROWS_AS(sync->delete_remote_version("meeting", "meeting-1", "different host", revision),
                    remote::RecordsUnavailable);
    CHECK_THROWS_AS(
        sync->delete_remote_version("meeting", "meeting-1", paired.fingerprint, "stale-version"),
        remote::RemoteRequestError);
    CHECK(kept.get_meeting("meeting-1").has_value());
    sync->delete_remote_version("meeting", "meeting-1", paired.fingerprint, revision);
    CHECK_FALSE(kept.get_meeting("meeting-1"));
    CHECK(local.get_meeting("meeting-1").has_value());

    auto replacement = manifest;
    replacement["session"]["title"] = "A different version";
    (void)meetings->import_record("meeting-1", folder.path() / "incoming", replacement,
                                  {.id = paired.device_id, .name = "Synthetic laptop"});
    CHECK_THROWS_AS(
        sync->delete_remote_version("meeting", "meeting-1", paired.fingerprint, revision),
        remote::RemoteRequestError);
    REQUIRE(kept.get_meeting("meeting-1"));
    CHECK(kept.get_meeting("meeting-1")->at("title") == "A different version");
    CHECK(local.get_meeting("meeting-1")->at("title") == "Synthetic meeting");
    const auto spool = folder.path() / "client-meetings" / "local";
    std::filesystem::create_directories(spool);
    std::string wav("RIFF");
    wav.append(4, char{0});
    wav += "WAVE";
    write_file_atomic(spool / "meeting_session.wav", wav);
    write_file_atomic(spool / "unreferenced.wav", wav);
    local.create_meeting({{"id", "local-ready"},
                          {"title", "Local agenda"},
                          {"status", "ended"},
                          {"started_at", "2026-02-01T10:00:00Z"},
                          {"host_token", "local-token"},
                          {"guest_token", "local-guest"},
                          {"spool_dir", path_to_utf8(spool)}});
    CHECK(sync->shareable_meetings(20).size() == 2);
    CHECK_FALSE(kept.get_meeting("local-ready"));
    CHECK_THROWS_AS(sync->share_meeting("another host", "local-ready", false, {}),
                    remote::RecordsUnavailable);
    CancelSource canceled;
    canceled.cancel();
    CHECK_THROWS_AS(sync->share_meeting(paired.fingerprint, "local-ready", false, canceled.token()),
                    remote::RecordsUnavailable);
    CHECK_FALSE(kept.get_meeting("local-ready"));

    sync->share_meeting(paired.fingerprint, "local-ready", false, {});
    const auto copied = kept.get_meeting("local-ready");
    REQUIRE(copied);
    CHECK(copied->at("origin_device_id") == paired.device_id);
    const auto retained = path_from_utf8(copied->at("spool_dir").get<std::string>());
    CHECK_FALSE(std::filesystem::exists(retained / "meeting_session.wav"));
    CHECK(std::filesystem::exists(spool / "meeting_session.wav"));
    REQUIRE(local.get_meeting("local-ready"));

    local.create_meeting({{"id", "local-audio"},
                          {"title", "Audio meeting"},
                          {"status", "ended"},
                          {"started_at", "2026-02-01T10:00:00Z"},
                          {"host_token", "local-token"},
                          {"guest_token", "local-guest"},
                          {"spool_dir", path_to_utf8(spool)}});
    sync->share_meeting(paired.fingerprint, "local-audio", true, {});
    const auto with_audio = kept.get_meeting("local-audio");
    REQUIRE(with_audio);
    const auto kept_audio = path_from_utf8(with_audio->at("spool_dir").get<std::string>());
    CHECK(std::filesystem::file_size(kept_audio / "meeting_session.wav") == wav.size());
    CHECK_FALSE(std::filesystem::exists(kept_audio / "unreferenced.wav"));
}

// ---- tests/test_remote_engine.py: the service ----

TEST_CASE("test_service_reports_a_busy_port") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    rig::HostRig blocker; // something else already listens on the port
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io,
                    unavailable_engine("No speech engine."), {}, std::nullopt);
    host.settings.save_setting(keys::remote_host_port.key(), static_cast<int>(blocker.port));
    host.service->set_host_enabled(true);
    const auto state = host.service->host_state();
    CHECK_FALSE(state.running);
    CHECK(state.enabled);
    CHECK(state.port == blocker.port);
    CHECK(state.error.starts_with(
        std::format("Couldn't listen on port {} (", static_cast<int>(blocker.port))));
    CHECK(state.error.ends_with("). Another program may be using it; choose a different port."));
    host.service->set_host_enabled(false);
    CHECK(host.service->host_state().error.empty());
}

TEST_CASE("test_a_service_without_a_switch_offers_no_models") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    // No engine control, model cache or components: nothing here can switch models.
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io,
                    std::make_shared<remote_host::testing::FakeHostEngine>());
    CHECK(host.service->host_models().empty());
    host.service->set_host_enabled(true);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto pairing = client.service->pair(
        remote::format_address("127.0.0.1", host.service->host_state().port), *code, false);
    auto connection = client.service->client_connection(pairing);
    const Json ready = connection->connect();
    CHECK(ready.at("models") == Json::array());
    CHECK_THROWS_WITH(
        connection->request("select_model", Json{{"family", "parakeet"}, {"model", "parakeet-v3"}}),
        Catch::Matchers::ContainsSubstring("doesn't let paired computers"));
}

TEST_CASE("test_service_trusts_the_tailscale_owner_only_while_the_setting_is_on") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    net::TailscaleStatus tailnet;
    tailnet.state = "running";
    tailnet.name = "devbox";
    tailnet.address = "100.64.0.1";
    tailnet.owner = "owner@example.com";
    host.tailscale->set_status(tailnet);
    host.service->set_host_enabled(true);
    const auto port = host.service->host_state().port;
    remote::ClientEnv env{reactor};
    // What a host says about pairing over Tailscale is whether it has an owner to trust.
    CHECK(remote::probe_host(env, "127.0.0.1", port).tailscale_pairing);
    host.service->set_tailscale_trust(false);
    CHECK_FALSE(host.settings.get(keys::remote_host_tailscale_trust));
    CHECK_FALSE(remote::probe_host(env, "127.0.0.1", port).tailscale_pairing);
    host.service->set_tailscale_trust(true);
    CHECK(remote::probe_host(env, "127.0.0.1", port).tailscale_pairing);
    // Its tailnet address is among the addresses it tells a paired computer to fall back on.
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto pairing =
        client.service->pair(remote::format_address("127.0.0.1", port), *code, false);
    CHECK(std::ranges::find(pairing.alternates, "100.64.0.1") != pairing.alternates.end());
}

TEST_CASE("a model chosen through the service rides on the next dictation load") {
    // The service's half of RemoteRuntime.select_remote_model: the choice is handed to the
    // backend the next reload makes, which switches the host before it adopts its engine.
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    std::mutex mutex;
    std::vector<std::string> chosen;
    rig::HostRig* self = nullptr;
    rig::HostRig host([&](SpeechHostDeps& deps) {
        deps.models = [] {
            return Json::array({Json{
                {"family", "parakeet"}, {"model", "orukeet-v0.1"}, {"label", "Orukeet TDT 0.6B"}}});
        };
        deps.select_model = [&](std::string family, std::string model, std::string device_name,
                                std::optional<std::string>) {
            {
                std::lock_guard lock(mutex);
                chosen.push_back(family + "|" + model + "|" + device_name);
            }
            if (model == "busy") {
                throw std::runtime_error(
                    "devbox is transcribing right now. Try again in a moment.");
            }
            self->engine->set_model(model);
            return self->engine->describe();
        };
    });
    self = &host;
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    client.service->pair(remote::format_address("127.0.0.1", host.port), host.host->open_pairing(),
                         false);
    speech::SpeechService speech({}, io);
    speech.set_backend_factory("remote", [service = client.service](const speech::BackendConfig&) {
        return service->make_backend();
    });
    // A (re)load of the dictation slot, and its answer.
    const auto reload = [&] {
        const auto before = speech.generation(speech::Slot::Dictation);
        speech.select(speech::Slot::Dictation, {.backend = "remote"});
        REQUIRE(rig::wait_for([&] {
            return speech.generation(speech::Slot::Dictation) > before &&
                   speech.readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready;
        }));
        return speech.readiness(speech::Slot::Dictation);
    };
    const auto switches = [&] {
        std::lock_guard lock(mutex);
        return chosen;
    };
    CHECK(reload().message.empty());
    CHECK(switches().empty());

    CHECK(client.service->request_model("parakeet", "orukeet-v0.1").model == "orukeet-v0.1");
    CHECK(reload().message.empty());
    CHECK(switches() == std::vector<std::string>{"parakeet|orukeet-v0.1|Laptop"});
    CHECK(host.engine->describe().at("model") == "orukeet-v0.1");
    // The choice rides on one load only.
    (void)reload();
    CHECK(switches().size() == 1);

    // A host that keeps its model says why, with the ready line of that load.
    (void)client.service->request_model("parakeet", "busy");
    CHECK(reload().message == "devbox is transcribing right now. Try again in a moment.");
    CHECK(switches().size() == 2);
}

// ---- tests/test_remote_registry.py ----

namespace {

// A device paired with a host that has since quit: what a service finds saved before it has
// started sharing.
remote::ClientPairing leave_a_paired_device(const std::filesystem::path& root,
                                            net::Reactor& reactor, Clock& clock, Executor& io) {
    ServiceRig earlier(root / "host", "Desk", reactor, clock, io);
    ServiceRig client(root / "client", "Fixture laptop", reactor, clock, io);
    earlier.service->set_host_enabled(true);
    const auto code = earlier.service->open_pairing();
    REQUIRE(code);
    auto pairing = client.service->pair(
        remote::format_address("127.0.0.1", earlier.service->host_state().port), *code, false);
    earlier.service->set_host_enabled(false);
    return pairing;
}

std::vector<std::string> ids(const std::vector<remote::PairedDevice>& devices) {
    std::vector<std::string> out;
    for (const auto& device : devices) {
        out.push_back(device.id);
    }
    return out;
}

std::vector<std::string> ids(const std::vector<remote::ClientHistoryStatus>& clients) {
    std::vector<std::string> out;
    for (const auto& client : clients) {
        out.push_back(client.device_id);
    }
    return out;
}

} // namespace

TEST_CASE("test_history_and_host_share_registry_before_and_after_host_creation") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    const auto first = leave_a_paired_device(folder.path(), reactor, clock, io);
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
    // Before any host exists, the history broker and host_state read the same devices.
    CHECK_FALSE(host.service->host_state().running);
    CHECK(ids(host.service->host_state().devices) == std::vector<std::string>{first.device_id});
    CHECK(ids(host.service->history().clients()) == std::vector<std::string>{first.device_id});
    // The host, once created, pairs into that same registry.
    host.service->set_host_enabled(true);
    REQUIRE(host.service->host_state().running);
    ServiceRig second(folder.path() / "second", "Fixture desktop", reactor, clock, io);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto added = second.service->pair(
        remote::format_address("127.0.0.1", host.service->host_state().port), *code, false);
    const std::vector<std::string> both{first.device_id, added.device_id};
    CHECK(ids(host.service->host_state().devices) == both);
    CHECK(ids(host.service->history().clients()) == both);
}

TEST_CASE("test_service_removal_uses_history_registry") {
    for (const bool create_host : {false, true}) {
        CAPTURE(create_host);
        testkit::TempDataRoot folder;
        net::Reactor reactor;
        FakeClock clock;
        rig::InlineExecutor io;
        const auto paired = leave_a_paired_device(folder.path(), reactor, clock, io);
        ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
        std::vector<std::string> events;
        const auto watching = host.service->changed.connect(
            io, [&](std::string event) { events.push_back(std::move(event)); });
        if (create_host) {
            host.service->set_host_enabled(true);
            REQUIRE(host.service->host_state().running);
        }
        REQUIRE(host.service->history().clients().size() == 1);
        CHECK(host.service->remove_device(paired.device_id, false));
        CHECK(host.service->history().clients().empty());
        CHECK(host.service->host_state().devices.empty());
        CHECK(std::ranges::find(events, "devices") != events.end());
        // Gone for good: a second removal finds nothing.
        CHECK_FALSE(host.service->remove_device(paired.device_id, false));
    }
}

// ---- tests/test_remote_keychain_recovery.py ----

TEST_CASE("test_forgetting_a_host_discards_an_unsaved_pairing") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
    ServiceRig client(folder.path() / "client", "Laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    client.backend->reject_write = true;
    CHECK_THROWS_AS(
        client.service->pair(remote::format_address("127.0.0.1", host.service->host_state().port),
                             *code, false),
        CredentialStoreError);
    CHECK(client.service->pending_pairing_name() == "Desk");
    client.service->forget_host();
    client.backend->reject_write = false;
    CHECK_THROWS_WITH(client.service->retry_pairing_save(),
                      Catch::Matchers::ContainsSubstring("Connect to a computer"));
    CHECK(client.service->pending_pairing_name().empty());
    CHECK_FALSE(client.service->client_pairing());
}

// service.py pairing_grants: what a newly paired computer may do here beyond transcribing. The
// records phrase says "finished meetings": the host keeps only ended or failed meetings
// (a17-retention). The MCP grant is left out (a17-paired-mcp).
TEST_CASE("a pairing request names what the computer may keep here") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto host_db = Database::open_or_create(folder.path() / "host-records.db");
    auto stored = std::make_shared<remote_records::HostRecordStore>(
        folder.path() / "host-transfers",
        remote_records::HostRecordStore::Kinds{
            {"dictation",
             remote_records::dictation_records(folder.path() / "host-audio", host_db)}},
        clock, [](const std::filesystem::path&) { return 8ULL << 30; });
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io,
                    unavailable_engine("Synthetic records only."), stored);
    host.service->set_keep_records(false);
    CHECK(host.service->pairing_grants().empty());
    host.service->set_keep_records(true);
    CHECK(host.service->pairing_grants() ==
          std::vector<std::string>{"keep its dictations and finished meetings here"});
}

// service.py prepare_update: with speech sharing off there is no host to hold.
TEST_CASE("an update check without speech sharing says sharing is off") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    ServiceRig host(folder.path() / "host", "Desk", reactor, clock, io);
    CHECK(host.service->prepare_update() == Json{{"reason", "Speech sharing is off"}});
}
