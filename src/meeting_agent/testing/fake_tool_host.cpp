#include "meeting_agent/testing/fake_tool_host.h"

#include <utility>

namespace fv::meeting_agent::testing {

using meeting_state::OpResult;

OpResult FakeToolHost::next_result(const Json& op) {
    if (!scripted_.empty()) {
        OpResult result = std::move(scripted_.front());
        scripted_.pop_front();
        return result;
    }
    OpResult result;
    result.ok = true;
    result.op = op;
    return result;
}

std::vector<OpResult> FakeToolHost::apply_agent_ops(const Json& ops) {
    std::function<std::vector<OpResult>(const Json&)> handler;
    {
        std::lock_guard lock(mutex_);
        batches_.push_back(ops);
        if (ops.is_array()) {
            for (const auto& op : ops) {
                ops_.push_back(op);
            }
        }
        handler = on_apply_;
    }
    if (handler) {
        return handler(ops);
    }
    std::lock_guard lock(mutex_);
    std::vector<OpResult> results;
    if (ops.is_array()) {
        for (const auto& op : ops) {
            results.push_back(next_result(op));
        }
    }
    return results;
}

bool FakeToolHost::segment_exists(std::string_view segment_id) {
    std::lock_guard lock(mutex_);
    segment_checks_.emplace_back(segment_id);
    return known_segments_ && known_segments_->contains(segment_id);
}

OpResult FakeToolHost::ask_question(std::string_view text, const Json& evidence) {
    std::lock_guard lock(mutex_);
    questions_.push_back({"ask_question", std::string(text), "", 0.0, evidence});
    return next_result(Json{{"op", "ask_question"}});
}

OpResult FakeToolHost::resolve_question(std::string_view question_id, std::string_view answer,
                                        double confidence, const Json& evidence) {
    std::lock_guard lock(mutex_);
    questions_.push_back(
        {"resolve_question", std::string(answer), std::string(question_id), confidence, evidence});
    return next_result(Json{{"op", "resolve_question"}});
}

Json FakeToolHost::search_past_meetings(std::string_view query, std::string_view meeting_id,
                                        const Json& limit) {
    std::lock_guard lock(mutex_);
    searches_.push_back(
        {"search_past_meetings", std::string(query), std::string(meeting_id), limit});
    return past_answer_.value_or(Json());
}

Json FakeToolHost::search_context_files(std::string_view query, std::string_view relative_path,
                                        const Json& limit) {
    std::lock_guard lock(mutex_);
    searches_.push_back(
        {"search_context_files", std::string(query), std::string(relative_path), limit});
    return folder_answer_.value_or(Json());
}

bool FakeToolHost::can_check_segments() const {
    std::lock_guard lock(mutex_);
    return known_segments_.has_value();
}

bool FakeToolHost::can_search_past_meetings() const {
    std::lock_guard lock(mutex_);
    return past_answer_.has_value();
}

bool FakeToolHost::can_search_context_files() const {
    std::lock_guard lock(mutex_);
    return folder_answer_.has_value();
}

void FakeToolHost::set_known_segments(std::set<std::string, std::less<>> ids) {
    std::lock_guard lock(mutex_);
    known_segments_ = std::move(ids);
}

void FakeToolHost::set_past_meetings_answer(Json answer) {
    std::lock_guard lock(mutex_);
    past_answer_.emplace(std::move(answer));
}

void FakeToolHost::set_context_files_answer(Json answer) {
    std::lock_guard lock(mutex_);
    folder_answer_.emplace(std::move(answer));
}

void FakeToolHost::script_results(std::vector<OpResult> results) {
    std::lock_guard lock(mutex_);
    for (auto& result : results) {
        scripted_.push_back(std::move(result));
    }
}

void FakeToolHost::on_apply(std::function<std::vector<OpResult>(const Json& ops)> handler) {
    std::lock_guard lock(mutex_);
    on_apply_ = std::move(handler);
}

std::vector<Json> FakeToolHost::ops() const {
    std::lock_guard lock(mutex_);
    return ops_;
}

std::vector<Json> FakeToolHost::batches() const {
    std::lock_guard lock(mutex_);
    return batches_;
}

std::vector<FakeToolHost::QuestionCall> FakeToolHost::question_calls() const {
    std::lock_guard lock(mutex_);
    return questions_;
}

std::vector<FakeToolHost::SearchCall> FakeToolHost::search_calls() const {
    std::lock_guard lock(mutex_);
    return searches_;
}

std::vector<std::string> FakeToolHost::segment_checks() const {
    std::lock_guard lock(mutex_);
    return segment_checks_;
}

} // namespace fv::meeting_agent::testing
