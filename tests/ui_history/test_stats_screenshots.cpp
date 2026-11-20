// The Stats window and the History export dialog against the references OpenWhisper rendered for
// them (tools/parity/builders/a09.py: stats_dialog/week, stats_dialog/empty,
// history_export_dialog/all, history_export_dialog/selected), with the same data: the stats rows
// seeded 40 days back from 2026-03-15 in UTC, weeks starting on the en_US Sunday, and five export
// entries with the output in a fixed folder.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDate>
#include <QListWidget>
#include <QLocale>
#include <QRadioButton>
#include <QScrollBar>
#include <QTimer>

#include "base/testing/manual_executor.h"
#include "history_ui_support.h"
#include "screenshot/screenshot.h"
#include "stats_test_support.h"
#include "ui_history/history_export_dialog.h"
#include "ui_history/stats_dialog.h"
#include "ui_widgets/no_wheel.h"

namespace ds = fv::dictation_stats;
using fv::Json;

namespace {

constexpr ds::Day kToday{std::chrono::year{2026}, std::chrono::March, std::chrono::day{15}};

std::string variantOf(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

// The en_US locale the builder pins, so the charts' weeks start on Sunday.
class ScopedUsLocale {
public:
    ScopedUsLocale() : previous_(QLocale()) {
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    }
    ~ScopedUsLocale() { QLocale::setDefault(previous_); }
    ScopedUsLocale(const ScopedUsLocale&) = delete;
    ScopedUsLocale& operator=(const ScopedUsLocale&) = delete;

private:
    QLocale previous_;
};

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start; Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes, as the calendar's references are (amendment 17).
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    for (QAbstractScrollArea* area : root.findChildren<QAbstractScrollArea*>()) {
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

// The tree keys check() compares itself.
const std::vector<std::string> kTreeKeys{
    "class", "qtClass",   "objectName", "geometry",  "visible",  "hasFocus",   "properties",
    "text",  "isChecked", "isDefault",  "alignment", "wordWrap", "styleSheet", "font"};

// A widget that is not shown keeps the geometry of whichever layout pass last reached it (a fresh
// widget's 640 x 480 until one does), which no one sees and Qt makes no promise about; a hidden
// segmented bar's wrapping is decided by the same passes. Both are taken from the reference for
// widgets it has hidden.
void normalizeHidden(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
        if (expected.contains("wordWrap")) {
            node["wordWrap"] = expected["wordWrap"];
        }
    }
    const auto& want = expected["children"];
    auto& have = node["children"];
    for (std::size_t i = 0; i < std::min(want.size(), have.size()); ++i) {
        normalizeHidden(have[i], want[i]);
    }
}

// Swallows the timer events of every QTimer below a widget while it lives, as expect_matches
// holds them between show() and the grab.
class HeldTimers : public QObject {
public:
    explicit HeldTimers(QWidget& root) {
        for (QTimer* timer : root.findChildren<QTimer*>()) {
            timer->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override { return event->type() == QEvent::Timer; }
};

void compareTree(QWidget& widget, const Json& pins, const std::string& name,
                 const std::string& variant) {
    Json actual = fv::screenshot::dump_tree(widget);
    normalizeHidden(actual, pins["tree"]);
    for (const auto& difference : fv::screenshot::compare_trees(pins["tree"], actual)) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
}

// The pixels through expect_matches, and the tree with hidden widgets normalized, both of the
// widget as one show() and one turn of the event loop leave it, as the reference was grabbed (a
// second turn lets queued layout requests move a few pixels).
void check(QWidget& widget, const std::string& name, const std::string& variant) {
    const Json pins = fv::screenshot::load_reference(name, variant);
    sizeHiddenScrollBarsAsQt611(widget);
    if (!fv::screenshot::pixel_comparison_enabled() ||
        !fv::screenshot::qt_kit_matches_reference()) {
        widget.setFixedSize(pins["logical_size"][0].get<int>(), pins["logical_size"][1].get<int>());
        const HeldTimers held(widget);
        widget.show();
        QApplication::processEvents();
        compareTree(widget, pins, name, variant);
    }
    fv::screenshot::Options options;
    options.ignore_keys = kTreeKeys;
    fv::screenshot::expect_matches(widget, name, variant, options);
    compareTree(widget, pins, name, variant);
}

// _stats_rows of the builder.
std::vector<ds::EntryFields> statsRows() {
    static const std::vector<const char*> kApps = {"Slack", "Outlook", "Notes", "Word", "Chrome"};
    static const std::vector<const char*> kCategories = {"work", "email", "personal", ""};
    static const std::vector<const char*> kCleanup = {"light", "medium", ""};
    std::vector<ds::EntryFields> rows;
    for (int back = 0; back < 40; ++back) {
        if (back % 9 == 4) {
            continue;
        }
        const ds::Day day{std::chrono::sys_days{kToday} - std::chrono::days{back}};
        const std::string iso = std::format("{:%Y-%m-%d}", std::chrono::sys_days{day});
        for (int index = 0; index < 1 + back % 3; ++index) {
            const int count = 20 + (back * 7 + index * 13) % 60;
            std::string text;
            for (int word = 0; word < count; ++word) {
                text += word == 0 ? "word" : " word";
            }
            ds::EntryFields fields;
            fields.text = text;
            fields.raw_text = index % 2 != 0 ? text : text + " um uh";
            fields.entry_kind = "dictation";
            fields.audio_duration = count / 2.0;
            fields.app_name = kApps[static_cast<std::size_t>((back + index) % 5)];
            fields.app_category = kCategories[static_cast<std::size_t>((back + index) % 4)];
            fields.cleanup_level = kCleanup[static_cast<std::size_t>((back * 2 + index) % 3)];
            fields.language = (back + index) % 6 == 0 ? "it" : "en";
            fields.timestamp = std::format("{}T{:02d}:{:02d}:00+00:00", iso,
                                           8 + (back * 3 + index * 5) % 11, (index * 17) % 60);
            if (back == 0 && index == 0) {
                fields.stats_counts =
                    Json{{"snippets", 1}, {"snippet_words", 8}, {"corrections", 2}};
            }
            rows.push_back(fields);
        }
        const std::string stamp = iso + "T13:00:00+00:00";
        if (back == 2 || back == 9 || back == 25) {
            ds::EntryFields file;
            file.text = "a long talk about the river and its valley";
            file.entry_kind = "file";
            file.audio_duration = 600.0 * (1 + back % 3);
            file.timestamp = stamp;
            rows.push_back(file);
        }
        if (back == 5) {
            ds::EntryFields walkthrough;
            walkthrough.text = "steps";
            walkthrough.entry_kind = "walkthrough";
            walkthrough.audio_duration = 120.0;
            walkthrough.stats_counts = Json{{"shots", 4}};
            walkthrough.timestamp = stamp;
            rows.push_back(walkthrough);
        }
        if (back == 1 || back == 3 || back == 12) {
            ds::EntryFields action;
            action.text = "send it";
            action.entry_kind = "action";
            action.stats_counts = Json{{"target", back != 12 ? "Slack" : "Notes"}};
            action.timestamp = stamp;
            rows.push_back(action);
        }
        if (back == 3) {
            ds::EntryFields command;
            command.text = "Formal now.";
            command.raw_text = "formal nw";
            command.entry_kind = "command";
            command.timestamp = stamp;
            rows.push_back(command);
        }
    }
    return rows;
}

void statsDialog(const std::string& variant, const std::string& theme, int scale) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    const ScopedUsLocale locale;
    fvtest::LocalStats stats(kToday);
    ds::StatsSummary summary;
    if (variant == "week") {
        fvtest::offGui([&stats] { ds::reset(stats.repository()); });
        for (const ds::EntryFields& fields : statsRows()) {
            stats.record(fields);
        }
        summary = stats.load();
    }
    // The count the window starts as it shows is never run, so the seeded summary stays.
    fv::ManualExecutor io;
    fv::StatsDialog dialog(stats.reads(), io);
    dialog.apply(dialog.token(), summary, QString());
    check(dialog, "stats_dialog/" + variant, variantOf(theme, scale));
}

// _export_entries of the builder.
std::vector<Json> exportEntries() {
    const std::vector<std::array<const char*, 3>> entries = {
        {"export-1", "Mar 15, 2026 03:00 PM", "Hello from OpenWhisper_CPP."},
        {"export-2", "Mar 14, 2026 08:05 PM", "Remember the birthday card."},
        {"export-3", "Mar 14, 2026 06:10 PM", "Pick up the bike from the shop."},
        {"export-4", "Mar 13, 2026 09:30 AM", "Send the invoice before Friday."},
        {"export-5", "Mar 11, 2026 12:00 PM", "Today we look at how rivers shape their valleys."},
    };
    std::vector<Json> made;
    for (const auto& [id, stamp, preview] : entries) {
        made.push_back(Json{{"id", id},
                            {"formatted_timestamp", stamp},
                            {"preview_text", preview},
                            {"text", preview},
                            {"timestamp", "2026-03-15T15:00:00+00:00"},
                            {"has_audio", false}});
    }
    return made;
}

void exportDialog(const std::string& variant, const std::string& theme, int scale) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fv::ManualExecutor io;
    fv::HistoryExportDialog dialog([] { return exportEntries(); },
                                   R"(C:\Users\Dana\AppData\Local\OpenWhisper_CPP\recordings)", io);
    dialog.fromDate()->setDate(QDate(2026, 2, 15));
    dialog.toDate()->setDate(QDate(2026, 3, 15));
    if (variant == "selected") {
        dialog.selectedRadio()->setChecked(true);
        for (const int row : {0, 2, 4}) {
            dialog.selectedList()->item(row)->setCheckState(Qt::Checked);
        }
    }
    check(dialog, "history_export_dialog/" + variant, variantOf(theme, scale));
}

} // namespace

TEST_CASE("reference screenshot stats_dialog week dark", "[screenshot]") {
    statsDialog("week", "dark", 100);
}

TEST_CASE("reference screenshot stats_dialog week light", "[screenshot]") {
    statsDialog("week", "light", 100);
}

TEST_CASE("reference screenshot stats_dialog week dark 130", "[screenshot]") {
    statsDialog("week", "dark", 130);
}

TEST_CASE("reference screenshot stats_dialog empty dark", "[screenshot]") {
    statsDialog("empty", "dark", 100);
}

TEST_CASE("reference screenshot history_export_dialog all dark", "[screenshot]") {
    exportDialog("all", "dark", 100);
}

TEST_CASE("reference screenshot history_export_dialog all light", "[screenshot]") {
    exportDialog("all", "light", 100);
}

TEST_CASE("reference screenshot history_export_dialog all dark 130", "[screenshot]") {
    exportDialog("all", "dark", 130);
}

TEST_CASE("reference screenshot history_export_dialog selected dark", "[screenshot]") {
    exportDialog("selected", "dark", 100);
}
