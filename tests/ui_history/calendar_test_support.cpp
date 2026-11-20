#include "calendar_test_support.h"

#include <algorithm>
#include <tuple>

namespace fvtest {

namespace hc = fv::history;

hc::Mark FakeReads::markOf(const hc::CalendarItem& item) {
    return hc::Mark{item.when, item.kind, item.place, item.record_id, item.seconds};
}

std::vector<hc::Mark> FakeReads::load_index() {
    std::vector<hc::Mark> marks;
    for (const hc::CalendarItem& each : items_) {
        marks.push_back(markOf(each));
    }
    return marks;
}

std::vector<hc::Mark> FakeReads::load_remote(const std::vector<std::string>&) {
    std::vector<hc::Mark> marks;
    for (const hc::CalendarItem& each : remote_) {
        marks.push_back(markOf(each));
    }
    return marks;
}

std::vector<hc::CalendarItem> FakeReads::load_month(int year, int month) {
    {
        const std::lock_guard lock(mutex_);
        monthReads_.push_back(hc::MonthKey{year, month});
    }
    std::vector<hc::CalendarItem> found;
    for (const auto* list : {&items_, &remote_}) {
        for (const hc::CalendarItem& each : *list) {
            if (each.when.year == year && each.when.month == month) {
                found.push_back(each);
            }
        }
    }
    // sorted(key=lambda it: it.when): stable, by wall time.
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return std::tie(a.when.year, a.when.month, a.when.day, a.when.hour, a.when.minute,
                        a.when.second, a.when.microsecond) <
               std::tie(b.when.year, b.when.month, b.when.day, b.when.hour, b.when.minute,
                        b.when.second, b.when.microsecond);
    });
    return found;
}

std::vector<hc::MonthKey> FakeReads::monthReads() const {
    const std::lock_guard lock(mutex_);
    return monthReads_;
}

std::vector<hc::CalendarItem> september() {
    return {
        item("d1", at(2026, 9, 27, 9, 5), {.preview = "Morning notes"}),
        item("d2", at(2026, 9, 27, 10, 40), {.preview = "Second thought"}),
        item("m1", at(2026, 9, 27, 14, 0),
             {.kind = std::string(hc::MEETING_KIND), .title = "Standup", .seconds = 900}),
        item("f1", at(2026, 9, 26, 16, 0),
             {.kind = std::string(hc::FILE_KIND), .title = "interview.wav"}),
        item("d3", at(2026, 9, 12, 8, 0)),
        item("d4", at(2026, 5, 4, 8, 0)),
    };
}

std::vector<hc::CalendarItem> yearsApart() {
    std::vector<hc::CalendarItem> items = september();
    items.push_back(item("y25", at(2025, 3, 14, 9, 0)));
    items.push_back(item("y24a", at(2024, 11, 2, 9, 0)));
    items.push_back(item("y24b", at(2024, 11, 3, 9, 0)));
    return items;
}

} // namespace fvtest
