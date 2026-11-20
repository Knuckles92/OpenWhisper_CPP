#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "llm/judge.h"

// meeting/agent/typesafe_signals.py: the meeting's Fast judgments questions, worded exactly as
// benchmarked on Jev. Each helper asks one narrow judgment over explicit state and answers
// nullopt whenever the judge is unavailable or fails, so callers keep their deterministic
// fallback. The thresholds were measured against AMI human labels on September 18, 2026; they
// are application policy, not properties of the model.
// Affinity: thread-safe; the helpers block on the judge (never on the GUI thread).
namespace fv::meeting_agent {

using Json = fv::Json;

// The scheduler's cut-off (SHIFT_SEMANTIC_THRESHOLD) was measured on 97 human-labelled minute
// pairs: AUROC 0.845 for this question.
inline constexpr std::string_view TOPIC_CHANGED_INSTRUCTIONS =
    "Does the discussion in `window` move on to a different agenda topic or activity than the "
    "one in `previous_window`? Elaborating the same topic, or a brief aside that returns to it, "
    "is not a change.";
// The scheduler waits for this answer before its next checkpoint and word overlap decides when
// none arrives, so a slow text model gets no longer than this (seconds). Jev answers well inside.
inline constexpr double TOPIC_TIMEOUT_S = 5.0;
// A minute of speech is about a thousand characters: headroom for a dense window without letting
// the request grow with the meeting (code points).
inline constexpr std::size_t TOPIC_WINDOW_MAX_CHARS = 6'000;

// Probability that `window` left the topic of `previous_window`: the older window keeps its
// tail and the newer its head, each stripped; nullopt when either is empty.
std::optional<double> topic_shift_probability(llm::Judge& judge, std::string_view previous_window,
                                              std::string_view window,
                                              const CancelToken& cancel = {});

// Measured on 884 real speech segments: no false positives at any confidence, 41 of 41 authored
// commands recognised, 40 of 41 at 0.8. Person-directed requests such as "write that down for
// me" also fire, which is why callers gate on a wake name in code before asking.
inline constexpr double VOICE_COMMAND_CONFIDENCE = 0.6;
inline constexpr std::string_view VOICE_COMMAND_INSTRUCTIONS =
    "Is `segment` an instruction directed at the automated note-taking assistant, which "
    "participants address using one of `assistant_names` or by referring explicitly to the notes "
    "it keeps? Speech directed at another person is none, even if it mentions notes, decisions or "
    "topics. `previous_segments` is context only.";
// {command: what it means}, in the benchmarked order: none, mark_decision, mark_action,
// note_this, recap, fix_transcript, set_topic.
const Json& voice_command_criteria();

// Classifies a wake-named segment as one command or "none"; the last three non-empty previous
// segments are context. nullopt for an empty segment or no judgment.
std::optional<llm::ChoiceAnswer> voice_command_choice(
    llm::Judge& judge, std::string_view segment, const std::vector<std::string>& previous,
    const std::vector<std::string>& assistant_names, const CancelToken& cancel = {});

} // namespace fv::meeting_agent
