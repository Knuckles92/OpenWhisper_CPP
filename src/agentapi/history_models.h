#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.h"

// The public v1 response contracts of the History API (services/agent_api/models.py), separate
// from database rows. to_json() writes pydantic's field order, which is the wire's key order; a
// float that is not finite is written as null, as pydantic's JSON mode writes it. Only these
// fields are ever written: whatever else a row or a saved meeting snapshot holds stays private.
// Affinity: values; any thread.
namespace fv::agentapi {

// A value pydantic's validation would refuse (a ValidationError). Raised while reading a saved
// meeting snapshot it becomes "snapshot_unavailable"; anywhere else it is a server error.
class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ClientStatus {
    std::string device_id;
    std::string name;
    std::string status; // online | sharing_disabled | unavailable
    Json to_json() const;
};

struct ApiStatus {
    std::vector<ClientStatus> clients;
    Json to_json() const; // {"api_version":"1","read_only":true,"clients":[...]}
};

struct TranscriptionSummary {
    std::string id;
    std::optional<std::string> device_id;
    std::optional<std::string> title;
    std::string timestamp;
    std::string model;
    std::optional<std::string> source_name;
    std::optional<double> audio_duration;
    std::optional<std::string> origin_device_id;
    std::optional<std::string> origin_device_name;
    std::string preview; // the first 320 characters of text
    Json to_json() const;
};

struct Transcription : TranscriptionSummary {
    std::string text;
    std::optional<std::string> raw_text;
    std::optional<std::string> cleanup_provider;
    std::optional<std::string> cleanup_model;
    Json to_json() const;
};

struct Meeting {
    std::string id;
    std::optional<std::string> device_id;
    std::string title;
    std::string status;
    std::string started_at;
    std::optional<std::string> ended_at;
    std::int64_t state_seq = 0;
    std::optional<std::string> origin_device_id;
    std::optional<std::string> origin_device_name;
    Json to_json() const;
};

struct Segment {
    std::string id;
    std::optional<std::string> device_id;
    std::string meeting_id;
    double start_s = 0.0;
    double end_s = 0.0;
    std::string channel;
    std::string text;
    std::optional<std::string> speaker_participant_id;
    std::string speaker_source;
    Json to_json() const;
};

struct Participant {
    std::string id;
    std::string display_name;
    std::string kind = "others_cluster";
    Json to_json() const;
};

// Allowlisted card metadata; future internal fields stay private.
struct InsightData {
    std::optional<std::string> heading;
    std::optional<double> start_s;
    std::optional<std::string> owner_participant_id;
    std::optional<std::string> due_date;
    std::optional<std::string> severity;
    Json to_json() const;
};

struct Insight {
    std::string id;
    std::string card;
    std::string text;
    std::string status = "proposed";
    bool pinned = false;
    InsightData data;
    std::vector<std::string> evidence;
    Json to_json() const;
};

struct Question {
    std::string id;
    std::string text;
    std::string status = "open";
    std::optional<std::string> answer;
    std::optional<std::string> answer_source;
    std::vector<std::string> evidence;
    Json to_json() const;
};

struct Report {
    std::string id;
    std::string title;
    std::string request;
    std::string status = "running";
    std::string markdown;
    Json to_json() const;
};

struct MeetingInsights {
    std::string meeting_id;
    std::optional<std::string> device_id;
    std::int64_t state_seq = 0;
    bool snapshot_available = false;
    std::string rolling_summary;
    std::vector<std::string> rolling_summary_evidence;
    std::vector<Participant> participants;
    std::vector<Insight> cards;
    std::vector<Question> questions;
    std::vector<Report> custom_reports;
    Json to_json() const;
};

struct SearchHit {
    std::string kind; // transcription | meeting | segment
    std::string id;
    std::optional<std::string> device_id;
    std::optional<std::string> origin_device_name;
    std::optional<std::string> meeting_id;
    std::string title;
    std::string timestamp;
    std::string excerpt;
    std::string matched_field; // text | raw_text | source_name | title
    std::optional<double> start_s;
    std::optional<double> end_s;
    std::optional<std::string> origin_device_id;
    std::string resource;
    Json to_json() const;
};

// Page[T]: {"items": [...], "next_cursor": ..., "clients": [...]}; `items` are to_json() values.
Json page_json(Json items, const std::optional<std::string>& next_cursor,
               const std::vector<ClientStatus>& clients = {});

// pydantic's model_validate of a value loaded from a saved snapshot (lax mode): a missing
// required field, a value of the wrong type, or a non-object throws ModelError. Unknown keys are
// dropped. Lax conversions: a bool from 0/1, 0.0/1.0 or "true"/"yes"/"on"/"t"/"y"/"1" (and their
// false forms) in any case; a float from a number, a bool or numeric text (whitespace around it,
// '_' between digits, inf and nan allowed); a str only from a string.
Participant participant_from(const Json&);
InsightData insight_data_from(const Json&);
Insight insight_from(const Json&); // the item with its "card" key set
Question question_from(const Json&);
Report report_from(const Json&);

} // namespace fv::agentapi
