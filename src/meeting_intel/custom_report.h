#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "agents/drivers.h"
#include "agents/loopback_mcp.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/generation.h"
#include "meeting_intel/recall.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"
#include "pyport/errors.h"

// meeting/custom_report.py: tailored reports. A participant says what they want out of a
// finished meeting in their own words, and one bounded pass reads the whole record and writes
// that document. The tools are all reads (this meeting's transcript, and the consent-gated
// past-meeting and knowledge-folder searches) and the model's last text is the deliverable. A
// long meeting's transcript is reached through the read tools rather than pasted whole.
// Transcript text is evidence, never instruction, and none of it is ever logged.
// Affinity: the corpus and prompt functions are pure; generate_report blocks for the whole pass
// (never on the GUI thread); start_custom_report returns once the report is claimed.
namespace fv::meeting_intel {

// Wall clock for one report, in seconds: the pass may read for several rounds first.
inline constexpr double kReportDefaultTimeoutS = 420.0;
// Code points of transcript pasted whole into the opening prompt; above it the prompt carries
// the opening and closing stretches and the tools reach the middle.
inline constexpr std::size_t kReportInlineTranscriptChars = 60000;
// Code points kept at each edge when the transcript is above the inline budget.
inline constexpr std::size_t kReportTranscriptEdgeChars = 14000;
// Code points of the dashboard digest (cards, notes, questions).
inline constexpr std::size_t kReportDigestChars = 28000;
// Code points of one read_transcript / search_transcript tool result.
inline constexpr std::size_t kReportWindowChars = 18000;
inline constexpr std::size_t kReportSearchResultChars = 9000;
inline constexpr std::int64_t kReportDefaultSearchLimit = 20;
inline constexpr std::int64_t kReportMaxSearchLimit = 60;
// Model calls before the model must write; bounds cost as well as latency.
inline constexpr int kReportMaxToolRounds = 6;
// A little warmth reads better in a document than the checkpoint passes' zero.
inline constexpr double kReportTemperature = 0.2;

// The system message of every report pass (SYSTEM_PROMPT).
std::string_view report_system_prompt();
// What the installed agent's closing reply must be (_AGENT_REPORT_CLOSING).
inline constexpr std::string_view kReportAgentClosing =
    "When you have what you need, reply with the finished report in Markdown and nothing else: "
    "no preamble, no notes about the tools.";
// The last round's user message, which also takes the tools away.
inline constexpr std::string_view kReportStopReading =
    "Stop reading and write the report now from what you have, noting anything you could not "
    "confirm.";

// The report could not be produced; what() is a sentence safe to show the participant.
class ReportUnavailable : public py::PyError {
public:
    explicit ReportUnavailable(const std::string& message)
        : py::PyError("ReportUnavailable", message) {}
};

// Meeting-clock seconds as "M:SS", or "H:MM:SS" past an hour; negative and NaN read as 0. An
// infinite value throws py::OverflowError, as Python's int() does. Seconds beyond the int64
// range are clamped to it (Python's int is unbounded).
std::string format_clock(double seconds);
// format_clock(float(value)): a value float() refuses (null, text that is not a number, a
// container) reads as 0.
std::string format_clock(const Json& seconds);

// The meeting's transcript rendered once as citable lines ("[3:12] Priya: we should hold the
// price") and readable by time or by term. Segments without text are dropped. Lengths are code
// points.
// Affinity: immutable once built; thread-safe.
class TranscriptCorpus {
public:
    // `segments` are repository segment rows ordered by start_s; `participants` the state's
    // {id: participant} map (null for none). Throws py::ValueError or py::TypeError for a
    // start_s or end_s that float() refuses, as Python's constructor does.
    TranscriptCorpus(const std::vector<Json>& segments, const Json& participants);

    std::size_t size() const { return lines_.size(); }
    const std::vector<std::string>& lines() const { return lines_; }
    // The largest end_s, 0.0 for no lines.
    double duration_s() const { return duration_s_; }
    // Every line's length plus its newline.
    std::size_t total_chars() const;

