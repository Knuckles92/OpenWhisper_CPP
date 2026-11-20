#include "history/dictation_stats.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "base/log.h"
#include "history/detail/rows.h"
#include "history/detail/time.h"
#include "history/detail/time_range.h"
#include "history/manager.h"
#include "pyport/difflib.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/unicode.h"

namespace fv::dictation_stats {

namespace {

constexpr log::Logger logger("services.dictation_stats");

// A row that only says history was already counted (or stats were reset), so it is never
// counted again.
constexpr std::string_view marker_kind = "marker";
// The marker's target once files, walkthroughs and meetings were counted too; the dictation
// marker has none.
constexpr std::string_view features_marker = "features";

using history_detail::optional_integer;
using history_detail::optional_real;
using history_detail::optional_text;

// Chinese and Japanese are written without spaces, so each character counts as a word; Korean
// separates words with spaces like Latin scripts.
bool is_cjk(char32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& items, std::string_view item) {
    return std::ranges::find(items, item) != items.end();
}

bool counted_kind(std::string_view kind) {
    return contains(COUNTED_KINDS, kind);
}

// Python's truthiness of an optional str.
bool truthy(const std::optional<std::string>& value) {
    return value && !value->empty();
}

bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string() || value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

const std::optional<std::string>& first_truthy(const std::optional<std::string>& a,
                                               const std::optional<std::string>& b) {
    return truthy(a) ? a : b;
}

std::optional<std::string> or_none(const std::optional<std::string>& value) {
    return truthy(value) ? value : std::nullopt;
}

// _count: a non-negative int from an int or float (a bool is an int in Python); anything else 0.
std::int64_t count_of(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 1 : 0;
    }
    if (value.is_number_integer()) {
        return std::max<std::int64_t>(0, value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw py::OverflowError("Python int too large to convert to SQLite INTEGER");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (std::isnan(number)) {
            throw py::ValueError("cannot convert float NaN to integer");
        }
        if (std::isinf(number)) {
            throw py::OverflowError("cannot convert float infinity to integer");
        }
        const double whole = std::trunc(number);
        if (whole >= 9.223372036854775808e18) {
            // Python keeps the int and fails when SQLite is handed it.
            throw py::OverflowError("Python int too large to convert to SQLite INTEGER");
        }
        return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::max(whole, -1.0)));
    }
    return 0;
}

// str(value): the forms a target can take in a job's counts.
std::string python_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    return value.dump();
}

