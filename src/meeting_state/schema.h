#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

// meeting/state/schema.py: the meeting-state document, the dashboard's single source of truth.
// to_json() round-trips through from_json() and is what is saved to meeting_sessions.state_json
// and sent to web clients; its key order is the wire and database order.
//
// from_json reads loosely typed documents as Python's from_dict does (bool(), int(), str() and
// their errors), with one difference: a field Python keeps as given but this port stores as text
// or a list of ids reads a value of another JSON type as missing (a required id as its str()),
// and drops ids that are not text. Documents written by to_json never hit it. Python's KeyError (a
// missing "id", "card" or "meeting_id") is a py::PyError with type_name() "KeyError"; a non-object
// where a mapping is needed is py::TypeError. Affinity: values; thread-compatible.
namespace fv::meeting_state {

using Json = fv::Json;

// The dashboard cards, in display and document order. timeline items carry data.start_s,
// action_items data.owner_participant_id, risks data.severity, live_notes (the AI note taker's
// record) data.heading and data.start_s.
inline constexpr std::array<std::string_view, 7> CARD_KEYS = {
    "key_points", "decisions", "action_items", "risks", "timeline", "live_notes", "user_notes"};
std::optional<std::size_t> card_index(std::string_view card);

// Lifecycle of one tailored report, and how many one meeting may keep.
inline constexpr std::array<std::string_view, 3> CUSTOM_REPORT_STATUSES = {"running", "ready",
                                                                           "failed"};
inline constexpr std::size_t MAX_CUSTOM_REPORTS = 20;

// Post-meeting consolidation lifecycle (orthogonal to the meeting's status).
inline constexpr std::array<std::string_view, 6> FINALIZATION_STATUSES = {
    "pending", "running", "completed", "disabled", "unavailable", "failed"};

inline constexpr std::string_view kInsightsOffMessage = "AI insights are off for this meeting.";
inline constexpr std::string_view kInsightsNotRecordedMessage =
    "Final insights were not recorded for this meeting.";
inline constexpr std::string_view kInsightsInterruptedMessage =
    "Final insights were interrupted before they finished.";
inline constexpr std::string_view kStepInterruptedDetail = "Interrupted before this step finished.";

// A saved state_json document: an object passes through; text is parsed. nullopt when it is
// missing, unreadable or not a JSON object.
std::optional<Json> parse_state_json(const Json& raw);
std::optional<Json> parse_state_json(std::string_view raw);
// Text arguments would otherwise be ambiguous between the two overloads above.
inline std::optional<Json> parse_state_json(const char* raw) {
    return parse_state_json(std::string_view(raw));
}
inline std::optional<Json> parse_state_json(const std::string& raw) {
    return parse_state_json(std::string_view(raw));
}

// A dict's insertion order and its update-in-place rule, for the document's keyed collections.
template <class V> class OrderedDict {
public:
    using value_type = std::pair<std::string, V>;

    V* find(std::string_view key) {
        for (auto& entry : entries_) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
    const V* find(std::string_view key) const { return const_cast<OrderedDict*>(this)->find(key); }
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    // d[key] = value: an existing key keeps its position.
    V& set(std::string key, V value) {
        if (V* existing = find(key)) {
            *existing = std::move(value);
            return *existing;
        }
        entries_.emplace_back(std::move(key), std::move(value));
        return entries_.back().second;
    }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    auto begin() { return entries_.begin(); }
    auto end() { return entries_.end(); }
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }

    friend bool operator==(const OrderedDict&, const OrderedDict&) = default;

private:
    std::vector<value_type> entries_;
};

// One entry on a dashboard card. created_at and updated_at are UTC ISO text ("...Z").
struct CardItem {
    std::string id;
    std::string card;
    std::string text;
    Json data = Json::object();
    std::string status = "proposed"; // proposed | edited | confirmed | removed
    std::string author_type = "agent";
    std::optional<std::string> author_id;
    bool pinned = false;
    std::int64_t revision = 1;
    std::vector<std::string> evidence; // segment ids
    std::string created_at;
    std::string updated_at;
    Json review = Json::object();
    Json citation_check = Json::object();

    // Python's `protected`: agent ops may no longer modify it (pinned, edited or confirmed, or
    // saved by a spoken request).
    bool is_protected() const;
    Json to_json() const;
    // A missing created_at or updated_at is stamped now from the clock, as Python does.
    static CardItem from_json(const Json& d, const Clock& clock = system_clock());

    friend bool operator==(const CardItem&, const CardItem&) = default;
};

// A quiet-inbox question with its optional suggestion and resolution. The confidences are kept
// as the JSON numbers given (null when unset).
struct Question {
    std::string id;
    std::string text;
    std::string status = "open"; // open | resolved | dismissed
    std::optional<std::string> suggested_answer;
    Json suggested_confidence;
    std::optional<std::string> answer;
    std::optional<std::string> answer_source; // user | audio
    Json confidence;
    Json thread = Json::array();
    std::vector<std::string> evidence;
    std::string asked_at;
    std::optional<std::string> resolved_at;
    std::optional<std::string> resolved_by;

    Json to_json() const;
    static Question from_json(const Json& d, const Clock& clock = system_clock());

    friend bool operator==(const Question&, const Question&) = default;
};

// One report a participant asked for in their own words; created "running" and finished in
// place, so its id never changes.
struct CustomReport {
    std::string id;
    std::string request;
    std::string status = "running";
    std::string title;
    std::string markdown;
    std::string message;
    std::string run_id;
    Json sources = Json::object();
    std::optional<std::string> requested_by;
    std::string created_at;
    std::string updated_at;