    std::string render_all() const;
    // The opening and closing stretches within `edge_chars` each, the gap between them labelled
    // with its time range; the whole transcript when nothing would be skipped.
    std::string render_edges(std::size_t edge_chars = kReportTranscriptEdgeChars) const;
    // The lines starting between two offsets (either order), within kReportWindowChars, or a
    // sentence saying there are none. format_clock's OverflowError for an infinite bound that
    // matches nothing.
    std::string window(double start_s, double end_s) const;
    // Term-overlap search, best matches shown in time order. `limit` goes through
    // int(limit or 20) and is clamped to 1..60: a value int() refuses throws its py::ValueError,
    // py::TypeError or py::OverflowError.
    std::string search(std::string_view q, int limit = kReportDefaultSearchLimit) const;
    std::string search(std::string_view q, const Json& limit) const;

private:
    std::vector<Json> segments_;
    std::vector<std::string> lines_;
    std::vector<std::size_t> lengths_; // code points of each line
    std::vector<double> starts_;
    double duration_s_ = 0.0;
};

// Everything the meeting recorded except the transcript: header, topic and summary,
// participant corrections, the live cards with the qualifiers a writer must keep, and the
// questions; clipped to kReportDigestChars. `meeting` is the repository row, `state` a
// MeetingState snapshot.
std::string build_digest(const Json& meeting, const Json& state, const TranscriptCorpus& corpus);
// The opening user message: the ask, the digest, the transcript (whole, its edges, or a note
// that there is none) and the closing instructions.
std::string build_corpus_prompt(const Json& meeting, const Json& state,
                                const TranscriptCorpus& corpus, std::string_view request);
// The document's own "# " title (at most 200 code points) when its first non-blank line is
// one, else the request with whitespace collapsed, clipped to 77 code points and "…" when it is
// longer than 78.
std::string report_title(std::string_view markdown, std::string_view request);

// Who writes the report: a text-model profile id with its model and the meeting's endpoint
// snapshot (null for none), or an installed agent id (claude_code, codex, opencode_cli).
struct ReportRoute {
    std::string provider;
    std::string model;
    Json endpoint;
};

// generate_report with Python's generator arguments; tests replace it.
using ReportGenerator =
    std::function<Json(const Json& meeting, const Json& state, const std::vector<Json>& segments,
                       std::string_view request, const ReportRoute& route, double timeout_s)>;
// agents::run_agent_task's signature; tests replace it.
using ReportAgentTask = std::function<agents::PassOutcome(
    std::string_view agent_id, std::string_view model, std::string_view system_prompt,
    std::string_view user_prompt, const Json& tools, agents::ToolHandler handler, double timeout_s,
    std::string_view closing, const CancelToken& cancel)>;

// What a report pass reads and runs on. The repository, the key sources, `recall` and `clock`
// must outlive every report started with them.
struct ReportDeps {
    meeting_state::MeetingRepository& repository;
    std::shared_ptr<http::Client> http;
    llm::KeySources keys;
    // The settings mapping (profiles, the recall and knowledge-folder switches), read per pass.
    std::function<Json()> settings;
    // Past-meeting recall; nullptr leaves search_past_meetings out of the tools.
    RecallDeps* recall = nullptr;
    // The pass's deadline is measured on its steady clock.
    const Clock& clock = system_clock();
    // Runs the report off the caller's thread; nullptr starts a thread of its own.
    Executor* worker = nullptr;
    // Empty: generate_report.
    ReportGenerator generator;
    // Empty: agents::run_agent_task.
    ReportAgentTask agent_task;
    CancelToken cancel;
    std::function<bool()> destination_allowed;
};

// Runs one report pass and returns {markdown, title, sources, usage}. sources is
// {transcript_lines, duration_s, transcript_complete, tools_used, past_meetings,
// knowledge_folder, model, provider}. Throws ReportUnavailable with a sentence to show; other
// exceptions mean a fault (the caller shows a generic sentence).
Json generate_report(const Json& meeting, const Json& state, const std::vector<Json>& segments,
                     std::string_view request, const ReportRoute& route, ReportDeps& deps,
                     double timeout_s = kReportDefaultTimeoutS, const CancelToken& cancel = {});

// Writes the report `report_id` claimed under `run_id` and publishes the outcome through the
// store as the report worker: ready with the document, or failed with a readable message, never
// left running.
void run_custom_report(const std::shared_ptr<meeting_state::MeetingStateStore>& store,
                       ReportDeps& deps, std::string_view report_id, std::string_view run_id,
                       std::string_view request, const ReportRoute& route,
                       double timeout_s = kReportDefaultTimeoutS);
// Claims a report slot under the store's lock (as the host `actor_id`, "" for none), so a
// second request while one runs is refused before any model call, then writes it on
// deps.worker or on a thread "fv-meeting-custom-report". Returns {ok, report_id, error}: error
// is the store's rejection reason, or "start_failed" when no thread could start (the report is
// then failed with "The report could not be started. Retry later.").
Json start_custom_report(std::shared_ptr<meeting_state::MeetingStateStore> store, ReportDeps& deps,
                         std::string_view request, const ReportRoute& route,
                         std::string_view actor_id, double timeout_s = kReportDefaultTimeoutS);

namespace detail {

// The report's read tools as Chat Completions function schemas, search_past_meetings and
// search_context_files only when offered.
Json report_tool_schemas(bool past_recall, bool context_files);
// The same tools as MCP definitions {name, description, inputSchema}.
Json report_mcp_tools(bool past_recall, bool context_files);

// The read tools of one pass over its corpus; counts every call by name, unknown names too.
// Affinity: thread-safe.
class ReportTools {
public:
    // `recall` may be null (past-meeting recall then answers "not available").
    ReportTools(const TranscriptCorpus& corpus, RecallDeps* recall, std::function<Json()> settings,
                std::string meeting_id);

    // The tool's text answer; the exceptions of the tool itself propagate.
    std::string dispatch(std::string_view name, const Json& args);
    // {name: calls}, sorted by name.
    Json used() const;

private:
    const TranscriptCorpus& corpus_;
    RecallDeps* recall_;
    std::function<Json()> settings_;
    std::string meeting_id_;
    mutable std::mutex mutex_;
    std::map<std::string, std::int64_t, std::less<>> used_;
};

// _run_tool: one call's text answer, never an exception: "Could not parse the arguments for X.",
// "Arguments for X must be an object.", "X failed; continue without it." (the failure's type
// logged), or the tool's answer.
std::string run_report_tool(ReportTools& tools, const llm::ToolCall& call);
// _clean_markdown: stripped, a wrapping ``` fence removed when the last line closes it too, and
// clipped to MAX_REPORT_CHARS with "\n\n*(Report truncated.)*" counted inside the cap.
std::string clean_markdown(std::string_view markdown);
// _validated_request: the stripped request; ReportUnavailable when it is blank or longer than
// MAX_REQUEST_CHARS.
std::string validated_request(std::string_view request);

// _terms: the lower-cased [\w']+ words longer than two code points, stop words left out.
std::vector<std::string> report_terms(std::string_view text);
// _clip_lines: lines within `budget` (each counted with its newline), then a note of how many
// were left out.
std::string clip_lines(const std::vector<std::string>& lines, std::size_t budget);
// str.splitlines() without the line ends.
std::vector<std::string> splitlines(std::string_view text);

} // namespace detail

} // namespace fv::meeting_intel
