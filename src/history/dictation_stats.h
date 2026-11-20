#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "base/subscription.h"
#include "db/database.h"
#include "history/entry.h"
#include "pyport/datetime.h"

// services/dictation_stats.py, the counting half: what a saved entry adds to the dictation_stats
// table, the one-time count of earlier history, finished meetings, and reset. Rows are local
// only and outlive Clear history and moves to a paired host. The Stats summary
// (load_summary) reads these rows; it is not here.
//
// Words are the words said: a transcript before AI cleanup, a snippet counted as its trigger
// (spoken_text). Commands, transforms and actions rewrite selected text, so they add no spoken
// words, but what the AI changed there counts as edited. Files, walkthroughs and meetings get a
// row each, apart from the words dictated.
namespace fv {
struct AddEntry;
} // namespace fv

namespace fv::dictation_stats {

// Entry kinds whose days keep the streak and whose words are words dictated.
inline constexpr std::array<std::string_view, 4> COUNTED_KINDS = {"dictation", "command",
                                                                  "transform", "action"};
inline constexpr std::string_view FILE_KIND = "file";
inline constexpr std::string_view WALKTHROUGH_KIND = "walkthrough";
inline constexpr std::string_view MEETING_KIND = "meeting";
// Kinds counted on a row of their own, never as words dictated.
inline constexpr std::array<std::string_view, 3> FEATURE_KINDS = {FILE_KIND, WALKTHROUGH_KIND,
                                                                  MEETING_KIND};
// Word counts worth a milestone; past the last, every further million.
inline constexpr std::array<std::int64_t, 9> MILESTONES = {
    1'000, 5'000, 10'000, 25'000, 50'000, 100'000, 250'000, 500'000, 1'000'000};

// The smallest milestone above `words`.
std::int64_t next_milestone(std::int64_t words);

// The words of `text`: runs of non-space characters, or one per Chinese or Japanese character
// (Hiragana, Katakana, CJK ideographs), each holding at least one \w character.
std::vector<std::string> words(std::optional<std::string_view> text);
std::size_t count_words(std::optional<std::string_view> text);
// Words replaced, inserted or deleted going from `before` to `after` (difflib opcodes over the
// word lists, autojunk off).
std::int64_t words_edited(std::optional<std::string_view> before,
                          std::optional<std::string_view> after);

// The naive local wall time of a stored timestamp ("Z" accepted), or nullopt when unreadable.
// Aware values are converted to `zone`; naive ones are local wall time already.
std::optional<py::DateTime> local_time(std::optional<std::string_view> timestamp,
                                       const std::chrono::time_zone& zone);

// The fields a stats row is made from: a history entry's columns, plus spoken_text and
// stats_counts from the save (Python's entry_fields dict; unset fields are None).
struct EntryFields {
    std::optional<std::string> id;
    std::optional<std::string> timestamp;
    std::optional<std::string> text;
    std::optional<std::string> raw_text;
    std::optional<std::string> cleaned_text;
    std::optional<double> audio_duration;
    std::optional<std::string> entry_kind;
    std::optional<std::string> source_name;
    std::optional<std::string> app_name;
    std::optional<std::string> app_category;
    std::optional<std::string> cleanup_level;
    std::optional<std::string> language;
    std::optional<std::string> origin_device_id;
    std::optional<std::string> spoken_text;
    // {"retranscription": bool, "target": str, "snippets", "snippet_words", "corrections",
    // "shots": counts}; anything but an object counts as {}.
    Json stats_counts = nullptr;

    // The columns a saved row carries (dictation_stats._ROW_FIELDS).
    static EntryFields of(const HistoryEntry& entry);
    // What HistoryManager::add_entry was given, with spoken_text and stats_counts.
    static EntryFields of(const AddEntry& fields);
};

// One dictation_stats row.
struct StatRow {
    std::optional<std::int64_t> id; // set once stored
    std::optional<std::string> entry_id;
    std::string timestamp;
    std::string entry_kind;
    std::optional<std::string> app_name;
    std::optional<std::string> app_category;
    std::optional<std::string> cleanup_level;
    std::int64_t spoken_words = 0;
    std::int64_t final_words = 0;
    std::int64_t words_edited = 0;
    std::optional<double> audio_seconds;
    std::optional<std::string> language;
    std::optional<std::string> target;
    std::int64_t snippets = 0;
    std::int64_t snippet_words = 0;
    std::int64_t corrections = 0;
    std::int64_t shots = 0;

    friend bool operator==(const StatRow&, const StatRow&) = default;
};

// _kind: entry_kind when set, else "dictation" for a source_name starting "Quick Record",
// else "file".
std::string kind(const EntryFields& fields);
// _stat: the row for one entry, or nullopt when it does not count (another computer's entry, a
// kind not counted, a re-transcribed file). Its timestamp is now (UTC) when the fields have
// none.
std::optional<StatRow> stat(const EntryFields& fields, const Clock& clock = system_clock());

// The dictation_stats table of one database. Writes are serialized (a dictation saved while
// history is being counted must not be counted twice), so make one per database.
// Affinity: thread-safe; blocking, never on the GUI thread.
class StatsRepository {
public:
    explicit StatsRepository(Database& database, const Clock& clock = system_clock());
    StatsRepository(const StatsRepository&) = delete;
    StatsRepository& operator=(const StatsRepository&) = delete;

    // Adds the row for a saved entry, once per entry id. `row` is the entry as saved (its
    // non-null columns win); `fields` what it was saved from. Listeners run on this thread
    // after a new row is stored.
    void record(const EntryFields& fields, const HistoryEntry* row);

    // _ensure_backfilled: counts the local dictations already in History, once.
    void ensure_backfilled();
    // Gives finished meetings a row, and once, the files and walkthroughs already in History.
    // Safe at any time: nothing is counted twice, nothing from before a reset again.
    void count_features();
    // Deletes every row; History is untouched and is not counted again.
    void reset();

    // Called whenever a row is counted, on the thread that counted it.
    Subscription add_listener(std::function<void()> listener);

    // Every stored row, markers included, in id order.
    std::vector<StatRow> rows();

private:
    struct Listeners;

    Database* database_;
    const Clock* clock_;
    std::mutex write_mutex_;
    std::shared_ptr<Listeners> listeners_;
};

// The module functions of dictation_stats.py, over the one StatsRepository of the database
// (Python's module-level db and listener list).
inline void record(StatsRepository& stats, const EntryFields& fields, const HistoryEntry* row) {
    stats.record(fields, row);
}
inline void count_features(StatsRepository& stats) {
    stats.count_features();
}
inline void reset(StatsRepository& stats) {
    stats.reset();
}
inline Subscription add_listener(StatsRepository& stats, std::function<void()> listener) {
    return stats.add_listener(std::move(listener));
}

} // namespace fv::dictation_stats
