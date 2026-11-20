#pragma once

#include <vector>

#include "base/json.h"
#include "ui_settings/home_rows.h"

namespace fv {

// The where strip's meeting entries for a settings snapshot (settings_dialog.py _refresh_home):
// "Meeting voice" stays here, or "Meeting voice (audio sent to paired computer)" leaves when
// meetings go to the paired computer; "Speaker labels (system audio after End)" leaves while
// speaker labels upload audio (never in this build: speaker_id_is_remote); "Meeting intelligence
// (transcript text, when enabled)" leaves while meeting_intelligence_is_remote, else "Meeting
// intelligence" stays.
std::vector<HomeWhereItem> meeting_where_items(const Json& settings);

// Home's Meetings card rows (settings_home.py _build_meetings): "Voice & speakers" with a Change
// button that opens that page and a line naming the meeting model, language and speaker labels;
// "Clean up the transcript" and "Write a final report", projections of the After the meeting
// page's switches. OpenWhisper's "AI insights" row is M8's; the card's "All meeting settings"
// link is Home's own. Also adds meeting_where_items to the where strip.
// Affinity: GUI thread.
void registerMeetingHomeRows(HomeRows& rows);

} // namespace fv
