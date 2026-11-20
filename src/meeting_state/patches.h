#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/ids.h"
#include "meeting_state/schema.h"
#include "meeting_state/types.h"

// meeting/state/patches.py: the single vocabulary of meeting-state changes. Agent output and
// human dashboard actions are both ops; apply_ops validates and applies them against a
// MeetingState with the protection rules (human corrections are authoritative; the agent can
// never overwrite human-touched content) and returns one result per op, with inverse ops for
// host undo. Rejections are per op, and their reasons go back to the agent.
// Affinity: apply_ops is reentrant on distinct states; the op registry is thread-safe.
namespace fv::meeting_state {

using Json = fv::Json;

// Validation limits (code points unless stated).
inline constexpr std::size_t MAX_TEXT_LEN = 4000;
inline constexpr std::size_t MAX_TOPIC_LEN = 500;
inline constexpr std::size_t MAX_NAME_LEN = 120;
inline constexpr std::size_t MAX_INTENT_LEN = 2000; // the host's standing brief
inline constexpr std::size_t MAX_SUMMARY_LEN = 8000;
inline constexpr std::size_t MAX_OPEN_QUESTIONS = 7; // the agent's quiet inbox
inline constexpr std::size_t MAX_EVIDENCE_REFS = 20;
inline constexpr std::size_t MAX_OPEN_QUESTIONS_HUMAN = 50; // bounds an abusive client
// json.dumps(data) with ASCII escapes and ", " / ": " separators, in characters.
inline constexpr std::size_t MAX_DATA_BYTES = 4096;
inline constexpr std::size_t MAX_CARD_ITEMS = 500;
inline constexpr std::size_t MAX_PARTICIPANTS = 200;

// Cards only humans may write to.
inline constexpr std::string_view HUMAN_ONLY_CARD = "user_notes";
// The AI note taker's card and the only ops a note-taker pass may emit.
inline constexpr std::string_view NOTES_CARD = "live_notes";
inline constexpr std::array<std::string_view, 3> NOTES_ONLY_OPS = {"add_item", "update_item",
                                                                   "remove_item"};
// The only participant kind the agent may mint.
inline constexpr std::string_view AGENT_PARTICIPANT_KIND = "others_cluster";

// Agent question resolution: at least RESOLVE_CONFIDENCE answers "from audio", at least
// SUGGEST_CONFIDENCE only suggests.
inline constexpr double RESOLVE_CONFIDENCE = 0.8;
inline constexpr double SUGGEST_CONFIDENCE = 0.4;

// Ops the agent may emit; everything else is human or system only.
inline constexpr std::array<std::string_view, 10> AGENT_OPS = {"add_item",
                                                               "update_item",
                                                               "remove_item",
                                                               "set_topic",
                                                               "set_rolling_summary",
                                                               "upsert_participant",
                                                               "suggest_participant_name",
                                                               "ask_question",
                                                               "resolve_question",
                                                               "revise_segment_text"};
// Ops only the agent (or the system, for undo) may emit.
inline constexpr std::array<std::string_view, 2> AGENT_ONLY_OPS = {"resolve_question",
                                                                   "revise_segment_text"};
// Meeting-level metadata a guest ("user") may not change.
inline constexpr std::array<std::string_view, 4> HOST_ONLY_OPS = {
    "set_topic", "set_rolling_summary", "set_title", "set_meeting_intent"};
// Ops on the transcript segment log: validated here, applied by the store's segment handler.
inline constexpr std::array<std::string_view, 2> SEGMENT_OPS = {"reassign_segment_speaker",
                                                                "revise_segment_text"};

// Per-batch context. actor_type is "agent", "user" (a guest), "host" or "system" (undo,
// repair, workers, the app); actor_id is the participant id for humans or the agent's name.
// The predicates are optional (empty: not checked); segment_pinned may throw, which fails
// closed. clock and ids stamp and name new entities (nullptr: the system clock and random ids).
struct OpContext {
    std::string actor_type;
    std::optional<std::string> actor_id;
    std::function<bool(std::string_view)> segment_exists;
    std::function<bool(std::string_view)> segment_pinned;
    const Clock* clock = nullptr;
    IdSource* ids = nullptr;