    Json to_json() const;
    // An unknown status reads as "failed".
    static CustomReport from_json(const Json& d, const Clock& clock = system_clock());

    friend bool operator==(const CustomReport&, const CustomReport&) = default;
};

// The host ("me"), a diarized remote-speaker cluster or a joined dashboard guest.
struct Participant {
    std::string id;
    std::string display_name;
    std::string kind = "others_cluster"; // me | others_cluster | guest
    std::string name_source = "default"; // default | agent_inferred | human
    bool is_provisional = false;
    std::string created_at;
    std::string updated_at;

    Json to_json() const;
    static Participant from_json(const Json& d, const Clock& clock = system_clock());

    friend bool operator==(const Participant&, const Participant&) = default;
};

// The evolving main topic; history entries are {text, ts, evidence, actor_type}.
struct TopicState {
    std::string current;
    Json history = Json::array();

    Json to_json() const;
    static TopicState from_json(const Json& d);

    friend bool operator==(const TopicState&, const TopicState&) = default;
};

// The host's standing brief for the meeting's notes. author_id is the participant who last
// wrote it; empty when seeded before participants exist.
struct MeetingIntent {
    std::string text;
    std::string updated_at;
    std::string author_id;

    Json to_json() const;
    // Anything but an object reads as an empty brief.
    static MeetingIntent from_json(const Json& d);

    friend bool operator==(const MeetingIntent&, const MeetingIntent&) = default;
};

// Optional post-meeting consolidation outcome. card_deferred only hides the desktop card.
struct FinalizationState {
    std::string status = "pending";
    std::string message;
    std::string stage;
    std::int64_t current_step = 0;
    std::int64_t total_steps = 0;
    std::string step_details;
    Json steps = Json::array(); // objects
    Json summary_stats = Json::object();
    bool card_deferred = false;

    Json to_json() const;

    // (label, tone) for the history list's pill; nullopt for live meetings (active, paused,
    // ending) and in-flight work.
    std::optional<std::pair<std::string, std::string>>
    history_pill(std::string_view meeting_status = "") const;

    // For a new meeting: "pending" with AI insights on, else "disabled".
    static FinalizationState default_for_cloud(bool cloud_enabled);
    // For snapshots that predate the field; never claims a success that was not recorded.
    static FinalizationState infer_legacy(bool cloud_enabled, std::string_view meeting_status);
    // A persisted or API value; an unknown shape or status falls back to infer_legacy. Throws
    // py::ValueError or py::TypeError where Python's int() and dict() do.
    static FinalizationState coerce(const Json& value, bool cloud_enabled = false,
                                    std::string_view meeting_status = "active");
    static FinalizationState coerce(const FinalizationState& value, bool cloud_enabled = false,
                                    std::string_view meeting_status = "active");
    // For archived or stored views: an interrupted pending or running value of a terminal
    // meeting (ended, failed, needs_recovery) becomes a durable outcome.
    static FinalizationState normalize_historical(const Json& value, bool cloud_enabled,
                                                  std::string_view meeting_status);

    friend bool operator==(const FinalizationState&, const FinalizationState&) = default;
};

// The desktop-card deferral flag: a bool, a number (non-zero), or "1", "true", "yes" in any
// case and surrounding space; anything else is false.
bool coerce_card_deferred(const Json& value);

// The cards of a document, always CARD_KEYS in order.
class Cards {
public:
    std::vector<CardItem>& operator[](std::size_t index) { return cards_.at(index); }
    const std::vector<CardItem>& operator[](std::size_t index) const { return cards_.at(index); }
    // Throws std::out_of_range for a name outside CARD_KEYS.
    std::vector<CardItem>& operator[](std::string_view card);
    const std::vector<CardItem>& operator[](std::string_view card) const;

    friend bool operator==(const Cards&, const Cards&) = default;

private:
    std::array<std::vector<CardItem>, CARD_KEYS.size()> cards_;
};

// The complete dashboard document for one meeting.
struct MeetingState {
    std::string meeting_id;
    std::int64_t seq = 0;
    std::string status = "active"; // active | paused | ending | ended | failed | needs_recovery
    bool cloud_enabled = false;
    bool intelligence_online = false;
    bool diarization_available = false;
    std::string diarization_reason; // why speaker separation is unavailable
    Json speech = Json::object();
    std::string title;
    MeetingIntent intent;
    TopicState topic;
    std::string rolling_summary;
    std::vector<std::string> rolling_summary_evidence;
    Json capture = default_capture();
    OrderedDict<Participant> participants;
    Cards cards;
    OrderedDict<Question> questions;
    FinalizationState finalization;
    std::vector<std::string> report_views = {"ribbon", "brief", "signal"};
    Json insight_review = Json::object();
    Json live_highlights = Json::array();
    std::string live_highlights_status = "unknown";
    Json voice_feedback = Json::object();
    std::vector<CustomReport> custom_reports; // oldest first

    // {mic_available, loopback_available, message, audio_incomplete, integrity_error}.
    static Json default_capture();

    CustomReport* find_custom_report(std::string_view report_id);
    // Across all cards, in card order.
    CardItem* find_item(std::string_view item_id);
    const CardItem* find_item(std::string_view item_id) const;
    std::size_t open_question_count() const;

    Json to_json() const;
    // A snapshot without "finalization" infers it (infer_legacy); unknown cards are dropped,
    // capture is rebuilt with its five keys, and an empty report_views takes the default.
    static MeetingState from_json(const Json& d, const Clock& clock = system_clock());

    friend bool operator==(const MeetingState&, const MeetingState&) = default;
};

} // namespace fv::meeting_state
