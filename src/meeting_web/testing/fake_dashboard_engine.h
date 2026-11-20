#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "meeting_web/dashboard_engine.h"

namespace fv::meeting_web::testing {

// A DashboardEngine over a real MeetingStateStore, recording every call: client actions and
// undo go to the store, add_guest applies upsert_participant as the meeting engine does (the
// name stripped and cut to 80 characters, "Guest" when blank; py::ValueError with the
// rejection reason, or "guest_rejected"), end, pause, resume and set_cloud_enabled record and
// run the lifecycle hook (nothing else unless one is set). Each behaviour can
// be replaced, for the Python tests whose fake engines answer with canned results.
// Affinity: thread-safe.
class FakeDashboardEngine final : public DashboardEngine {
public:
    struct Call {
        std::string method; // "apply_client_action", "undo", "add_guest", "end", ...
        Json args;          // the call's arguments as a list
    };

    using ApplyFn = std::function<std::vector<meeting_state::OpResult>(
        std::string_view actor, const std::optional<std::string>& participant_id, const Json& op)>;
    using UndoFn = std::function<std::vector<meeting_state::OpResult>(
        std::int64_t seq, const std::optional<std::string>& participant_id)>;
    using AddGuestFn = std::function<Json(std::string_view name)>;
    // Runs after end, pause, resume and set_cloud_enabled are recorded, with the method and its
    // arguments; it may throw (EngineRefusal) or take time (a slow end).
    using LifecycleHook = std::function<void(std::string_view method, const Json& args)>;

    explicit FakeDashboardEngine(std::string meeting_id = {},
                                 std::shared_ptr<meeting_state::MeetingStateStore> store = nullptr,
                                 std::filesystem::path spool_root = {});

    std::string meeting_id() const override;
    std::shared_ptr<meeting_state::MeetingStateStore> store() const override;
    bool is_active() const override;
    std::vector<meeting_state::OpResult>
    apply_client_action(std::string_view actor, const std::optional<std::string>& participant_id,
                        const Json& op) override;
    std::vector<meeting_state::OpResult>
    undo(std::int64_t seq, const std::optional<std::string>& participant_id) override;
    Json add_guest(std::string_view name) override;
    void end() override;
    void pause() override;
    void resume() override;
    Json regenerate_tokens() override;
    Json recent_agent_activity() const override;
    std::filesystem::path spool_root() const override;
    void set_cloud_enabled(bool enabled) override;
    using NoteRequestFn = std::function<void(std::string, std::function<void(Json)>)>;
    void request_note_adjustment(std::string text, std::function<void(Json)> complete) override;
    void set_note_request(NoteRequestFn handler);

    void set_meeting_id(std::string meeting_id);
    void set_store(std::shared_ptr<meeting_state::MeetingStateStore> store);
    void set_active(bool active);
    void set_activity(Json ticks);
    void set_apply(ApplyFn apply);
    void set_undo(UndoFn undo);
    void set_add_guest(AddGuestFn add_guest);
    // What regenerate_tokens does and answers; {"host_url": "", "guest_url": ""} until set.
    void set_regenerate(std::function<Json()> regenerate);
    void set_lifecycle_hook(LifecycleHook hook);

    std::vector<Call> calls() const;
    std::vector<Call> calls(std::string_view method) const;

private:
    void record(std::string method, Json args);
    void run_hook(std::string_view method, const Json& args);

    mutable std::mutex mutex_;
    std::string meeting_id_;
    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::filesystem::path spool_root_;
    bool active_ = true;
    Json activity_ = Json::array();
    ApplyFn apply_;
    UndoFn undo_;
    AddGuestFn add_guest_;
    std::function<Json()> regenerate_;
    LifecycleHook hook_;
    NoteRequestFn note_request_;
    std::vector<Call> calls_;
};

} // namespace fv::meeting_web::testing
