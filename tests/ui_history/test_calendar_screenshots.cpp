// The History calendar against the references OpenWhisper rendered for it
// (tools/parity/builders/a09.py: history_calendar_dialog/march, calendar_month_grid/crowded-day,
// calendar_month_picker/months), with the same records, today pinned to 2026-03-15 and the
// animations at their ends.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QEvent>
#include <QList>
#include <QScrollBar>
#include <QSize>

#include "base/testing/fake_clock.h"
#include "calendar_test_support.h"
#include "history/calendar_source.h"
#include "history_ui_support.h"
#include "pyport/datetime.h"
#include "screenshot/screenshot.h"
#include "ui_history/history_calendar.h"
#include "ui_history/history_calendar_dialog.h"

using fv::HistoryEntry;
namespace hc = fv::history;

namespace {

constexpr char kModel[] = "parakeet (Parakeet TDT 0.6B v3 | cuda)";

std::chrono::system_clock::time_point pinnedNow() {
    return std::chrono::sys_days{std::chrono::year{2026} / 3 / 15} + std::chrono::hours{15};
}

HistoryEntry entry(const char* id, const char* timestamp, const char* text, const char* source,
                   const char* kind, double seconds) {
    HistoryEntry made;
    made.id = id;
    made.timestamp = timestamp;
    made.text = text;
    made.model = kModel;
    made.audio_duration = seconds;
    made.source_name = source;
    made.entry_kind = kind;
    return made;
}

// The builder's entries: the History sidebar's three on 2026-03-15 and twelve across February
// and March.
std::vector<hc::CalendarItem> calendarItems() {
    HistoryEntry dictation = entry("entry-dictation", "2026-03-15T15:00:00",
                                   "Hello from OpenWhisper_CPP.", "Quick Record", "dictation", 4.6);
    dictation.raw_text = "hello from openwhisper-cpp";
    dictation.audio_file = "recording_20260315_150000.wav";
    dictation.transcription_time = 0.42;
    dictation.file_size = 403200;
    dictation.cleanup_provider = "openai";
    dictation.cleanup_model = "gpt-4o-mini";
    dictation.app_name = "Notepad";
    dictation.cleanup_level = "medium";
    HistoryEntry failed =
        entry("entry-failed", "2026-03-15T14:30:00", "", "Quick Record", "failed_recording", 3.0);
    failed.audio_file = "recording_20260315_143000.wav";
    failed.file_size = 264600;
    HistoryEntry upload =
        entry("entry-file", "2026-03-15T14:00:00",
              "We reviewed the interview notes and agreed on the next steps for the launch.",
              "interview.mp3", "file", 754.0);
    upload.transcription_time = 12.3;

    std::vector<HistoryEntry> entries = {
        dictation,
        failed,
        upload,
        entry("cal-01", "2026-02-03T09:12:00", "Notes for the February planning meeting.",
              "Quick Record", "dictation", 6.2),
        entry("cal-02", "2026-02-03T16:40:00",
              "The budget review went through every line of the plan.", "budget-review.mp3", "file",
              312.0),
        entry("cal-03", "2026-02-17T11:05:00", "Call the printer about the order.", "Quick Record",
              "dictation", 3.4),
        entry("cal-04", "2026-02-24T08:30:00", "Draft the reply to the landlord.", "Quick Record",
              "dictation", 5.0),
        entry("cal-05", "2026-03-02T10:15:00", "Book the room for Thursday.", "Quick Record",
              "dictation", 4.1),
        entry("cal-06", "2026-03-02T13:20:00", "Everyone shared what they finished yesterday.",
              "standup.m4a", "file", 640.0),
        entry("cal-07", "2026-03-09T09:00:00", "Water the plants.", "Quick Record", "dictation",
              2.8),
        entry("cal-08", "2026-03-10T17:45:00",
              "Ideas for the spring newsletter and the photo page.", "Quick Record", "dictation",
              7.5),
        entry("cal-09", "2026-03-11T12:00:00", "Today we look at how rivers shape their valleys.",
              "lecture.wav", "file", 1800.0),
        entry("cal-10", "2026-03-13T09:30:00", "Send the invoice before Friday.", "Quick Record",
              "dictation", 3.0),
        entry("cal-11", "2026-03-14T18:10:00", "Pick up the bike from the shop.", "Quick Record",
              "dictation", 4.4),
        entry("cal-12", "2026-03-14T20:05:00", "Remember the birthday card.", "Quick Record",
              "dictation", 2.2),
    };
    std::vector<hc::CalendarItem> items;
    for (const HistoryEntry& each : entries) {
        items.push_back(hc::entry_item(each, fv::py::fromisoformat(each.timestamp)));
    }
    return items;
}

// The builder's _CalendarSource sorts a month by (when, record_id).
class BuilderReads final : public fvtest::FakeReads {
public:
    BuilderReads() : FakeReads(calendarItems()) {}
    std::vector<hc::CalendarItem> load_month(int year, int month) override {
        std::vector<hc::CalendarItem> items = FakeReads::load_month(year, month);
        std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            return std::tie(a.when.year, a.when.month, a.when.day, a.when.hour, a.when.minute,
                            a.when.second, a.when.microsecond, a.record_id) <
                   std::tie(b.when.year, b.when.month, b.when.day, b.when.hour, b.when.minute,
                            b.when.second, b.when.microsecond, b.record_id);
        });
        return items;
    }
};

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start; Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes, as tests/ui_history/test_reference_screenshots.cpp does (amendment 17).
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    QList<QAbstractScrollArea*> areas = root.findChildren<QAbstractScrollArea*>();
    for (QAbstractScrollArea* area : areas) {
        for (QScrollBar* bar : {area->horizontalScrollBar(), area->verticalScrollBar()}) {
            QWidget* container = bar->parentWidget();
            if (container == nullptr || container->size() != kUnsized || bar->size() != kUnsized) {
                continue;
            }
            const QSize hint = bar->sizeHint();
            const QSize sized = bar->orientation() == Qt::Horizontal
                                    ? QSize(kUnsized.width(), hint.height())
                                    : QSize(hint.width(), kUnsized.height());
            container->resize(sized);
            bar->resize(sized);
        }
    }
}

