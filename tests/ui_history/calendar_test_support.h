#pragma once

// What the History calendar tests share (OpenWhisper's tests/test_history_calendar_dialog.py
// fixtures): calendar items made like its item(), a source answering from a fixed list
// (FakeSource), the September of its tests, and an io executor that runs a read at once on the
// calling thread (Python's threaded=False).

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "history/calendar.h"
#include "history/entry.h"
#include "pyport/datetime.h"
#include "ui_history/history_calendar_dialog.h"

namespace fvtest {

// Runs each task at once, on the thread that posts it.
class InlineExecutor final : public fv::Executor {
public:
    void post(fv::Task task) override {
        running_ = true;
        task();
        running_ = false;
    }
    fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task task) override {
        post(std::move(task));
        return {};
    }
    bool is_current() const override { return running_; }

private:
    bool running_ = false;
};

inline fv::py::DateTime at(int year, int month, int day, int hour = 0, int minute = 0) {
    fv::py::DateTime when;
    when.year = year;
    when.month = month;
    when.day = day;
    when.hour = hour;
    when.minute = minute;
    return when;
}

inline fv::history::Day dayOf(int year, int month, int day) {
    return fv::history::Day{std::chrono::year{year},
                            std::chrono::month{static_cast<unsigned>(month)},
                            std::chrono::day{static_cast<unsigned>(day)}};
}

// item(record_id, when, kind=DICTATION, place=HERE, title="", preview="said something",
// seconds=4.0): its source is an entry with that id and the preview as its text.
struct ItemSpec {
    std::string kind{fv::history::DICTATION_KIND};
    fv::history::Place place{};
    std::string title;
    std::string preview = "said something";
    double seconds = 4.0;
};

inline fv::history::CalendarItem item(const std::string& recordId, const fv::py::DateTime& when,
                                      const ItemSpec& spec = {}) {
    fv::HistoryEntry entry;
    entry.id = recordId;
    entry.text = spec.preview;
    fv::history::CalendarItem made;
    made.kind = spec.kind;
    made.record_id = recordId;
    made.when = when;
    made.title = spec.title;
    made.preview = spec.preview;
    made.seconds = spec.seconds;
    made.place = spec.place;
    if (spec.kind == fv::history::MEETING_KIND) {
        made.source = fv::Json{{"id", recordId}};
    } else {
        made.source = entry;
    }
    return made;
}

// FakeSource: the calendar's reads from a fixed list, recording each month read.
// Affinity: thread-safe.
class FakeReads : public fv::CalendarReads {
public:
    explicit FakeReads(std::vector<fv::history::CalendarItem> items,
                       std::vector<fv::history::CalendarItem> remote = {})
        : items_(std::move(items)), remote_(std::move(remote)) {}

    std::vector<fv::history::Mark> load_index() override;
    bool remote_wanted() override { return !remote_.empty(); }
    std::vector<fv::history::Mark> load_remote(const std::vector<std::string>&) override;
    std::vector<fv::history::CalendarItem> load_month(int year, int month) override;
    std::string notice() override { return ""; }

    std::vector<fv::history::MonthKey> monthReads() const;

private:
    static fv::history::Mark markOf(const fv::history::CalendarItem& item);

    std::vector<fv::history::CalendarItem> items_;
    std::vector<fv::history::CalendarItem> remote_;
    mutable std::mutex mutex_;
    std::vector<fv::history::MonthKey> monthReads_;
};

// september(): three records today (2026-09-27), a file the day before, one on the 12th and
// one in May.
std::vector<fv::history::CalendarItem> september();
// years_apart(): september() and three records in 2025 and 2024.
std::vector<fv::history::CalendarItem> yearsApart();

// TODAY, 2026-09-27, at noon UTC.
inline std::chrono::system_clock::time_point todayNoon() {
    return std::chrono::sys_days{std::chrono::year{2026} / 9 / 27} + std::chrono::hours{12};
}

} // namespace fvtest