std::optional<py::DateTime> parse_stored(std::optional<std::string_view> timestamp) {
    std::string text(timestamp.value_or(""));
    for (auto at = text.find('Z'); at != std::string::npos; at = text.find('Z', at + 6)) {
        text.replace(at, 1, "+00:00");
    }
    try {
        return py::fromisoformat(text);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
}

// _utc: a stored timestamp as microseconds since the epoch; a naive one is read as UTC.
std::optional<std::int64_t> utc_micros(const std::optional<std::string>& timestamp) {
    const auto moment =
        parse_stored(timestamp ? std::optional<std::string_view>(*timestamp) : std::nullopt);
    if (!moment) {
        return std::nullopt;
    }
    return history_detail::epoch_micros(*moment);
}

StatRow marker(const Clock& clock, std::optional<std::string> target = std::nullopt) {
    StatRow row;
    row.timestamp = history_detail::utc_now(clock).isoformat();
    row.entry_kind = std::string(marker_kind);
    row.target = std::move(target);
    return row;
}

sql::Value text_value(const std::optional<std::string>& value) {
    if (value) {
        return *value;
    }
    return nullptr;
}

void insert(sql::Connection& conn, const StatRow& row) {
    sql::Statement statement = conn.prepare(
        "INSERT INTO dictation_stats (entry_id, timestamp, entry_kind, app_name, app_category, "
        "cleanup_level, spoken_words, final_words, words_edited, audio_seconds, language, target, "
        "snippets, snippet_words, corrections, shots) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?)");
    int index = 1;
    statement.bind(index++, text_value(row.entry_id));
    statement.bind(index++, row.timestamp);
    statement.bind(index++, row.entry_kind);
    statement.bind(index++, text_value(row.app_name));
    statement.bind(index++, text_value(row.app_category));
    statement.bind(index++, text_value(row.cleanup_level));
    statement.bind(index++, row.spoken_words);
    statement.bind(index++, row.final_words);
    statement.bind(index++, row.words_edited);
    statement.bind(index++,
                   row.audio_seconds ? sql::Value(*row.audio_seconds) : sql::Value(nullptr));
    statement.bind(index++, text_value(row.language));
    statement.bind(index++, text_value(row.target));
    statement.bind(index++, row.snippets);
    statement.bind(index++, row.snippet_words);
    statement.bind(index++, row.corrections);
    statement.bind(index++, row.shots);
    statement.step();
}

std::set<std::string> counted_ids(sql::Connection& conn) {
    std::set<std::string> counted;
    sql::Statement statement = conn.prepare(
        "SELECT dictation_stats.entry_id FROM dictation_stats WHERE dictation_stats.entry_id IS "
        "NOT NULL");
    while (statement.step()) {
        if (auto id = optional_text(statement.column(0))) {
            counted.insert(std::move(*id));
        }
    }
    return counted;
}

std::vector<HistoryEntry> history_where(sql::Connection& conn, std::string_view where) {
    sql::Statement statement =
        conn.prepare(history_detail::select_columns() + " WHERE " + std::string(where));
    std::vector<HistoryEntry> entries;
    while (statement.step()) {
        entries.push_back(history_detail::read_entry(statement));
    }
    return entries;
}

// _history_features: rows for the files and walkthroughs already in History.
int history_features(sql::Connection& conn, const std::set<std::string>& counted,
                     const Clock& clock) {
    const auto history = history_where(
        conn, "transcription_history.origin_device_id IS NULL AND "
              "(transcription_history.entry_kind IN ('file', 'walkthrough') OR "
              "transcription_history.entry_kind IS NULL AND "
              "(transcription_history.source_name IS NULL OR transcription_history.source_name "
              "NOT LIKE 'Quick Record%'))");
    int added = 0;
    for (const HistoryEntry& entry : history) {
        if (counted.contains(entry.id) || entry.entry_kind == FAILED_RECORDING_KIND) {
            continue;
        }
        const std::string source = entry.source_name.value_or("");
        if (source.starts_with("recording_") && source.ends_with(".wav")) {
            // One of the app's own recordings, transcribed again.
            continue;
        }
        if (const auto row = stat(EntryFields::of(entry), clock)) {
            insert(conn, *row);
            ++added;
        }
    }
    return added;
}

std::optional<StatRow> meeting_stat(sql::Connection& conn, const std::string& meeting_id,
                                    const std::optional<std::string>& started,
                                    const std::optional<std::string>& ended,
                                    std::optional<double> paused) {
    const auto start = utc_micros(started);
    const auto end = utc_micros(ended);
    if (!start || !end) {
        return std::nullopt;
    }
    sql::Statement statement = conn.prepare(
        "SELECT meeting_segments.text FROM meeting_segments WHERE meeting_segments.meeting_id = ?");
    statement.bind(1, meeting_id);
    std::int64_t spoken = 0;
    while (statement.step()) {
        const auto text = optional_text(statement.column(0));
        spoken += static_cast<std::int64_t>(
            count_words(text ? std::optional<std::string_view>(*text) : std::nullopt));
    }
    StatRow row;
    row.entry_id = meeting_id;
    row.timestamp = started.value_or("");
    row.entry_kind = std::string(MEETING_KIND);
    row.final_words = spoken;
    const double seconds = static_cast<double>(*end - *start) / 1e6;
    row.audio_seconds = std::max(0.0, seconds - paused.value_or(0.0));
    return row;
}

} // namespace

