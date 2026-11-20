#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/database.h"
#include "history/entry.h"
#include "sql/db.h"

// The history queries of services/database.py (DatabaseManager, lines 413-614), as the SQL
// SQLAlchemy emits for them. Rows come back as value copies.
// Affinity: thread-safe; blocking, never on the GUI thread.
namespace fv {

// database.py _origin_filter: every entry (Python's `...`), this computer's own (None), or the
// entries one paired computer stored here.
struct HistoryOrigin {
    enum class Kind { Any, Own, Device };
    Kind kind = Kind::Any;
    std::string device_id;

    static HistoryOrigin any() { return {}; }
    static HistoryOrigin own() { return {Kind::Own, {}}; }
    static HistoryOrigin device(std::string id) { return {Kind::Device, std::move(id)}; }
};

// One history_calendar_rows() tuple: what the calendar needs to place an entry on its day.
struct HistoryCalendarRow {
    std::string id;
    std::string timestamp;
    std::optional<std::string> source_name;
    std::optional<std::string> origin_device_name;
    std::optional<double> audio_duration;
    std::optional<std::string> entry_kind;

    friend bool operator==(const HistoryCalendarRow&, const HistoryCalendarRow&) = default;
};

// The entries one paired computer stored here (history_origin_counts). name is the greatest
// origin_device_name among them, "" when every one is NULL: the record kinds show it as the
// computer's name.
struct OriginCount {
    std::string device_id;
    std::string name;
    std::int64_t count = 0;

    friend bool operator==(const OriginCount&, const OriginCount&) = default;
};

// Columns and their new values for complete_failed_recording, as Python's `fields` dict.
using HistoryColumnValues = std::vector<std::pair<std::string, sql::Value>>;
// Keyword arguments of update_history_entry: a name with no value is None.
using HistoryEdits = std::map<std::string, std::optional<std::string>, std::less<>>;

class HistoryRepository {
public:
    explicit HistoryRepository(Database& database) : database_(&database) {}

    // add_history_entry: the entry's id, text, timestamp, model, audio and timing fields,
    // raw_text, cleanup provider and model, source_name and its context columns. Its title and
    // origin fields are not written (Python's signature has none).
    void add_history_entry(const HistoryEntry& entry);
    // add_history_entry with **context: throws py::TypeError before writing anything when a key
    // is not a context column; None values stay NULL.
    void add_history_entry(const HistoryEntry& entry, const ContextFields& context);

    // Newest first by the timestamp string; a limit of 0 or none means every entry.
    std::vector<HistoryEntry> get_history_entries(std::optional<std::int64_t> limit = std::nullopt,
                                                  const HistoryOrigin& origin = {});
    // Case-insensitive (SQLite's ASCII lower()) substring search over text, raw_text, the
    // stored timestamp string, source_name, title, origin_device_name and app_name; "\", "%"
    // and "_" match literally. The query is stripped first; an empty one lists every entry.
    // Newest first, limited as get_history_entries.
    std::vector<HistoryEntry> search_history_entries(std::string_view query,
                                                     std::optional<std::int64_t> limit = {},
                                                     const HistoryOrigin& origin = {});
    // Entries whose stored timestamp sorts in [start, end), oldest first.
    std::vector<HistoryEntry> get_history_entries_between(std::string_view start,
                                                          std::string_view end);
    std::vector<HistoryCalendarRow> history_calendar_rows();
    std::optional<HistoryEntry> get_history_entry_by_id(std::string_view entry_id);
    // The entry only when it belongs to `origin` (what the record kinds check after a lookup:
    // entry.origin_device_id == origin).
    std::optional<HistoryEntry> get_history_entry_by_id(std::string_view entry_id,
                                                        const HistoryOrigin& origin);
    // The first local failed-recording entry naming `audio_file`.
    std::optional<HistoryEntry> get_failed_recording(std::string_view audio_file);
    // Updates `fields` only while the entry is still a local failed recording; the entry as
    // saved now, or nullopt. Throws std::invalid_argument for a name that is not a column.
    std::optional<HistoryEntry> complete_failed_recording(std::string_view entry_id,
                                                          const HistoryColumnValues& fields);
    bool delete_history_entry(std::string_view entry_id);
    // EDITABLE_HISTORY_FIELDS ("text", "cleaned_text") only: py::TypeError("Not editable history
    // fields: ...") for any other name, py::TypeError("text can't be cleared") for a None text.
    // The updated entry, or nullopt when there is none.
    std::optional<HistoryEntry> update_history_entry(std::string_view entry_id,
                                                     const HistoryEdits& fields);
    // Insert or replace by id, every column (a record a paired computer stored).
    void put_history_entry(const HistoryEntry& entry);
    // Entries paired computers stored here, grouped by device (this computer's own, NULL, left
    // out), in SQLite's group order.
    std::vector<OriginCount> history_origin_counts();
    // Badges the entries `device_id` stored here with its new name; the number of entries.
    int rename_history_origin(std::string_view device_id, std::string_view name);
    // This computer's own entries only; entries paired computers stored here stay.
    void clear_history();
    // NULL the audio_file of every entry naming `audio_file`.
    void clear_history_audio_file(std::string_view audio_file);

private:
    Database* database_;
};

} // namespace fv