    bool is_agent() const { return actor_type == "agent"; }
    bool is_human() const { return actor_type == "user" || actor_type == "host"; }
    const Clock& time() const { return clock ? *clock : system_clock(); }
    std::string new_id(std::string_view prefix) const;
    std::string now_iso() const;
};

// Validates and applies a batch (a JSON list of op objects) against `candidate`, each op on its
// own: a rejected op leaves the document as it was. Callers own locking, seq numbering,
// persistence and broadcast. Gates, in order: malformed_op, unknown_op, agent_forbidden (an
// agent op outside AGENT_OPS), the agent's evidence on every op, agent_only, host_only (for
// "user"), then the handler; a handler that throws is internal_error. A segment_exists
// predicate that throws at the agent's evidence gate propagates, as in Python. Throws
// py::TypeError when `ops` is not a list or an op's name is a list or an object (Python cannot
// hash it), after applying the ops before it.
std::vector<OpResult> apply_ops(MeetingState& candidate, const OpContext& ctx, const Json& ops);

// The registry. A handler validates before it changes anything and returns ok or a rejection;
// `op` is its own copy of the submitted op, which it may normalize (timeline start_s becomes a
// float) and which goes into the result.
using Handler = std::function<OpResult(MeetingState& state, Json& op, const OpContext& ctx)>;

// The parts of the document (or the segment log) an op may write, for copy-on-write stores.
struct OpDomains {
    bool cards = false;
    bool questions = false;
    bool participants = false;
    bool topic = false;
    bool summary = false; // rolling_summary and its evidence
    bool title = false;
    bool intent = false;
    bool cloud = false;      // cloud_enabled
    bool review = false;     // insight_review
    bool reports = false;    // custom_reports
    bool highlights = false; // live_highlights
    bool feedback = false;   // voice_feedback
    bool segments = false;   // the transcript segment log, through the store's handler

    friend bool operator==(const OpDomains&, const OpDomains&) = default;
};

// Gate membership of a registered op. Python's AGENT_OPS, AGENT_ONLY_OPS, HOST_ONLY_OPS and
// SEGMENT_OPS name the built-in ones.
struct OpFlags {
    bool agent = false;      // the agent may emit it
    bool agent_only = false; // only the agent or the system may
    bool host_only = false;  // a "user" may not
    bool segment = false;    // applied to the segment log by the store
};

// Adds an op to the vocabulary (the review, custom-report and fast handlers of later packages).
// Throws std::logic_error for a name already registered.
void register_op(std::string name, Handler handler, OpDomains domains, OpFlags flags);
// Every registered op with its domains, the built-in ones included. The map is the registry's
// own: iterate it only once every package has registered its ops (a register_op running at the
// same time is a data race). A store that looks an op up per batch uses op_domain.
const std::map<std::string, OpDomains, std::less<>>& op_domains();
// The domains of one registered op, read under the registry's lock; nullopt for an unknown name.
std::optional<OpDomains> op_domain(std::string_view name);
std::optional<OpFlags> op_flags(std::string_view name);
// ALL_OPS: every registered op name.
std::set<std::string, std::less<>> all_ops();

// Rejection and validation helpers shared with the handlers of later packages.
OpResult reject(const Json& op, std::string reason);
OpResult reject(const Json& op, std::string reason, std::string target_id,
                std::optional<std::int64_t> current_revision = std::nullopt);
// The op's evidence list: missing_evidence (an agent citing nothing), invalid_evidence (not a
// list, more than MAX_EVIDENCE_REFS, or, without a segment_exists predicate, an id that is not
// text), unknown_evidence (an id segment_exists refuses).
std::optional<std::string> check_evidence(const Json& op, const OpContext& ctx);
// invalid_text (not text, or blank), text_too_long (past max_len code points).
std::optional<std::string> check_text(const Json& value, std::size_t max_len = MAX_TEXT_LEN);
// data_too_large past MAX_DATA_BYTES.
std::optional<std::string> check_data_size(const Json& data);

// The effect of an item edit: {"entity": "item", "item": ...}, or, when the edit supersedes
// insight-review questions about the item, {"entity": "review", "review": ..., "items": [...]}
// after marking them superseded and rewriting their linked notes (review.py item_effect).
// Throws as Python's KeyError and TypeError on a malformed insight_review.
Json item_effect(MeetingState& state, CardItem& item, const Clock& clock = system_clock());

// Keeps only live_notes item ops for a note-taker pass: add_item to live_notes, and
// update_item or remove_item of a known note id. `known_note_ids` nullopt allows adds only.
Json filter_notes_ops(const Json& ops,
                      const std::optional<std::set<std::string, std::less<>>>& known_note_ids);
// Ids of the live_notes blocks in a MeetingState::to_json() snapshot.
std::set<std::string, std::less<>> live_note_ids(const Json& state_snapshot);

} // namespace fv::meeting_state
