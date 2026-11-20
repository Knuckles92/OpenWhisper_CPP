#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "meeting_agent/interfaces.h"
#include "meeting_intel/recall.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"
#include "pyport/errors.h"

// meeting/reinsight.py StoreToolHost: the AgentToolHost over one meeting's state store, which
// the live meeting and every headless re-run hand their agent, so validation behaves the same in
// both. Writes go through the store as the agent (which still cannot emit agent-forbidden ops);
// reads are the consent-gated past-meeting and knowledge-folder searches.
// Affinity: thread-safe, as the store and the repository are.
namespace fv::meeting_intel {

// reinsight.py AgentUnavailable (a RuntimeError there): no agent core could be created for a
// headless pass. Raised by the re-run (M8.10); what() is the reason.
class AgentUnavailable : public py::PyError {
public:
    explicit AgentUnavailable(const std::string& message)
        : py::PyError("AgentUnavailable", message) {}
};

class StoreToolHost final : public meeting_agent::AgentToolHost {
public:
    // `get_store` returns the meeting's current store (null while it has none: every write is
    // then refused "inactive"); `writes_allowed` returns false once agent writes are revoked
    // ("agent_writes_revoked"), so a late or canceled agent cannot change durable state (empty:
    // always allowed). `repository` answers evidence checks; `recall` (may be null: recall is
    // then "not available") answers past-meeting searches; `settings` gates both searches. The
    // repository and `recall` must outlive the host.
    StoreToolHost(std::function<std::shared_ptr<meeting_state::MeetingStateStore>()> get_store,
                  meeting_state::MeetingRepository& repository,
                  std::function<bool()> writes_allowed, RecallDeps* recall,
                  std::function<Json()> settings);

    std::vector<meeting_state::OpResult> apply_agent_ops(const Json& ops) override;
    // False without a store, and when the repository fails (logged by type).
    bool segment_exists(std::string_view segment_id) override;
    meeting_state::OpResult ask_question(std::string_view text, const Json& evidence) override;
    meeting_state::OpResult resolve_question(std::string_view question_id, std::string_view answer,
                                             double confidence, const Json& evidence) override;
    Json search_past_meetings(std::string_view query, std::string_view meeting_id,
                              const Json& limit) override;
    Json search_context_files(std::string_view query, std::string_view relative_path,
                              const Json& limit) override;

    // How many ops the store actually applied for the agent so far.
    int applied() const { return applied_.load(); }

private:
    std::shared_ptr<meeting_state::MeetingStateStore> current_store() const;
    std::string meeting_id() const;
    // Why an agent write must not land now, with the store it would land on.
    std::optional<std::string>
    refusal(std::shared_ptr<meeting_state::MeetingStateStore>& store) const;
    meeting_state::OpResult apply_single(Json op);
    std::vector<meeting_state::OpResult> record(std::vector<meeting_state::OpResult> results);

    std::function<std::shared_ptr<meeting_state::MeetingStateStore>()> get_store_;
    meeting_state::MeetingRepository& repository_;
    std::function<bool()> writes_allowed_;
    RecallDeps* recall_;
    std::function<Json()> settings_;
    std::atomic<int> applied_{0};
};

} // namespace fv::meeting_intel
