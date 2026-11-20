#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "meeting_agent/interfaces.h"

namespace fv::meeting_agent::testing {

// An AgentToolHost that records what an agent core asked for (tests/fakes/agent_core.py
// RecordingAgentTools, grown). By default it accepts every op and question, cannot check
// segment ids and has neither search tool, like the Python fake; each part can be switched on.
// Scripted results are used in order, one per op or question; when they run out the host
// answers ok with the op echoed.
// Affinity: thread-safe.
class FakeToolHost final : public AgentToolHost {
public:
    struct QuestionCall {
        std::string tool; // "ask_question" or "resolve_question"
        std::string text; // the question, or the answer
        std::string question_id;
        double confidence = 0.0;
        Json evidence;
    };
    struct SearchCall {
        std::string tool; // "search_past_meetings" or "search_context_files"
        std::string query;
        std::string target; // meeting_id or relative_path; "" for none
        Json limit;
    };

    std::vector<meeting_state::OpResult> apply_agent_ops(const Json& ops) override;
    bool segment_exists(std::string_view segment_id) override;
    meeting_state::OpResult ask_question(std::string_view text, const Json& evidence) override;
    meeting_state::OpResult resolve_question(std::string_view question_id, std::string_view answer,
                                             double confidence, const Json& evidence) override;
    Json search_past_meetings(std::string_view query, std::string_view meeting_id,
                              const Json& limit) override;
    Json search_context_files(std::string_view query, std::string_view relative_path,
                              const Json& limit) override;
    bool can_check_segments() const override;
    bool can_search_past_meetings() const override;
    bool can_search_context_files() const override;

    // Switches on segment_exists over these ids.
    void set_known_segments(std::set<std::string, std::less<>> ids);
    // Switches on a search tool with the answer it gives every call.
    void set_past_meetings_answer(Json answer);
    void set_context_files_answer(Json answer);
    // Results for the next ops and questions, consumed in order.
    void script_results(std::vector<meeting_state::OpResult> results);
    // Replaces apply_agent_ops' answer altogether (called under no lock).
    void on_apply(std::function<std::vector<meeting_state::OpResult>(const Json& ops)> handler);

    // Every op apply_agent_ops was given, flattened in call order, and the batches as given.
    std::vector<Json> ops() const;
    std::vector<Json> batches() const;
    std::vector<QuestionCall> question_calls() const;
    std::vector<SearchCall> search_calls() const;
    std::vector<std::string> segment_checks() const;

private:
    meeting_state::OpResult next_result(const Json& op);

    mutable std::mutex mutex_;
    std::optional<std::set<std::string, std::less<>>> known_segments_;
    std::optional<Json> past_answer_;
    std::optional<Json> folder_answer_;
    std::deque<meeting_state::OpResult> scripted_;
    std::function<std::vector<meeting_state::OpResult>(const Json&)> on_apply_;
    std::vector<Json> ops_;
    std::vector<Json> batches_;
    std::vector<QuestionCall> questions_;
    std::vector<SearchCall> searches_;
    std::vector<std::string> segment_checks_;
};

} // namespace fv::meeting_agent::testing
