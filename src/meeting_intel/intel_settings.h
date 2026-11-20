#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"

// services/settings.py's resolvers for the meeting features of Fast judgments, past-meeting
// recall, the knowledge folder and the insight review. Each reads a settings mapping (a snapshot;
// missing or invalid keys read as their defaults).
// Affinity: thread-safe (pure functions; resolve_meeting_context_folder_path reads the working
// directory).
namespace fv::meeting_intel {

// The four meeting features resolve_typesafe_feature_enabled knows.
inline constexpr std::string_view kFeatureCitations = "citations";
inline constexpr std::string_view kFeatureSemanticSearch = "semantic_search";
inline constexpr std::string_view kFeatureHighlights = "highlights";
inline constexpr std::string_view kFeatureQuestionRadar = "question_radar";

// The consent text version an insight review is agreed under (INSIGHT_REVIEW_CONSENT).
inline constexpr std::string_view kInsightReviewConsent = "typesafe-text-v1";

// Fast judgments' master switch.
bool resolve_typesafe_enabled(const Json& settings);
// The feature's own switch AND the master switch. With the master switch on, throws
// std::out_of_range for a name that is not one of the four (Python's KeyError).
bool resolve_typesafe_feature_enabled(const Json& settings, std::string_view name);
// Topic-shift judgments for the checkpoint scheduler (on by default once the master is on).
bool resolve_typesafe_topic_shift_enabled(const Json& settings);
// Spoken commands to the note taker (off by default).
bool resolve_typesafe_voice_commands_enabled(const Json& settings);

// The names participants call the note taker by when the settings store none: "note taker",
// "notetaker", "assistant" and the two spoken forms of `defaults_from`'s product name (OpenWhisper
// lists its own; lead amendment 19).
std::vector<std::string> default_voice_command_names(const AppIdentity& defaults_from = identity());
// The stored wake names, each stripped, lower-cased and with whitespace runs joined by one space;
// blank and non-text entries dropped. The defaults when the setting is unset (null) or nothing is
// left.
std::vector<std::string>
resolve_typesafe_voice_command_names(const Json& settings,
                                     const AppIdentity& defaults_from = identity());

bool resolve_meeting_past_recall_enabled(const Json& settings);
bool resolve_meeting_context_folder_enabled(const Json& settings);
// The knowledge folder as an absolute, normalized path ("~" expanded through `env`, a relative
// path made absolute against the working directory), or "" when unset. The folder need not exist.
std::string resolve_meeting_context_folder_path(const Json& settings,
                                                const EnvReader& env = system_env());

// {enabled, consent, sensitivity}: enabled only with the review switch, the consent of
// kInsightReviewConsent and the master switch; consent is that version or "".
Json resolve_meeting_insight_review(const Json& settings);

} // namespace fv::meeting_intel
