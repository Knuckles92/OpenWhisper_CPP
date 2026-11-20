#pragma once

#include <cstddef>
#include <string_view>

#include "base/json.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/types.h"

// meeting/state/custom_reports.py: the tailored reports a participant asks for in their own
// words. One lifecycle: the host's request_custom_report claims a slot under the state lock (a
// double-click cannot start two writers), the report worker's finish_custom_report fills it in,
// and the host may remove_custom_report. The agent never reaches these ops: the worker writes
// as "system" with the WORKER_ACTOR id, so an intelligence pass cannot forge a report nobody
// asked for. Reports are never undoable (no inverse). Effects are {"entity": "custom_report",
// "report": <report>, "removed": bool}.
// Affinity: functions over a document the caller owns.
namespace fv::meeting_state {

// The worker's actor id: only this system actor may publish report text.
inline constexpr std::string_view WORKER_ACTOR = "custom-report";

// Code points. A longer request is rejected ("request_too_long"); a longer report, title or
// message is clipped silently, because reports live in state_json, read whole per snapshot.
inline constexpr std::size_t MAX_REQUEST_CHARS = 2000;
inline constexpr std::size_t MAX_REPORT_CHARS = 60000;
inline constexpr std::size_t MAX_TITLE_CHARS = 200;
inline constexpr std::size_t MAX_MESSAGE_CHARS = 500;

// {request, run_id}: host or system, once the meeting ended and finalization settled; one
// running report at a time, at most MAX_CUSTOM_REPORTS kept. The record starts "running".
OpResult request_custom_report(MeetingState& state, Json& op, const OpContext& ctx);
// {report_id, run_id, status: ready | failed, markdown?, title?, message?, sources?}: the
// worker only; a run that is not the report's current one is "stale_report".
OpResult finish_custom_report(MeetingState& state, Json& op, const OpContext& ctx);
// {report_id}: host or system, finished or still running (the escape hatch for a run a crash
// stranded; its late finish then lands on nothing).
OpResult remove_custom_report(MeetingState& state, Json& op, const OpContext& ctx);

} // namespace fv::meeting_state
