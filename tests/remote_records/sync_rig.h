#pragma once

// OpenWhisper's tests/test_remote_records.py `host` and `client` fixtures: a real host service
// sharing over TLS on 127.0.0.1 with a HostRecordStore over its own database, and this computer
// paired with it, its History (a real HistoryManager on its own database) and its RecordSync.
// The client's transfers run on `client_io`, which the test runs (`run()`).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/atomic_file.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "db/database.h"
#include "history/manager.h"
#include "history/repository.h"
#include "meeting_state/sql_repository.h"
#include "net/reactor.h"
#include "remote/client.h"
#include "remote/errors.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote/wire.h"
#include "remote_host/host_engine.h"
#include "remote_host/service.h"
#include "remote_records/dictation_records.h"
#include "remote_records/meeting_records.h"
#include "remote_records/sync.h"
#include "settings/store.h"
#include "testkit/testkit.h"

namespace fv::records_rig {

namespace fs = std::filesystem;

class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

inline fs::path made(const fs::path& folder) {
    fs::create_directories(folder);
    return folder;
}

inline MapEnvReader private_env(const fs::path& root) {
    return MapEnvReader{
        {std::string(identity().env_prefix) + "DATA_DIR", path_to_utf8(root)},
        {"LOCALAPPDATA", path_to_utf8(root / "local")},
        {"XDG_DATA_HOME", path_to_utf8(root / "local")},
        {"HOME", path_to_utf8(root / "home")},
        {"USERPROFILE", path_to_utf8(root / "home")},
    };
}

// One computer's remote engine service (tests/remote_host/test_service.cpp's ServiceRig).
struct Computer {
    Computer(const fs::path& root, std::string name, net::Reactor& reactor, Clock& clock,
             Executor& io, std::shared_ptr<remote_records::HostRecordStore> records = {})
        : env(private_env(root)), paths(Paths::resolve(identity(), env)),
          service(std::make_shared<remote_host::RemoteEngineService>(
              remote_host::RemoteEngineServiceDeps{
                  .paths = paths,
                  .identity = identity(),
                  .settings = settings,
                  .credentials = credentials,
                  .env = env,
                  .clock = clock,
                  .io = io,
                  .client_env = {reactor},
                  .computer_name = std::move(name),
                  .tailscale = tailscale,
                  .engine =
                      [] { return remote_host::unavailable_engine("Synthetic records only."); },
                  .records = std::move(records),
                  .bind = "127.0.0.1",
                  .discovery_port = std::nullopt,
                  .listen_port = 0})) {}

    MapEnvReader env;
    Paths paths;
    SettingsStore settings{paths.user_file(identity().settings_file)};
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    std::shared_ptr<remote::testing::FakeTailscaleCli> tailscale =
        std::make_shared<remote::testing::FakeTailscaleCli>();
    std::shared_ptr<remote_host::RemoteEngineService> service;
};

// The dev box ("devbox") and this laptop, paired; the laptop's History and RecordSync.
struct Paired {
    Paired()
        // The host's folder is laid out as an app data root (its database, recordings, meetings
        // and remote_records under it), so its own backup code can carry it (host_root()).
        : host_db(Database::open_or_create(made(host_root()) /
                                           path_from_utf8(identity().database_file))),
          client_db(Database::open_or_create(made(root() / "client") / "client.db")),
          host_meetings(
              remote_records::meeting_records(root() / "host" / "meetings", host_db, clock)),
          store(std::make_shared<remote_records::HostRecordStore>(
              root() / "host" / "remote_records",
              remote_records::HostRecordStore::Kinds{
                  {"dictation",
                   remote_records::dictation_records(root() / "host" / "recordings", host_db)},
                  {"meeting", host_meetings}},
              clock,
              [this](const fs::path&) {
                  if (on_begin) {
                      on_begin();
                  }
                  return 8ULL << 30;
              })),
          host(host_root(), "devbox", reactor, clock, host_io, store),
          client(root() / "client" / "service", "laptop", reactor, clock, client_io) {
        host.service->set_host_enabled(true);
        host.service->set_keep_records(true);
        const auto state = host.service->host_state();
        REQUIRE(state.running);
        const auto code = host.service->open_pairing();
        REQUIRE(code);
        paired =
            client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false);
        HistoryServices services;
        services.clock = &clock;
        services.record_sync = sync.get();
        history = std::make_unique<HistoryManager>(client_db, recordings(), RecordingRetention{},
                                                   services);
    }

    ~Paired() {
        sync->shutdown();
        history.reset();
    }

    const fs::path& root() const { return folder.path(); }
    // The host service's data root.
    fs::path host_root() const { return root() / "host"; }
    fs::path recordings() const { return root() / "client" / "recordings"; }

    // client.run_once(): the transfers wake(true) queued.
    void run() { client_io.run_all(); }

    // _dictate: a dictation saved here with its recording.
    HistoryEntry dictate(std::string text = "hello from the laptop",
                         std::optional<std::string> raw = std::nullopt,
                         std::size_t audio_bytes = 64) {
        const auto source = made(root() / "takes") / (std::to_string(++takes) + ".wav");
        std::string riff("RIFF");
        riff.append(4, char{0});
        riff += "WAVE";
        riff.append(audio_bytes, char{1});
        write_file_atomic(source, riff);
        AddEntry fields;
        fields.text = std::move(text);
        fields.raw_text = std::move(raw);
        fields.model = "parakeet (cuda)";
        fields.source_name = "Quick Record";
        fields.source_audio_path = source;
        fields.columns = {{"entry_kind", std::string("dictation")}};
        clock.advance(std::chrono::seconds(2));
        return history->add_entry(fields).value();
    }

    std::optional<HistoryEntry> on_host(std::string_view id) {
        return HistoryRepository(host_db).get_history_entry_by_id(id);
    }

    testkit::TempDataRoot folder;
    FakeClock clock;
    net::Reactor reactor;
    InlineExecutor host_io;
    ManualExecutor client_io;
    Database host_db;
    Database client_db;
    std::shared_ptr<remote_records::RecordKind> host_meetings;
    // Runs on the host as each records_begin is answered.
    std::function<void()> on_begin;
    std::shared_ptr<remote_records::HostRecordStore> store;
    Computer host;
    Computer client;
    remote::ClientPairing paired;
    // How many times the sync asked for the pairing; it answers "not paired" at the
    // `unpaired_at`-th ask.
    std::atomic<int> pairing_asks{0};
    std::atomic<int> unpaired_at{0};
    std::shared_ptr<remote_records::RecordSync> sync =
        std::make_shared<remote_records::RecordSync>(remote_records::SyncDeps{
            .db = client_db,
            .settings = client.settings,
            .clock = clock,
            .io = client_io,
            .transfer_root = root() / "client" / "remote_records",
            .recordings = recordings(),
            .meetings = root() / "client" / "meetings",
            .pairing = [this]() -> std::optional<remote::ClientPairing> {
                if (++pairing_asks == unpaired_at) {
                    return std::nullopt;
                }
                return client.service->client_pairing();
            },
            .connect =
                [this](const remote::ClientPairing& peer) {
                    return client.service->client_connection(peer);
                },
        });
    std::unique_ptr<HistoryManager> history;
    int takes = 0;
};

} // namespace fv::records_rig
