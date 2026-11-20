#include "meeting/finalization.h"

#include <algorithm>
#include <format>

#include "base/log.h"
#include "meeting/detail/py_value.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "speech/api_models.h"

namespace fv::meeting {

namespace {

const log::Logger logger("meeting.finalization");

struct StepText {
    std::string_view id;
    std::string_view name;
    std::string_view detail;
};

// STEP_NAMES and STEP_DETAILS. The redecode detail names OpenWhisper_CPP's engine where
// OpenWhisper's names Whisper.
constexpr std::array<StepText, 5> kSteps{{
    {"redecode", "Audio Re-transcription", "High-accuracy full session Parakeet decode"},
    {"speaker_id", "Speaker Identification", "OpenAI labels on the system-audio recording"},
    {"polish", "Transcript Cleanup", "AI grammar, punctuation, and speaker formatting"},
    {"consolidation", "Summary & Action Items",
     "Synthesizing executive summary, key points, decisions, and action items"},
    {"finalize", "State Finalization", "Saving final transcript and consolidating meeting state"},
}};

const StepText* find_step(std::string_view step_id) {
    const auto it = std::ranges::find(kSteps, step_id, &StepText::id);
    return it == kSteps.end() ? nullptr : &*it;
}

// len(value or ""): code points of a string, items of a container.
std::size_t text_size(const Json& value) {
    if (!detail::truthy(value)) {
        return 0;
    }
    if (value.is_string()) {
        return py::len(value.get_ref<const std::string&>());
    }
    if (value.is_array() || value.is_object()) {
        return value.size();
    }
    throw py::TypeError("object of type '" + std::string(value.type_name()) + "' has no len()");
}

// sum(1 for item in (items or []) if status(item) != excluded)
std::int64_t live_count(const Json& items, std::string_view excluded) {
    std::int64_t count = 0;
    for (const auto& item : detail::list_or_empty(items)) {
        if (detail::str_or_empty(detail::get(item, "status")) != excluded) {
            ++count;
        }
    }
    return count;
}

// stats[key]: Python's KeyError when it is missing.
const Json& stat(const Json& stats, std::string_view key) {
    if (!stats.is_object() || !stats.contains(key)) {
        throw py::PyError("KeyError", py::repr(key));
    }
    return stats.at(std::string(key));
}

// step.get("name") or step.get("id")
std::string name_or_id(const Json& step) {
    const Json& name = detail::get(step, "name");
    return detail::py_str(detail::truthy(name) ? name : detail::get(step, "id"));
}

} // namespace

namespace finalization_detail {

void log_call_raised(std::string_view name, const std::exception* error) {
    const std::string message = std::format("Agent call {} raised", name);
    if (error != nullptr) {
        logger.error_with(*error, message);
    } else {
        logger.error("{}", message);
    }
}

void log_call_timed_out(std::string_view name, std::chrono::milliseconds timeout) {
    logger.warning("{} timed out after {:.0f}s; canceling", name,
                   std::chrono::duration<double>(timeout).count());
}

void log_timeout_handler_raised(std::string_view name, const std::exception* error) {
    const std::string message = std::format("Timeout handler for {} raised", name);
    if (error != nullptr) {
        logger.error_with(*error, message);
    } else {
        logger.error("{}", message);
    }
}

void log_cancel_raised(std::string_view name, const std::exception* error) {
    const std::string message = std::format("Agent cancel raised after {} timed out", name);
    if (error != nullptr) {
        logger.error_with(*error, message);
    } else {
        logger.error("{}", message);
    }
}

} // namespace finalization_detail

std::vector<std::pair<std::size_t, std::size_t>> polish_blocks(const std::vector<Json>& segments) {
    std::vector<std::pair<std::size_t, std::size_t>> blocks;
    std::size_t start = 0;
    std::size_t covered = 0;
    while (covered < segments.size()) {
        std::size_t end = start;
        std::size_t chars = 0;
        while (end < segments.size() && end - start < kPolishMaxSegments) {
            const std::size_t size = text_size(detail::get(segments[end], "text"));
            if (end > start && chars + size > kPolishMaxTextChars) {
                break;
            }
            chars += size;
            ++end;
        }
        if (end <= covered) {
            // Context consumed the text budget; retry without that overlap.
            start = covered;
            continue;
        }
        blocks.emplace_back(start, end);
        covered = end;
        const std::size_t overlap = std::min(kPolishOverlapSegments, (end - start) / 4);
        start = end - overlap;
    }
    return blocks;
}

Json summary_stats(const Json& cards, const Json& questions, const std::vector<Json>& transcript,
                   double duration_s) {
    static const Json empty_cards = Json::object();
    const Json& by_card = cards.is_object() ? cards : empty_cards;
    std::int64_t words = 0;
    for (const auto& segment : transcript) {
        words += static_cast<std::int64_t>(
            py::split(detail::str_or_empty(detail::get(segment, "text"))).size());
    }
    Json stats = Json::object();
    stats["segments"] = static_cast<std::int64_t>(transcript.size());
    stats["words"] = words;
    stats["key_points"] = live_count(detail::get(by_card, "key_points"), "removed");
    stats["action_items"] = live_count(detail::get(by_card, "action_items"), "removed");
    stats["decisions"] = live_count(detail::get(by_card, "decisions"), "removed");
    stats["risks"] = live_count(detail::get(by_card, "risks"), "removed");
    stats["questions"] = live_count(questions, "dismissed");
    // max(0.0, nan) is 0.0 in Python as here: the comparison with NaN is false.
    stats["duration_s"] = std::max(0.0, duration_s);
    return stats;
}

Json summary_stats(const Json& state, const std::vector<Json>& segments, double duration_s) {
    return summary_stats(detail::get(state, "cards"), detail::get(state, "questions"), segments,
                         duration_s);
}

std::string saved_state_detail(const Json& stats) {
    return std::format("Saved {} segments ({} words)", detail::py_str(stat(stats, "segments")),
                       detail::py_str(stat(stats, "words")));
}

std::string insights_ready_message(const Json& stats) {
    std::string parts = detail::py_str(stat(stats, "segments")) + " segments";
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kLabels{{
        {"key_points", "key points"},
        {"action_items", "action items"},
        {"decisions", "decisions"},
    }};
    for (const auto& [key, label] : kLabels) {
        const Json& value = detail::get(stats, key);
        if (detail::truthy(value)) {
            parts += std::format(", {} {}", detail::py_str(value), label);
        }
    }
    return std::format("Final insights ready — {}.", parts);
}

std::string sparse_redecode_detail(std::int64_t new_words, std::int64_t old_words) {
    return std::format("Re-transcription produced {} words versus {} in the live transcript "
                       "(below the 80% coverage threshold). Kept the live transcript to avoid "
                       "losing speech.",
                       new_words, old_words);
}

std::optional<std::string_view> step_name(std::string_view step_id) {
    const StepText* step = find_step(step_id);
    return step != nullptr ? std::optional(step->name) : std::nullopt;
}

std::optional<std::string_view> step_detail(std::string_view step_id) {
    const StepText* step = find_step(step_id);
    return step != nullptr ? std::optional(step->detail) : std::nullopt;
}

Json make_step(std::string_view step_id, std::string_view status) {
    const StepText* step = find_step(step_id);
    if (step == nullptr) {
        throw py::PyError("KeyError", py::repr(step_id));
    }
    Json made = Json::object();
    made["id"] = std::string(step->id);
    made["name"] = std::string(step->name);
    made["status"] = std::string(status);
    made["detail"] = std::string(step->detail);
    return made;
}

std::string failed_steps_message(const Json& steps) {
    std::string names;
    std::string details;
    for (const auto& step : detail::list_or_empty(steps)) {
        if (detail::get(step, "status") != "failed") {
            continue;
        }
        const std::string name = name_or_id(step);
        names += names.empty() ? name : ", " + name;
        if (detail::truthy(detail::get(step, "detail"))) {
            const std::string line =
                std::format("{}: {}", name, detail::py_str(detail::get(step, "detail")));
            details += details.empty() ? line : " " + line;
        }
    }
    if (names.empty()) {
        return "";
    }
    std::string message = names + " failed. The recording and transcript were kept.";
    if (!details.empty()) {
        message += " " + details;
    }
    return message;
}

std::pair<std::string, std::string> speaker_step_outcome(const Json& result) {
    if (detail::truthy(detail::get(result, "ok"))) {
        const Json& applied = detail::get(result, "applied");
        const std::int64_t count = detail::int_or_zero(applied);
        return {"completed",
                std::format("Updated {} speaker label{}", count, count == 1 ? "" : "s")};
    }
    const std::string error = detail::str_or_empty(detail::get(result, "error"));
    if (detail::truthy(detail::get(result, "skipped"))) {
        return {"completed", error.empty() ? "Speaker identification skipped." : error};
    }
    return {"failed", error.empty() ? "Speaker identification failed." : error};
}

SpeakerPassGate speaker_pass_gate(std::string_view backend, bool consent,
                                  const std::function<bool()>& key_present,
                                  std::chrono::year_month_day today) {
    if (backend != "openai") {
        return {false, std::string(kNotOpenaiReason), false};
    }
    if (speech::retired(today)) {
        return {false, std::string(speech::kSpeakerModelRetiredMessage), false};
    }
    if (!consent) {
        return {false, std::string(kNoConsentReason), true};
    }
    if (key_present) {
        bool found = false;
        try {
            found = key_present();
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not resolve the OpenAI API key");
        }
        if (!found) {
            return {false, std::string(kNoKeyReason), true};
        }
    }
    return {true, "", true};
}

SpeakerPassGate speaker_pass_gate(std::string_view backend, bool consent,
                                  const std::function<bool()>& key_present, const Clock& clock) {
    return speaker_pass_gate(backend, consent, key_present, speech::local_today(clock));
}

} // namespace fv::meeting
