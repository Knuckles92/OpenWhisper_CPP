#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/json.h"
#include "meeting_state/repository.h"

// meeting/content.py: what a persisted meeting row shows in history lists (titles, the insights
// pill, the preview line) and the content capabilities derived from its durable rows.
// Affinity: thread-safe (functions; the repository is the caller's).
namespace fv::meeting_state {

// Longest preview line, in code points, before it is clipped with "…".
inline constexpr std::size_t PREVIEW_LIMIT = 100;

// The best stored label: the SQL title, then the snapshot's title, then its topic; "" when none.
// The snapshot is the row's "state" object, else its state_json.
std::string meeting_display_title(const Json& meeting);
// The display title, or "Failed meeting" / "Untitled meeting" when nothing is stored.
std::string fallback_meeting_title(const Json& meeting);
// topic.current (at most MAX_NAME_LEN code points) while the snapshot's title is still blank.
std::string untitled_title_from_topic(const Json& state);
// [{"op": "set_title", "text": <topic>}] for a still untitled meeting, else [].
Json build_untitled_title_ops(const Json& state);
// (label, tone) of a history row's insights pill: "Failed start" and "Empty" first, then the
// stored finalization's pill; nullopt when there is none or it cannot be read.
std::optional<std::pair<std::string, std::string>> meeting_insights_pill(const Json& meeting);
// Honest audio and transcript capabilities from durable rows (not the finalization status):
// {has_audio, has_loopback_audio, has_transcript, is_empty, audio_chunks, transcript_segments,
// can_rerun_speakers, preview_text}. The SQL repository answers from its aggregate reads; any
// other reads chunks and segments, and a failed read counts as none (logged).
Json summarize_meeting_content(MeetingRepository& repository, std::string_view meeting_id);
// The list preview: content_summary's preview_text, else the snapshot's topic, else "".
std::string meeting_preview_text(const Json& meeting);

} // namespace fv::meeting_state
