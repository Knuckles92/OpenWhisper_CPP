#pragma once

// The tool host of tools/parity/cases/meeting.agent.tool_policy.py: built from the same spec,
// it records every call in one list, in Python's shapes, so a vector's "calls" compare as they
// are.

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "meeting_agent/interfaces.h"
#include "meeting_state/types.h"

namespace fv::meeting_agent_test {

using fv::Json;
using fv::meeting_state::OpResult;

// OpResult(**fields).
inline OpResult op_result_of(const Json& fields) {
    OpResult result;
    result.ok = fields.value("ok", false);
    result.op = fields.contains("op") ? fields["op"] : Json();
    const auto text = [&](const char* key) -> std::optional<std::string> {
        if (fields.contains(key) && fields[key].is_string()) {
            return fields[key].get<std::string>();
        }
        return std::nullopt;
    };
    const auto integer = [&](const char* key) -> std::optional<std::int64_t> {
        if (fields.contains(key) && fields[key].is_number_integer()) {
            return fields[key].get<std::int64_t>();
        }
        return std::nullopt;
    };
    result.reason = text("reason");
    result.target_id = text("target_id");
    result.current_revision = integer("current_revision");
    result.seq = integer("seq");
    if (fields.contains("effect")) {
        result.effect = fields["effect"];
    }
    if (fields.contains("inverse")) {
        result.inverse = fields["inverse"];
    }
    return result;
}

// [ok, op, reason, target_id, current_revision, seq].
inline Json result_row(const OpResult& result) {
    const auto optional = [](const auto& value) {
        return value ? Json(*value) : Json();
    };
    return Json::array({result.ok, result.op, optional(result.reason), optional(result.target_id),
                        optional(result.current_revision), optional(result.seq)});
}

class SpecHost final : public meeting_agent::AgentToolHost {
public:
    // null is the bare host: the required methods only.
    explicit SpecHost(const Json& spec) {
        if (spec.is_null()) {
            return;
        }
        if (spec.contains("segments") && spec["segments"].is_array()) {
            segments_.emplace(spec["segments"]);
        }
        if (spec.contains("past")) {
            past_.emplace(spec["past"]);
        }
        if (spec.contains("folder")) {
            folder_.emplace(spec["folder"]);
        }
        if (spec.contains("results") && spec["results"].is_array()) {
            for (const auto& fields : spec["results"]) {
                results_.push_back(op_result_of(fields));
            }
        }
    }

    std::vector<OpResult> apply_agent_ops(const Json& ops) override {
        std::lock_guard lock(mutex_);
        calls_.push_back(Json::array({"apply_agent_ops", ops}));
        std::vector<OpResult> out;
        for (const auto& op : ops) {
            out.push_back(next(op));
        }
        return out;
    }

    bool segment_exists(std::string_view segment_id) override {
        std::lock_guard lock(mutex_);
        calls_.push_back(Json::array({"segment_exists", std::string(segment_id)}));
        for (const auto& known : *segments_) {
            if (known.is_string() && known.get<std::string>() == segment_id) {
                return true;
            }
        }
        return false;
    }

    OpResult ask_question(std::string_view text, const Json& evidence) override {
        std::lock_guard lock(mutex_);
        calls_.push_back(Json::array({"ask_question", std::string(text), evidence}));
        return next(Json{{"op", "ask_question"}});
    }

    OpResult resolve_question(std::string_view question_id, std::string_view answer,
                              double confidence, const Json& evidence) override {
        std::lock_guard lock(mutex_);
        calls_.push_back(Json::array({"resolve_question", std::string(question_id),
                                      std::string(answer), confidence, evidence}));
        return next(Json{{"op", "resolve_question"}});
    }

    Json search_past_meetings(std::string_view query, std::string_view meeting_id,
                              const Json& limit) override {
        std::lock_guard lock(mutex_);
        calls_.push_back(
            Json::array({"search_past_meetings", std::string(query), target(meeting_id), limit}));
        return *past_;
    }

    Json search_context_files(std::string_view query, std::string_view relative_path,
                              const Json& limit) override {
        std::lock_guard lock(mutex_);
        calls_.push_back(Json::array(
            {"search_context_files", std::string(query), target(relative_path), limit}));
        return *folder_;
    }

    bool can_check_segments() const override { return segments_.has_value(); }
    bool can_search_past_meetings() const override { return past_.has_value(); }
    bool can_search_context_files() const override { return folder_.has_value(); }

    Json calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    static Json target(std::string_view value) {
        return value.empty() ? Json() : Json(std::string(value));
    }

    OpResult next(const Json& op) {
        if (!results_.empty()) {
            OpResult result = std::move(results_.front());
            results_.pop_front();
            return result;
        }
        OpResult result;
        result.ok = true;
        result.op = op;
        return result;
    }

    mutable std::mutex mutex_;
    std::optional<Json> segments_;
    std::optional<Json> past_;
    std::optional<Json> folder_;
    std::deque<OpResult> results_;
    Json calls_ = Json::array();
};

} // namespace fv::meeting_agent_test
