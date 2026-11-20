#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"
#include "meeting_web/dashboard_engine.h"

// meeting/web/archive.py ArchivedMeetingDashboard: the engine surface the dashboard serves, over
// a persisted meeting, so a past meeting opens in the dashboard after a restart without capture,
// ASR, diarization or an agent. Its store is opened historical: work a restart interrupted reads
// as failed, never as still running. The live controls (end, pause, resume) do nothing, and the
// AI insights switch is refused (EngineRefusal, which the route answers 409 where OpenWhisper
// answered a bare 500). The agent options of the archive (provider, model, endpoint, agent core)
// arrive with the meeting intelligence.
// Affinity: thread-safe.
namespace fv::meeting_web {

class MeetingWebServer;

inline constexpr std::string_view kArchiveCloudRefusal =
    "AI insights are unavailable for archived playback";

class ArchivedMeetingDashboard final : public DashboardEngine {
public:
    // `meeting` is the repository row; its "id" is required (py::PyError KeyError without it).
    // `settings` is what a fresh state's report views are read from.
    ArchivedMeetingDashboard(std::shared_ptr<meeting_state::MeetingRepository> repository,
                             const Json& meeting, std::filesystem::path spool_root,
                             const Json& settings = Json::object(),
                             const Clock& clock = system_clock());

    // The server serving this archive, so link rotation can name the new links; null detaches.
    // The server must stay alive while attached.
    void attach_server(MeetingWebServer* server);
    // The current host link, "" without a running server.
    std::string host_url() const;
    bool is_running() const;
    // Stops and detaches the attached server. Not called from the server's reactor.
    void shutdown();
    // The meeting's segments after `after_start_s` (all by default).
    std::vector<Json> get_transcript(double after_start_s = -1.0,
                                     std::optional<std::int64_t> limit = std::nullopt);

    std::string meeting_id() const override;
    std::shared_ptr<meeting_state::MeetingStateStore> store() const override;
    bool is_active() const override;
    std::vector<meeting_state::OpResult>
    apply_client_action(std::string_view actor, const std::optional<std::string>& participant_id,
                        const Json& op) override;
    std::vector<meeting_state::OpResult>
    undo(std::int64_t seq, const std::optional<std::string>& participant_id) override;
    // A guest participant for a guest link opened on the archive: the name stripped and cut to
    // 80 characters, "Guest" when blank. py::ValueError with the store's reason (or
    // "guest_rejected") when the store refuses it.
    Json add_guest(std::string_view name) override;
    void end() override;
    void pause() override;
    void resume() override;
    // New host and guest tokens in the repository, every socket of the old pair closed, and the
    // new links ({host_url, guest_url}, "" without a server).
    Json regenerate_tokens() override;
    std::filesystem::path spool_root() const override;
    // Throws EngineRefusal(kArchiveCloudRefusal).
    void set_cloud_enabled(bool enabled) override;

private:
    std::shared_ptr<meeting_state::MeetingRepository> repository_;
    std::string meeting_id_;
    std::filesystem::path spool_root_;
    std::shared_ptr<meeting_state::MeetingStateStore> store_;

    mutable std::mutex mutex_;
    MeetingWebServer* server_ = nullptr;
};

} // namespace fv::meeting_web