struct StatsRepository::Listeners {
    struct Entry {
        std::uint64_t id;
        std::shared_ptr<CallbackGuard> guard;
        std::shared_ptr<std::function<void()>> fn;
    };
    std::mutex mutex;
    std::vector<Entry> entries;
    std::uint64_t next_id = 0;
};

std::int64_t next_milestone(std::int64_t words) {
    for (const std::int64_t milestone : MILESTONES) {
        if (milestone > words) {
            return milestone;
        }
    }
    // Python's // floors.
    const std::int64_t millions =
        words >= 0 ? words / 1'000'000 : -((-words + 999'999) / 1'000'000);
    return (millions + 1) * 1'000'000;
}

std::vector<std::string> words(std::optional<std::string_view> text) {
    const std::u32string points = py::to_code_points(text.value_or(""));
    std::vector<std::string> found;
    std::size_t at = 0;
    while (at < points.size()) {
        const char32_t cp = points[at];
        std::u32string token;
        if (is_cjk(cp)) {
            token.push_back(cp);
            ++at;
        } else if (py::is_space(cp)) {
            ++at;
            continue;
        } else {
            while (at < points.size() && !py::is_space(points[at]) && !is_cjk(points[at])) {
                token.push_back(points[at++]);
            }
        }
        if (std::ranges::any_of(token, [](char32_t c) { return py::is_word(c); })) {
            found.push_back(py::to_utf8(token));
        }
    }
    return found;
}

std::size_t count_words(std::optional<std::string_view> text) {
    return words(text).size();
}

std::int64_t words_edited(std::optional<std::string_view> before,
                          std::optional<std::string_view> after) {
    std::vector<std::string> old_words = words(before);
    std::vector<std::string> new_words = words(after);
    if (old_words == new_words) {
        return 0;
    }
    py::WordSequenceMatcher matcher(std::move(old_words), std::move(new_words), false);
    std::int64_t edited = 0;
    for (const py::Opcode& op : matcher.get_opcodes()) {
        const auto deleted = static_cast<std::int64_t>(op.i2 - op.i1);
        const auto inserted = static_cast<std::int64_t>(op.j2 - op.j1);
        switch (op.tag) {
        case py::OpTag::Replace:
            edited += std::max(deleted, inserted);
            break;
        case py::OpTag::Delete:
            edited += deleted;
            break;
        case py::OpTag::Insert:
            edited += inserted;
            break;
        case py::OpTag::Equal:
            break;
        }
    }
    return edited;
}

std::optional<py::DateTime> local_time(std::optional<std::string_view> timestamp,
                                       const std::chrono::time_zone& zone) {
    auto moment = parse_stored(timestamp);
    if (!moment) {
        return std::nullopt;
    }
    if (moment->utc_offset_us) {
        // astimezone() moves the value to UTC first, so a moment whose UTC lies outside years
        // 1..9999 overflows whatever the target zone.
        const auto instant = history_detail::utc_instant(*moment);
        const auto local = history_detail::utc_in_range(instant)
                               ? history_detail::to_zone(instant, zone)
                               : std::nullopt;
        if (!local) {
            throw py::OverflowError("date value out of range");
        }
        return local;
    }
    return moment;
}

EntryFields EntryFields::of(const HistoryEntry& entry) {
    EntryFields fields;
    fields.id = entry.id;
    fields.timestamp = entry.timestamp;
    fields.text = entry.text;
    fields.raw_text = entry.raw_text;
    fields.cleaned_text = entry.cleaned_text;
    fields.audio_duration = entry.audio_duration;
    fields.entry_kind = entry.entry_kind;
    fields.source_name = entry.source_name;
    fields.app_name = entry.app_name;
    fields.app_category = entry.app_category;
    fields.cleanup_level = entry.cleanup_level;
    fields.language = entry.language;
    fields.origin_device_id = entry.origin_device_id;
    return fields;
}

EntryFields EntryFields::of(const AddEntry& added) {
    EntryFields fields;
    fields.text = added.text;
    fields.raw_text = added.raw_text;
    fields.audio_duration = added.audio_duration;
    fields.source_name = added.source_name;
    fields.spoken_text = added.spoken_text;
    fields.stats_counts = added.stats_counts;
    const auto column = [&](std::string_view name) -> std::optional<std::string> {
        const auto found = added.columns.find(name);
        return found != added.columns.end() ? found->second : std::nullopt;
    };
    fields.cleaned_text = column("cleaned_text");
    fields.entry_kind = column("entry_kind");
    fields.app_name = column("app_name");
    fields.app_category = column("app_category");
    fields.cleanup_level = column("cleanup_level");
    fields.language = column("language");
    return fields;
}

std::string kind(const EntryFields& fields) {
    if (truthy(fields.entry_kind)) {
        return *fields.entry_kind;
    }
    const std::string source = fields.source_name.value_or("");
    return source.starts_with("Quick Record") ? "dictation" : "file";
}

std::optional<StatRow> stat(const EntryFields& fields, const Clock& clock) {
    const std::string entry_kind = kind(fields);
    const Json counts = fields.stats_counts.is_object() && !fields.stats_counts.empty()
                            ? fields.stats_counts
                            : Json::object();
    const auto count = [&](std::string_view name) -> Json {
        const auto found = counts.find(std::string(name));
        return found != counts.end() ? *found : Json(nullptr);
    };
    if (!(counted_kind(entry_kind) || entry_kind == FILE_KIND || entry_kind == WALKTHROUGH_KIND) ||
        truthy(fields.origin_device_id)) {
        return std::nullopt;
    }
    if (entry_kind == FILE_KIND && truthy(count("retranscription"))) {
        // The recording was counted when it was first made.
        return std::nullopt;
    }
    const std::string text = fields.text.value_or("");
    const std::optional<std::string>& raw = fields.raw_text;
    const std::string ai_version = truthy(fields.cleaned_text) ? *fields.cleaned_text : text;

    StatRow row;
    if (entry_kind == "dictation") {
        const std::optional<std::string>& said = first_truthy(fields.spoken_text, raw);
        row.spoken_words = static_cast<std::int64_t>(
            count_words(truthy(said) ? std::string_view(*said) : std::string_view(text)));
    }
    if (entry_kind == "dictation" || contains(FEATURE_KINDS, entry_kind)) {
        row.audio_seconds = fields.audio_duration;
    }
    row.entry_id = fields.id;
    row.timestamp =
        truthy(fields.timestamp) ? *fields.timestamp : history_detail::utc_now(clock).isoformat();
    row.entry_kind = entry_kind;
    row.app_name = or_none(fields.app_name);
    row.app_category = or_none(fields.app_category);
    row.cleanup_level = or_none(fields.cleanup_level);
    row.final_words = static_cast<std::int64_t>(count_words(text));
    row.words_edited = truthy(raw) && counted_kind(entry_kind) ? words_edited(*raw, ai_version) : 0;
    row.language = or_none(fields.language);
    if (const Json target = count("target"); truthy(target)) {
        if (std::string named = python_str(target); !named.empty()) {
            row.target = std::move(named);
        }
    }
    row.snippets = count_of(count("snippets"));
    row.snippet_words = count_of(count("snippet_words"));
    row.corrections = count_of(count("corrections"));
    row.shots = count_of(count("shots"));
    return row;
}

StatsRepository::StatsRepository(Database& database, const Clock& clock)
    : database_(&database), clock_(&clock), listeners_(std::make_shared<Listeners>()) {}

void StatsRepository::record(const EntryFields& entry_fields, const HistoryEntry* row) {
    EntryFields fields = entry_fields;
    if (row != nullptr) {
        const EntryFields saved = EntryFields::of(*row);
        const auto take = [](auto& into, const auto& value) {
            if (value) {
                into = value;
            }
        };
        take(fields.id, saved.id);
        take(fields.timestamp, saved.timestamp);
        take(fields.text, saved.text);
        take(fields.raw_text, saved.raw_text);
        take(fields.cleaned_text, saved.cleaned_text);
        take(fields.audio_duration, saved.audio_duration);
        take(fields.entry_kind, saved.entry_kind);
        take(fields.source_name, saved.source_name);
        take(fields.app_name, saved.app_name);
        take(fields.app_category, saved.app_category);
        take(fields.cleanup_level, saved.cleanup_level);
        take(fields.language, saved.language);
        take(fields.origin_device_id, saved.origin_device_id);
    }
    const auto made = stat(fields, *clock_);
    if (!made) {
        return;
    }
    const bool added = [&] {
        std::lock_guard lock(write_mutex_);
        return database_->write([&](sql::Connection& conn) {
            sql::Transaction tx = conn.begin();
            if (truthy(made->entry_id)) {
                sql::Statement existing =
                    conn.prepare("SELECT dictation_stats.id FROM dictation_stats WHERE "
                                 "dictation_stats.entry_id = ? LIMIT 1 OFFSET 0");
                existing.bind(1, *made->entry_id);
                if (existing.step()) {
                    return false;
                }
            }
            insert(conn, *made);
            tx.commit();
            return true;
        });
    }();
    if (!added) {
        return;
    }
    std::vector<Listeners::Entry> entries;
    {
        std::lock_guard lock(listeners_->mutex);
        entries = listeners_->entries;
    }
    for (const auto& entry : entries) {
        entry.guard->run([&] { (*entry.fn)(); });
    }
}

void StatsRepository::ensure_backfilled() {
    int added = 0;
    {
        std::lock_guard lock(write_mutex_);
        const bool done = database_->write([&](sql::Connection& conn) {
            sql::Transaction tx = conn.begin();
            sql::Statement statement = conn.prepare(
                "SELECT dictation_stats.id FROM dictation_stats WHERE dictation_stats.entry_kind "
                "= ? AND dictation_stats.target IS NULL LIMIT 1 OFFSET 0");
            statement.bind(1, std::string(marker_kind));
            if (statement.step()) {
                return true;
            }
            const std::set<std::string> counted = counted_ids(conn);
            const auto history =
                history_where(conn, "transcription_history.origin_device_id IS NULL AND "
                                    "(transcription_history.entry_kind = 'dictation' OR "
                                    "transcription_history.entry_kind IS NULL AND "
                                    "transcription_history.source_name LIKE 'Quick Record%')");
            for (const HistoryEntry& entry : history) {
                if (counted.contains(entry.id)) {
                    continue;
                }
                if (const auto row = stat(EntryFields::of(entry), *clock_)) {
                    insert(conn, *row);
                    ++added;
                }
            }
            insert(conn, marker(*clock_));
            tx.commit();
            return false;
        });
        if (done) {
            return;
        }
    }
    logger.info("Counted {} earlier dictations for Stats", added);
}

void StatsRepository::count_features() {
    int added = 0;
    {
        std::lock_guard lock(write_mutex_);
        database_->write([&](sql::Connection& conn) {
            sql::Transaction tx = conn.begin();
            sql::Statement found = conn.prepare(
                "SELECT dictation_stats.timestamp FROM dictation_stats WHERE "
                "dictation_stats.entry_kind = ? AND dictation_stats.target = ? ORDER BY "
                "dictation_stats.id DESC LIMIT 1 OFFSET 0");
            found.bind(1, std::string(marker_kind));
            found.bind(2, std::string(features_marker));
            const bool has_marker = found.step();
            std::optional<std::int64_t> since;
            if (has_marker) {
                since = utc_micros(optional_text(found.column(0)));
            }
            const std::set<std::string> counted = counted_ids(conn);
            if (!has_marker) {
                added += history_features(conn, counted, *clock_);
            }
            struct Meeting {
                std::string id;
                std::optional<std::string> started;
                std::optional<std::string> ended;
                std::optional<double> paused;
            };
            std::vector<Meeting> meetings;
            {
                sql::Statement statement = conn.prepare(
                    "SELECT meeting_sessions.id, meeting_sessions.started_at, "
                    "meeting_sessions.ended_at, meeting_sessions.paused_total_s FROM "
                    "meeting_sessions WHERE meeting_sessions.origin_device_id IS NULL AND "
                    "meeting_sessions.ended_at IS NOT NULL AND (meeting_sessions.status NOT IN "
                    "('active', 'paused', 'ending'))");
                while (statement.step()) {
                    meetings.push_back({optional_text(statement.column(0)).value_or(""),
                                        optional_text(statement.column(1)),
                                        optional_text(statement.column(2)),
                                        optional_real(statement.column(3))});
                }
            }
            for (const Meeting& meeting : meetings) {
                if (counted.contains(meeting.id)) {
                    continue;
                }
                const auto finished = utc_micros(meeting.ended);
                if (since && (!finished || *finished <= *since)) {
                    continue;
                }
                if (const auto row = meeting_stat(conn, meeting.id, meeting.started, meeting.ended,
                                                  meeting.paused)) {
                    insert(conn, *row);
                    ++added;
                }
            }
            if (!has_marker) {
                insert(conn, marker(*clock_, std::string(features_marker)));
            }
            tx.commit();
        });
    }
    if (added != 0) {
        logger.info("Counted {} files, walkthroughs and meetings for Stats", added);
    }
}

void StatsRepository::reset() {
    {
        std::lock_guard lock(write_mutex_);
        database_->write([&](sql::Connection& conn) {
            sql::Transaction tx = conn.begin();
            conn.exec("DELETE FROM dictation_stats");
            insert(conn, marker(*clock_));
            insert(conn, marker(*clock_, std::string(features_marker)));
            tx.commit();
        });
    }
    logger.info("Stats reset");
}

Subscription StatsRepository::add_listener(std::function<void()> listener) {
    auto guard = std::make_shared<CallbackGuard>();
    std::uint64_t id = 0;
    {
        std::lock_guard lock(listeners_->mutex);
        id = listeners_->next_id++;
        listeners_->entries.push_back(
            {id, guard, std::make_shared<std::function<void()>>(std::move(listener))});
    }
    std::weak_ptr<Listeners> weak = listeners_;
    return Subscription(guard, [weak, id] {
        if (auto listeners = weak.lock()) {
            std::lock_guard lock(listeners->mutex);
            std::erase_if(listeners->entries,
                          [id](const Listeners::Entry& entry) { return entry.id == id; });
        }
    });
}

std::vector<StatRow> StatsRepository::rows() {
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(
            "SELECT id, entry_id, timestamp, entry_kind, app_name, app_category, cleanup_level, "
            "spoken_words, final_words, words_edited, audio_seconds, language, target, snippets, "
            "snippet_words, corrections, shots FROM dictation_stats ORDER BY id");
        std::vector<StatRow> found;
        while (statement.step()) {
            int column = 0;
            const auto next = [&] {
                return statement.column(column++);
            };
            StatRow row;
            row.id = optional_integer(next());
            row.entry_id = optional_text(next());
            row.timestamp = optional_text(next()).value_or("");
            row.entry_kind = optional_text(next()).value_or("");
            row.app_name = optional_text(next());
            row.app_category = optional_text(next());
            row.cleanup_level = optional_text(next());
            row.spoken_words = optional_integer(next()).value_or(0);
            row.final_words = optional_integer(next()).value_or(0);
            row.words_edited = optional_integer(next()).value_or(0);
            row.audio_seconds = optional_real(next());
            row.language = optional_text(next());
            row.target = optional_text(next());
            row.snippets = optional_integer(next()).value_or(0);
            row.snippet_words = optional_integer(next()).value_or(0);
            row.corrections = optional_integer(next()).value_or(0);
            row.shots = optional_integer(next()).value_or(0);
            found.push_back(std::move(row));
        }
        return found;
    });
}

} // namespace fv::dictation_stats
