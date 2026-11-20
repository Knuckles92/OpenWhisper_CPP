#include "meeting_intel/store_tool_host.h"

#include <exception>
#include <utility>

#include "base/log.h"
#include "meeting_intel/context_folder.h"
#include "meeting_intel/detail/text.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace {

constexpr log::Logger logger("meeting.reinsight");

const std::optional<std::string> kAgentActor("agent");

// list(evidence or []): a list as it is, a text as its characters, a dict as its keys.
Json evidence_list(const Json& evidence) {
    Json out = Json::array();
    if (!detail::truthy(evidence)) {
        return out;
    }
    if (evidence.is_array()) {
        return evidence;
    }
    if (evidence.is_string()) {
        const std::string& text = evidence.get_ref<const std::string&>();
        const auto count = static_cast<std::int64_t>(py::len(text));
        for (std::int64_t i = 0; i < count; ++i) {
            out.push_back(py::slice(text, i, i + 1));
        }
        return out;
    }
    if (evidence.is_object()) {
        for (const auto& [key, value] : evidence.items()) {
            out.push_back(key);
        }
        return out;
    }
    throw py::TypeError("evidence is not iterable");
}

} // namespace

StoreToolHost::StoreToolHost(
    std::function<std::shared_ptr<meeting_state::MeetingStateStore>()> get_store,
    meeting_state::MeetingRepository& repository, std::function<bool()> writes_allowed,
    RecallDeps* recall, std::function<Json()> settings)
    : get_store_(std::move(get_store)), repository_(repository),
      writes_allowed_(std::move(writes_allowed)), recall_(recall), settings_(std::move(settings)) {}

std::shared_ptr<meeting_state::MeetingStateStore> StoreToolHost::current_store() const {
    return get_store_ ? get_store_() : nullptr;
}

std::string StoreToolHost::meeting_id() const {
    const auto store = current_store();
    return store ? store->meeting_id() : std::string();
}

std::optional<std::string>
StoreToolHost::refusal(std::shared_ptr<meeting_state::MeetingStateStore>& store) const {
    store = current_store();
    if (!store) {
        return "inactive";
    }
    if (writes_allowed_ && !writes_allowed_()) {
        return "agent_writes_revoked";
    }
    return std::nullopt;
}

std::vector<meeting_state::OpResult>
StoreToolHost::record(std::vector<meeting_state::OpResult> results) {
    int applied = 0;
    for (const auto& result : results) {
        applied += result.ok ? 1 : 0;
    }
    applied_ += applied;
    return results;
}

std::vector<meeting_state::OpResult> StoreToolHost::apply_agent_ops(const Json& ops) {
    const Json batch = ops.is_array() ? ops : Json::array();
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    if (const auto reason = refusal(store)) {
        std::vector<meeting_state::OpResult> refused;
        for (const auto& op : batch) {
            meeting_state::OpResult result;
            result.ok = false;
            result.op = op.is_object() ? op : Json{{"op", op}};
            result.reason = *reason;
            refused.push_back(std::move(result));
        }
        return refused;
    }
    return record(store->apply("agent", kAgentActor, batch));
}

bool StoreToolHost::segment_exists(std::string_view segment_id) {
    const std::string id = meeting_id();
    if (id.empty()) {
        return false;
    }
    try {
        return repository_.segment_exists(id, segment_id);
    } catch (const std::exception& error) {
        logger.error("Segment existence probe failed ({})", detail::type_name(error));
        return false;
    }
}

meeting_state::OpResult StoreToolHost::apply_single(Json op) {
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    if (const auto reason = refusal(store)) {
        meeting_state::OpResult result;
        result.ok = false;
        result.op = std::move(op);
        result.reason = *reason;
        return result;
    }
    return record(store->apply("agent", kAgentActor, Json::array({op}))).at(0);
}

meeting_state::OpResult StoreToolHost::ask_question(std::string_view text, const Json& evidence) {
    return apply_single(
        Json{{"op", "ask_question"}, {"text", text}, {"evidence", evidence_list(evidence)}});
}

meeting_state::OpResult StoreToolHost::resolve_question(std::string_view question_id,
                                                        std::string_view answer, double confidence,
                                                        const Json& evidence) {
    return apply_single(Json{{"op", "resolve_question"},
                             {"question_id", question_id},
                             {"answer_text", answer},
                             {"confidence", confidence},
                             {"evidence", evidence_list(evidence)}});
}

Json StoreToolHost::search_past_meetings(std::string_view query, std::string_view meeting_id,
                                         const Json& limit) {
    if (writes_allowed_ && !writes_allowed_()) {
        return {{"ok", false},
                {"disabled", true},
                {"hits", Json::array()},
                {"text", "This meeting agent no longer has access to meeting sources."}};
    }
    // Without a recall source this is Python's recall over no repository.
    RecallDeps none{nullptr, settings_, nullptr, nullptr};
    return meeting_intel::search_past_meetings(recall_ != nullptr ? *recall_ : none, query,
                                               this->meeting_id(), meeting_id, limit);
}

Json StoreToolHost::search_context_files(std::string_view query, std::string_view relative_path,
                                         const Json& limit) {
    if (writes_allowed_ && !writes_allowed_()) {
        return {{"ok", false},
                {"disabled", true},
                {"hits", Json::array()},
                {"text", "This meeting agent no longer has access to meeting sources."}};
    }
    return meeting_intel::search_context_files(settings_, query, relative_path, limit);
}

} // namespace fv::meeting_intel
