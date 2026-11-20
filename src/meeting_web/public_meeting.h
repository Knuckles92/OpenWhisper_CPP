#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/repository.h"

// meeting/web/api.py's client-safe projection of a meeting row (_public_meeting) and the
// History row digest (_meeting_digest). Tokens, state_json and step details never go out.
// Affinity: thread-safe (functions; the repository is the caller's).
namespace fv::meeting_web {

// The meeting columns a dashboard client may see.
inline constexpr std::array<std::string_view, 8> kPublicMeetingKeys = {
    "id",       "title",          "status",        "started_at",
    "ended_at", "paused_total_s", "cloud_enabled", "asr_model"};

// Statuses whose clock still runs (no ended_at yet).
inline constexpr std::array<std::string_view, 3> kRunningStatuses = {"active", "paused", "ending"};

// Avatars shown per History row; the full count travels separately.
inline constexpr std::size_t kDigestParticipants = 6;

// Counts and people for a History row from the saved snapshot: non-removed decisions, action
// items and risks, open questions (a list or a mapping), participants with an id sorted "me"
// first, then created_at, then id, at most kDigestParticipants of them. nullopt for a missing
// or corrupt snapshot; never throws.
std::optional<Json> meeting_digest(const Json& meeting);

// The public fields, display_title, duration_s (null when unreadable), digest (when there is
// one), the compact finalization fields, and content_summary with has_audio, has_transcript and
// can_rerun_speakers when the row carries a summary or a repository is given (which then
// summarizes the meeting's durable rows). {} for a null or empty row. A row whose
// content_summary lacks one of the three flags raises KeyError, as Python does.
Json public_meeting(const Json& meeting, meeting_state::MeetingRepository* repository = nullptr,
                    const Clock& clock = system_clock());

} // namespace fv::meeting_web
