#include "stats_test_support.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>

#include "history_ui_support.h"

namespace fvtest {

namespace ds = fv::dictation_stats;

void SyncIo::post(fv::Task task) {
    offGui([&task] { task(); });
    QCoreApplication::sendPostedEvents();
}

fv::TimerHandle SyncIo::post_after(std::chrono::milliseconds, fv::Task task) {
    post(std::move(task));
    return {};
}

LocalStats::LocalStats(ds::Day today)
    : database_(std::make_unique<fv::Database>(
          offGui([this] { return fv::Database::open_or_create(root_.path() / "stats.db"); }))),
      clock_(std::chrono::sys_days{today} + std::chrono::hours{12}, "UTC"),
      repository_(std::make_shared<ds::StatsRepository>(*database_, clock_)) {}

LocalStats::~LocalStats() {
    offGui([this] { database_->close(); });
}

void LocalStats::record(const ds::EntryFields& fields) {
    offGui([&] { ds::record(*repository_, fields, nullptr); });
}

ds::StatsSummary LocalStats::load() {
    return offGui([this] { return ds::load_summary(*repository_, clock_); });
}

std::string LocalStats::noon(int daysBack) {
    const std::chrono::sys_seconds instant =
        std::chrono::sys_days{kToday} - std::chrono::days{daysBack} + std::chrono::hours{12};
    return std::format("{:%Y-%m-%dT%H:%M:%S}+00:00", instant);
}

namespace {

std::string words(int count) {
    std::string out;
    for (int index = 0; index < count; ++index) {
        out += index == 0 ? "word" : " word";
    }
    return out;
}

} // namespace

void seedThreeDays(LocalStats& stats) {
    const std::vector<std::pair<int, std::string>> days = {
        {6, "Slack"}, {4, "Outlook"}, {3, "Slack"}};
    for (std::size_t offset = 0; offset < days.size(); ++offset) {
        ds::EntryFields fields;
        fields.text = words(days[offset].first);
        fields.raw_text = words(days[offset].first) + " um";
        fields.entry_kind = "dictation";
        fields.audio_duration = 3.0;
        fields.app_name = days[offset].second;
        fields.timestamp = LocalStats::noon(static_cast<int>(offset));
        stats.record(fields);
    }
}

void seedChartDays(LocalStats& stats) {
    const std::vector<int> counts = {7, 4, 0, 3};
    for (std::size_t offset = 0; offset < counts.size(); ++offset) {
        if (counts[offset] == 0) {
            continue;
        }
        ds::EntryFields fields;
        fields.text = words(counts[offset]);
        fields.raw_text = words(counts[offset]);
        fields.entry_kind = "dictation";
        fields.audio_duration = 3.0;
        fields.app_name = "Slack";
        fields.timestamp = LocalStats::noon(static_cast<int>(offset));
        stats.record(fields);
    }
}

} // namespace fvtest
