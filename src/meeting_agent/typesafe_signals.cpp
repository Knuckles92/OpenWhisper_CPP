#include "meeting_agent/typesafe_signals.h"

#include <cstdint>

#include "pyport/text.h"

namespace fv::meeting_agent {

std::optional<double> topic_shift_probability(llm::Judge& judge, std::string_view previous_window,
                                              std::string_view window, const CancelToken& cancel) {
    std::string older = py::strip(previous_window);
    std::string newer = py::strip(window);
    if (cancel.cancelled() || older.empty() || newer.empty()) {
        return std::nullopt;
    }
    const auto max = static_cast<std::int64_t>(TOPIC_WINDOW_MAX_CHARS);
    if (py::len(older) > TOPIC_WINDOW_MAX_CHARS) {
        older = py::slice(older, -max, std::nullopt);
    }
    if (py::len(newer) > TOPIC_WINDOW_MAX_CHARS) {
        newer = py::slice(newer, std::nullopt, max);
    }
    Json state = Json::object();
    state["previous_window"] = std::move(older);
    state["window"] = std::move(newer);
    return judge.noul(state, Json(std::string(TOPIC_CHANGED_INSTRUCTIONS)), Json(), TOPIC_TIMEOUT_S,
                      cancel);
}

const Json& voice_command_criteria() {
    static const Json criteria = [] {
        Json out = Json::object();
        out["none"] = "Ordinary meeting speech between participants, including talking about "
                      "notes, decisions or topics without instructing the assistant.";
        out["mark_decision"] =
            "Instructs the note-taking assistant to record the preceding point as a decision.";
        out["mark_action"] = "Instructs the assistant to record an action item or to-do, possibly "
                             "with an owner or deadline.";
        out["note_this"] =
            "Instructs the assistant to write down or capture the preceding point in the notes.";
        out["recap"] =
            "Asks the assistant to summarize, read back, or recap decisions, actions or progress.";
        out["fix_transcript"] = "Tells the assistant that a word, name or number in the transcript "
                                "is wrong and gives the correction.";
        out["set_topic"] =
            "Tells the assistant that the meeting is moving to a new topic or section.";
        return out;
    }();
    return criteria;
}

std::optional<llm::ChoiceAnswer>
voice_command_choice(llm::Judge& judge, std::string_view segment,
                     const std::vector<std::string>& previous,
                     const std::vector<std::string>& assistant_names, const CancelToken& cancel) {
    std::string text = py::strip(segment);
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> context;
    for (const auto& line : previous) {
        if (!line.empty()) {
            context.push_back(line);
        }
    }
    Json previous_segments = Json::array();
    const std::size_t first = context.size() > 3 ? context.size() - 3 : 0;
    for (std::size_t i = first; i < context.size(); ++i) {
        previous_segments.push_back(context[i]);
    }
    Json names = Json::array();
    for (const auto& name : assistant_names) {
        names.push_back(name);
    }
    Json state = Json::object();
    state["previous_segments"] = std::move(previous_segments);
    state["segment"] = std::move(text);
    state["assistant_names"] = std::move(names);
    return judge.choice(state, Json(std::string(VOICE_COMMAND_INSTRUCTIONS)),
                        voice_command_criteria(), cancel);
}

} // namespace fv::meeting_agent