std::string variantOf(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

void calendarDialog(const std::string& theme, int scale) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fv::FakeClock clock(pinnedNow());
    fvtest::InlineExecutor io;
    fv::HistoryCalendarDialog dialog(std::make_shared<BuilderReads>(), io, clock, hc::SUNDAY);
    dialog.refresh();
    dialog.finishAnimations();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    sizeHiddenScrollBarsAsQt611(dialog);
    fv::screenshot::expect_matches(dialog, "history_calendar_dialog/march",
                                   variantOf(theme, scale));
}

} // namespace

TEST_CASE("reference screenshot history_calendar_dialog march dark", "[screenshot]") {
    calendarDialog("dark", 100);
}

TEST_CASE("reference screenshot history_calendar_dialog march light", "[screenshot]") {
    calendarDialog("light", 100);
}

TEST_CASE("reference screenshot history_calendar_dialog march dark 130", "[screenshot]") {
    calendarDialog("dark", 130);
}

TEST_CASE("reference screenshot calendar_month_grid crowded-day dark", "[screenshot]") {
    const fvtest::AppTheme appTheme(QStringLiteral("dark"), 100);
    fv::FakeClock clock(pinnedNow());
    // The 12th holds every kind and more lines than its cell has room for.
    struct Record {
        std::string id;
        fv::py::DateTime when;
        std::string kind;
        std::string title;
    };
    std::vector<Record> records;
    for (int n = 0; n < 7; ++n) {
        records.push_back({"g-d" + std::to_string(n), fvtest::at(2026, 3, 12, 8, 5 * n),
                           std::string(hc::DICTATION_KIND), "Quick note " + std::to_string(n + 1)});
    }
    records.push_back(
        {"g-m1", fvtest::at(2026, 3, 12, 15, 0), std::string(hc::MEETING_KIND), "Design review"});
    records.push_back(
        {"g-f1", fvtest::at(2026, 3, 12, 17, 30), std::string(hc::FILE_KIND), "interview.wav"});
    records.push_back({"g-d10", fvtest::at(2026, 3, 3, 9, 0), std::string(hc::DICTATION_KIND),
                       "Groceries for the week"});
    records.push_back(
        {"g-f2", fvtest::at(2026, 3, 5, 14, 0), std::string(hc::FILE_KIND), "lecture-notes.mp3"});
    records.push_back(
        {"g-m2", fvtest::at(2026, 3, 9, 10, 0), std::string(hc::MEETING_KIND), "Weekly sync"});
    records.push_back({"g-d11", fvtest::at(2026, 3, 15, 9, 30), std::string(hc::DICTATION_KIND),
                       "Plan the trip"});
    std::vector<hc::CalendarItem> items;
    std::vector<hc::Mark> marks;
    for (const Record& record : records) {
        hc::CalendarItem made = fvtest::item(
            record.id, record.when,
            {.kind = record.kind, .title = record.title, .preview = "", .seconds = 60.0});
        marks.push_back(hc::Mark{made.when, made.kind, made.place, made.record_id, made.seconds});
        items.push_back(std::move(made));
    }
    const hc::CalendarIndex index(marks);
    fv::MonthDetails details;
    details.days = index.days(2026, 3);
    details.milestones = index.milestone_days(2026, 3);
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return std::tie(a.when.day, a.when.hour, a.when.minute, a.record_id) <
               std::tie(b.when.day, b.when.hour, b.when.minute, b.record_id);
    });
    for (const hc::CalendarItem& each : items) {
        details.items[each.day()].push_back(each);
    }
    fv::MonthGrid grid(clock);
    grid.setMonth(2026, 3, std::move(details), fvtest::dayOf(2026, 3, 15), hc::SUNDAY);
    grid.setSelected(fvtest::dayOf(2026, 3, 12), false);
    fv::screenshot::expect_matches(grid, "calendar_month_grid/crowded-day", variantOf("dark", 100));
}

TEST_CASE("reference screenshot calendar_month_picker months dark", "[screenshot]") {
    const fvtest::AppTheme appTheme(QStringLiteral("dark"), 100);
    fv::FakeClock clock(pinnedNow());
    fv::MonthPicker picker(clock, nullptr);
    picker.setData(
        {{{2025, 11}, 4}, {{2025, 12}, 9}, {{2026, 1}, 6}, {{2026, 2}, 4}, {{2026, 3}, 11}},
        {2026, 3}, {2026, 3});
    // As popup() leaves it, without showing it under an anchor: this year, this month in focus.
    picker.setFocusIndex(2);
    fv::screenshot::expect_matches(picker, "calendar_month_picker/months", variantOf("dark", 100));
}
