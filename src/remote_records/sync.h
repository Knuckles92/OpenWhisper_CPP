#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "history/calendar_source.h"
#include "history/repository.h"
#include "remote/connection.h"
#include "remote/record_sync_control.h"
#include "remote_records/host_store.h"
#include "remote_records/outbox.h"

namespace fv::remote_records {

struct SyncDeps {
    Database& db;
    SettingsStore& settings;
    Clock& clock;
    Executor& io;
    std::filesystem::path transfer_root;
    std::filesystem::path recordings;
    std::filesystem::path meetings;
    std::function<std::optional<remote::ClientPairing>()> pairing;
    std::function<std::unique_ptr<remote::RemoteConnection>(const remote::ClientPairing&)> connect;
};

// User-initiated dictation transfers. The outbox survives restarts and stays pinned to the
// saved peer. Recorder callbacks and startup do not send transcript data automatically.
class RecordSync final : public remote::RecordSyncControl,
                         public history::RemoteRecordSource,
                         public std::enable_shared_from_this<RecordSync> {
public:
    // How long list_remote answers "<host> isn't reachable right now." without asking again.
    static constexpr std::chrono::seconds kUnreachableQuiet{30};

    explicit RecordSync(SyncDeps deps);
    ~RecordSync() override;
    void shutdown();

    std::string location() override;
    void set_location(std::string_view location) override;
    remote::SyncStatus status() override;
    bool manual_transfers() const override { return true; }
    void refresh_summary() override;
    int send_existing() override;
    int bring_back(std::function<void(std::string)> progress = {}) override;
    void wake(bool now = false) override;
    void record_saved(std::string_view, std::string_view) override {}
    void record_edited(std::string_view, std::string_view) override {}
    void record_deleted(std::string_view, std::string_view) override {}
    void cleared(std::string_view) override {}
    void cleared(std::string_view, std::span<const std::string>) override {}

    std::vector<Json> list_remote(std::string_view kind, std::string_view query = "",
                                  int limit = 100) override;
    std::vector<Json> shareable_meetings(int limit);
    void share_meeting(std::string_view expected_host, std::string_view id, bool include_audio,
                       const CancelToken& cancel);
    std::vector<Json> list_held_meetings(std::string_view expected_host, int limit);
    Json read_held_meeting(std::string_view expected_host, std::string_view id);
    std::optional<Json> remote_item(std::string_view kind, std::string_view id) override;
    bool delete_remote(std::string_view kind, std::string_view id) override;
    void check_out_from(std::string_view kind, std::string_view id, std::string_view expected_host,
                        std::function<void(std::int64_t, std::int64_t)> progress) override;
    void delete_remote_version(std::string_view kind, std::string_view id,
                               std::string_view expected_host, std::string_view revision) override;
    bool delete_remote_from(std::string_view kind, std::string_view id,
                            std::string_view expected_host) override;
    std::filesystem::path audio_for(std::string_view id) override;
    void check_out(std::string_view kind, std::string_view id,
                   std::function<void(std::int64_t, std::int64_t)> progress = {}) override;
    std::set<std::string> copies_on_host(std::string_view kind,
                                         std::span<const std::string> ids) override;
    std::vector<std::string> copies_on_host(std::string_view kind,
                                            const std::vector<std::string>& ids) override;
    bool listing_wanted(std::string_view kind) override;
    std::string host_name() override;
    HistoryEntry remote_history_entry(const Json& item) override;
    remote::SyncPause paused_for_backup() override;

private:
    remote::ClientPairing require_pairing();
    void require_same_peer(const remote::ClientPairing& expected);
    std::unique_ptr<remote::RemoteConnection> open(const remote::ClientPairing& expected,
                                                   bool upload = false,
                                                   const CancelToken& cancel = {});
    void observe(const remote::ClientPairing& peer, const Json& ready);
    void transfer();
    Json upload(remote::RemoteConnection&, const remote::ClientPairing&, const OutboxItem&,
                const std::filesystem::path& directory, const Files& files,
                const CancelToken& cancel);
    Json fetch(remote::RemoteConnection&, std::string_view id,
               const std::filesystem::path& directory,
               const std::function<void(std::int64_t, std::int64_t)>& progress,
               std::string_view kind = "dictation");
    std::vector<Json> list(remote::RemoteConnection&, const remote::ClientPairing&,
                           std::string_view query, int limit, std::string_view kind = "dictation");
    void restore_local(std::string_view id, const std::filesystem::path& directory,
                       const Json& record);
    void problem(std::string text);

    SyncDeps deps_;
    Outbox outbox_;
    HistoryRepository entries_;
    std::shared_ptr<RecordKind> adapter_;
    CancelSource stopping_;
    RecordGate gate_;
    std::mutex transfers_;
    std::mutex state_mutex_;
    bool running_ = false;
    bool requested_again_ = false;
    std::string observed_host_;
    remote::SyncStatus state_;
    std::map<std::string, Json, std::less<>> remote_items_;
    // When list_remote last failed to reach the host; it stays quiet for kUnreachableQuiet.
    std::optional<std::chrono::steady_clock::time_point> unreachable_since_;
    const std::filesystem::path audio_cache_dir_;
    std::map<std::string, std::filesystem::path, std::less<>> audio_;
};

} // namespace fv::remote_records
