#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/env.h"
#include "base/json.h"
#include "pyport/errors.h"
#include "sql/db.h"

// Bounded, read-only retrieval over the history database (services/agent_api/store.py), shared
// by the History API routes and, through them, the MCP history tools. Search is literal
// substring matching (SQLite's ASCII case folding), independent of FTS indexes or models. Every
// call opens its own read-only connection (mode=ro, query_only, 5 s busy timeout) and closes it,
// as SQLAlchemy's NullPool did, so the store never holds the database open between calls and
// never creates, migrates or writes it. The SQL is the text SQLAlchemy emits for store.py's
// queries, parameters included, so ordering, comparison and NULL rules are SQLite's own.
//
// Results are the response models' JSON (history_models.h). sql::Error escapes as it is; the
// routes turn it into "database_unavailable" without its message.
namespace fv::agentapi {

// A query the store refuses; the message is shown to the client (400 invalid_query).
class InvalidQuery : public py::ValueError {
public:
    explicit InvalidQuery(const std::string& message) : py::ValueError(message) {}
};

// No visible record with that id (404 not_found, "Record not found.").
class NotFound : public std::runtime_error {
public:
    explicit NotFound(const std::string& message = "Record not found.")
        : std::runtime_error(message) {}
};

// A meeting's saved state cannot be read (503 snapshot_unavailable).
class SnapshotUnavailable : public py::ValueError {
public:
    explicit SnapshotUnavailable(const std::string& message) : py::ValueError(message) {}
};

// transcriptions() and meetings(). `since` and `before` are UTC isoformat text, as the route's
// _filters makes them; `origin_device_id` restricts to one paired computer's stored copies.
struct ListQuery {
    std::optional<std::string> q; // a non-empty q matches literally
    std::int64_t limit = 20;
    std::optional<std::string> cursor;
    bool include_remote = false;
    std::optional<std::string> since;
    std::optional<std::string> before;
    std::optional<std::string> origin_device_id;
};

// segments(): start_s inclusive and end_s exclusive, both compared with each segment's start_s.
struct SegmentQuery {
    std::string meeting_id;
    std::optional<double> start_s;
    std::optional<double> end_s;
    std::int64_t limit = 20;
    std::optional<std::string> cursor;
    bool include_remote = false;
};

// search(): kind is "all", "transcription" or "meeting".
struct SearchQuery {
    std::string q;
    std::string kind = "all";
    std::int64_t limit = 20;
    std::optional<std::string> cursor;
    bool include_remote = false;
    std::optional<std::string> since;
    std::optional<std::string> before;
    std::optional<std::string> origin_device_id;
};

// The connection every store call reads through: the file opened mode=ro, then PRAGMA
// query_only=ON; a write through it fails with SQLITE_READONLY.
sql::Connection open_read_only(const std::filesystem::path& database);

// Affinity: one call at a time per thread; separate threads may call at once (each call has its
// own connection).
class HistoryStore {
public:
    // Opens nothing that stays open. Throws std::invalid_argument "Database does not exist. Open
    // <product> first or select --database." for a path that is not a file, and "Database schema
    // is incompatible. Open it with this version of <product> first." for another schema version;
    // sql::Error when the file is not a database or lacks the history tables.
    explicit HistoryStore(std::filesystem::path database); // read-only; throws invalid_argument
    // Path(database).expanduser() reads "~" from `env` (the process environment above).
    HistoryStore(std::filesystem::path database, const EnvReader& env);

    // A database probe: throws sql::Error when history cannot be read.
    void check();

    // Page[TranscriptionSummary], newest first (sort_time DESC, id ASC). q matches text,
    // raw_text, source_name or title.
    Json transcriptions(const ListQuery&);
    // Transcription; throws NotFound for a missing row or a remote copy without include_remote.
    Json transcription(std::string_view id, bool include_remote = false);
    // Page[Meeting], newest first by started_at; q matches the title.
    Json meetings(const ListQuery&);
    Json meeting(std::string_view id, bool include_remote = false);
    // Page[Segment] in time order (start_s ASC, id ASC); the meeting must be visible first.
    Json segments(const SegmentQuery&);
    Json segment(std::string_view meeting_id, std::string_view segment_id,
                 bool include_remote = false);
    // MeetingInsights from the saved state_json. Throws SnapshotUnavailable when it cannot be
    // read.
    Json insights(std::string_view meeting_id, bool include_remote = false);
    // Page[SearchHit] over dictations, meeting titles and transcript segments, newest first
    // (sort_time DESC, kind ASC, id ASC), each hit with the resource path that fetches it.
    Json search(const SearchQuery&);

    const std::filesystem::path& database() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace fv::agentapi
