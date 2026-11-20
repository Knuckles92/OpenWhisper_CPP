#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "meeting_state/store.h"
#include "meeting_state/types.h"

namespace fv::meeting_web {

// What an engine throws when it cannot do what a dashboard asked (the archive's AI insights
// toggle); the route answers 409 with what().
class EngineRefusal : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The engine surface the dashboard serves (a30's dependency on a24): the running meeting's
// engine, or the archive adapter over a past meeting. Calls that block (store, repository)
// come from the web server's worker threads, never from the reactor.
// Affinity: implementations are thread-safe.
class DashboardEngine {
public:
    virtual ~DashboardEngine() = default;

    // The current meeting's id; empty when there is none.
    virtual std::string meeting_id() const = 0;
    // The meeting's state store; null before the meeting starts. It can change while the
    // server runs (a store created after the server started).
    virtual std::shared_ptr<meeting_state::MeetingStateStore> store() const = 0;
    virtual bool is_active() const = 0;
    // One client op as `actor` ("host" or "user") on behalf of `participant_id`.
    virtual std::vector<meeting_state::OpResult>
    apply_client_action(std::string_view actor, const std::optional<std::string>& participant_id,
                        const Json& op) = 0;
    // A host undo of event `seq`.
    virtual std::vector<meeting_state::OpResult>
    undo(std::int64_t seq, const std::optional<std::string>& participant_id) = 0;
    // Adds a guest participant for a joining dashboard client and returns it (the participant
    // object, with its "id"). Throws when the meeting cannot take one.
    virtual Json add_guest(std::string_view name) = 0;
    virtual void end() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    // Writes a new token pair, closes the sockets of the old one, and returns
    // {host_url, guest_url}.
    virtual Json regenerate_tokens() = 0;
    // Agent activity ticks, oldest first; an engine without an agent has none.
    virtual Json recent_agent_activity() const { return Json::array(); }
    // The meetings folder the dashboard's delete removes a meeting's spool from.
    virtual std::filesystem::path spool_root() const = 0;
    // The AI insights switch of the dashboard (/api/meeting/cloud). Without the meeting
    // intelligence there is nothing to switch, so the default does nothing; an engine that
    // cannot take it throws EngineRefusal.
    virtual void set_cloud_enabled(bool enabled) { (void)enabled; }
    // Queues a note change on the active, approved insights session.
    virtual void request_note_adjustment(std::string, std::function<void(Json)>) {
        throw EngineRefusal("Turn on AI insights in an active meeting first.");
    }
};

} // namespace fv::meeting_web
