#pragma once

// What the Stats window tests share (OpenWhisper's tests/test_personalize_s9_stats_dialog.py
// fixtures): a dictation_stats table on a fresh database, today pinned to Tuesday 2026-10-06 at
// noon in UTC (Python's tests read date.today() in the local zone), and an io executor that runs a
// count at once and delivers it before returning (the `synchronous` fixture).

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "history/dictation_stats.h"
#include "history/stats_summary.h"
#include "testkit/testkit.h"
#include "ui_history/stats_dialog.h"

namespace fvtest {

// Runs each task on a thread of its own, waits for it, then delivers what it posted to this
// thread: the count's database work stays off the GUI thread, and its result lands before post
// returns, as Python's synchronous fixture emits it.
class SyncIo final : public fv::Executor {
public:
    void post(fv::Task task) override;
    fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task task) override;
    bool is_current() const override { return false; }
};

// A dictation_stats table on a fresh database.
class LocalStats {
public:
    // TODAY: Tuesday 2026-10-06.
    static constexpr fv::dictation_stats::Day kToday{std::chrono::year{2026}, std::chrono::October,
                                                     std::chrono::day{6}};

    // The clock at noon UTC of today.
    explicit LocalStats(fv::dictation_stats::Day today = kToday);
    ~LocalStats();
    LocalStats(const LocalStats&) = delete;
    LocalStats& operator=(const LocalStats&) = delete;

    fv::dictation_stats::StatsRepository& repository() { return *repository_; }
    std::shared_ptr<fv::dictation_stats::StatsRepository> shared() { return repository_; }
    fv::FakeClock& clock() { return clock_; }
    fv::StatsReads reads() { return fv::statsReadsOf(*repository_, clock_); }

    // dictation_stats.record(fields, None), off the GUI thread.
    void record(const fv::dictation_stats::EntryFields& fields);
    // load_summary(), off the GUI thread.
    fv::dictation_stats::StatsSummary load();

    // Noon of the day daysBack before today, as a stored UTC timestamp (_now).
    static std::string noon(int daysBack = 0);

private:
    fv::testkit::TempDataRoot root_;
    std::unique_ptr<fv::Database> database_;
    fv::FakeClock clock_;
    std::shared_ptr<fv::dictation_stats::StatsRepository> repository_;
};

// _seed of test_personalize_s9_stats_dialog.py: 6, 4 and 3 words (each said with an extra "um")
// into Slack, Outlook and Slack, today and the two days before, 3 s each.
void seedThreeDays(LocalStats& stats);
// _seed of test_personalize_fix2_g3_stats_chart.py: 7, 4, none and 3 words into Slack, today and
// the three days before.
void seedChartDays(LocalStats& stats);

} // namespace fvtest
