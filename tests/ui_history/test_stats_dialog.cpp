// The Stats window: loading off the UI thread, empty and full states, reset, the feature rows and
// what they open (OpenWhisper's tests/test_personalize_s9_stats_dialog.py,
// test_personalize_fix2_g3_stats_dialog.py and the window half of test_stats_features.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTest>

#include "appkit/app_context.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/manual_executor.h"
#include "history_ui_support.h"
#include "settings/testing/memory_store.h"
#include "stats_test_support.h"
#include "ui_history/history_feature.h"
#include "ui_history/stats_dialog.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/wrapped_label.h"

namespace ds = fv::dictation_stats;
using fv::StatsDialog;
using fvtest::LocalStats;

namespace {

QString q(const char* text) {
    return QString::fromUtf8(text);
}

// The window over a table, counting through SyncIo.
struct Window {
    LocalStats stats;
    fvtest::SyncIo io;
    std::unique_ptr<StatsDialog> dialog;

    StatsDialog& make() {
        dialog = std::make_unique<StatsDialog>(stats.reads(), io);
        return *dialog;
    }
};

// _seed_window of test_stats_features.py: two dictations, a file, an action today, and a
// walkthrough 20 days ago.
void seedWindow(LocalStats& stats) {
    ds::EntryFields said;
    said.text = "four words said here";
    said.entry_kind = "dictation";
    said.timestamp = LocalStats::noon();
    said.app_category = "work";
    said.cleanup_level = "medium";
    said.language = "en";
    said.stats_counts = fv::Json{{"snippets", 1}, {"snippet_words", 12}};
    stats.record(said);
    ds::EntryFields italian;
    italian.text = "due parole";
    italian.entry_kind = "dictation";
    italian.timestamp = LocalStats::noon();
    italian.language = "it";
    stats.record(italian);
    ds::EntryFields talk;
    talk.text = "a talk";
    talk.entry_kind = "file";
    talk.audio_duration = 600.0;
    talk.timestamp = LocalStats::noon();
    stats.record(talk);
    ds::EntryFields action;
    action.text = "ask it";
    action.entry_kind = "action";
    action.timestamp = LocalStats::noon();
    action.stats_counts = fv::Json{{"target", "Claude"}};
    stats.record(action);
    ds::EntryFields steps;
    steps.text = "steps";
    steps.entry_kind = "walkthrough";
    steps.timestamp = LocalStats::noon(20);
    steps.stats_counts = fv::Json{{"shots", 3}};
    stats.record(steps);
}

std::vector<std::pair<QString, std::int64_t>>
rows(std::initializer_list<std::pair<const char*, std::int64_t>> made) {
    std::vector<std::pair<QString, std::int64_t>> out;
    for (const auto& [name, words] : made) {
        out.emplace_back(q(name), words);
    }
    return out;
}

// The shell, History and the Stats window as Bootstrap builds them, over a stats table.
class StatsWiring {
public:
    StatsWiring()
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          context_(services()) {
        shell_.provide(context_);
        history_.provide(context_);
        context_.provide<fv::HistoryStore>(store_);
        context_.provide<ds::StatsRepository>(stats_.shared());
        shell_.install(context_);
        history_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
    }

    ~StatsWiring() {
        if (StatsDialog* dialog = presenter()->dialog()) {
            dialog->hide();
        }
        window().hide();
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::StatsPresenter* presenter() { return window().findChild<fv::StatsPresenter*>(); }
    fv::MenuRegistry& menus() { return context_.menus(); }
    LocalStats& stats() { return stats_; }
    fv::MemorySettingsStore& settings() { return settings_; }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.clock = &stats_.clock();
        made.ui = &ui_;
        made.io = &io_;
        return made;
    }

    LocalStats stats_;
    std::shared_ptr<fvtest::FakeStore> store_ = std::make_shared<fvtest::FakeStore>();
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fvtest::SyncIo io_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::HistoryFeature history_;
};

} // namespace

TEST_CASE("with nothing counted it says so and offers no reset",
          "[test_personalize_s9_stats_dialog][widget]") {
    // from
    // tests/test_personalize_s9_stats_dialog.py::test_with_nothing_counted_it_says_so_and_offers_no_reset
    Window window;
    StatsDialog& dialog = window.make();
    dialog.show();
    CHECK(dialog.pages()->currentIndex() == 1);
    CHECK(dialog.emptyText()->text().startsWith(q("Dictate something")));
    CHECK_FALSE(dialog.resetButton()->isEnabled());
    dialog.close();
}

TEST_CASE("it shows the counts days and apps", "[test_personalize_s9_stats_dialog][widget]") {
    // from tests/test_personalize_s9_stats_dialog.py::test_it_shows_the_counts_days_and_apps
    Window window;
    fvtest::seedThreeDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();

    CHECK(dialog.pages()->currentIndex() == 2);
    CHECK(dialog.heroCaption()->text() == q("Dictated · last 7 days"));
    CHECK(dialog.heroValue()->text() == q("16 words"));
    CHECK(dialog.trendChip()->isHidden());
    CHECK(dialog.savedCard()->valueLabel()->text() == q("Under a minute"));
    CHECK(dialog.milestoneLabel()->text() == q("Next milestone: 1,000 words"));
    CHECK(dialog.milestoneLeft()->text() == q("984 to go"));
    CHECK(dialog.paceCard()->valueLabel()->text() == q("107 wpm"));
    CHECK(dialog.paceCard()->detailLabel()->text() == q("2.7× faster than typing"));
    CHECK(dialog.dictationsCard()->valueLabel()->text() == q("3"));
    CHECK(dialog.dictationsCard()->detailLabel()->text() == q("5 words each"));
    CHECK(dialog.cleanupLabel()->text() == q("AI cleanup changed 3 words"));
    CHECK(dialog.streakCard()->valueLabel()->text() == q("3 days"));
    CHECK(dialog.streakCard()->detailLabel()->text() == q("Keep it going"));
    REQUIRE(dialog.bars()->bars().size() == 7);
    CHECK(dialog.bars()->bars().back().words == 7);
    CHECK(dialog.appRows() == rows({{"Slack", 11}, {"Outlook", 5}}));
    CHECK(dialog.bestLabel()->text().endsWith(q("7 words")));
    // TODAY is a Tuesday.
    CHECK(dialog.hourLabel()->text() == q("Most on Tuesdays around 12 PM"));
    REQUIRE(dialog.rhythm()->grid());
    CHECK((*dialog.rhythm()->grid())[1][12] == 7);
    CHECK(dialog.featureRows().empty());
    CHECK(dialog.featuresSection()->isHidden());
    CHECK(dialog.noAppsLabel()->isHidden());
    CHECK(dialog.activity()->days().size() == static_cast<std::size_t>(ds::ACTIVITY_DAYS));
    CHECK(dialog.activeDaysLabel()->text() == q("3 active days this past year"));
    CHECK(dialog.resetButton()->isEnabled());
    dialog.close();
}

TEST_CASE("the period switch recounts every section",
          "[test_personalize_s9_stats_dialog][widget]") {
    // from tests/test_personalize_s9_stats_dialog.py::test_the_period_switch_recounts_every_section
    Window window;
    fvtest::seedThreeDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();

    dialog.periodBar()->buttons()[1]->click();
    CHECK(dialog.periodKey() == q("month"));
    CHECK(dialog.heroCaption()->text() == q("Dictated · last 30 days"));
    CHECK(dialog.bars()->bars().size() == 30);

    dialog.periodBar()->buttons()[2]->click();
    CHECK(dialog.heroCaption()->text() == q("Dictated · all time"));
    CHECK(dialog.trendChip()->text().startsWith(q("Since ")));
    CHECK(dialog.chartTitle()->text() == q("Words per month"));
    CHECK(dialog.bars()->bars().back().axis == q("Oct"));
    CHECK(dialog.appRows() == rows({{"Slack", 11}, {"Outlook", 5}}));
    dialog.close();
}

TEST_CASE("a new dictation refreshes the open window",
          "[test_personalize_s9_stats_dialog][widget]") {
    // from
    // tests/test_personalize_s9_stats_dialog.py::test_a_new_dictation_refreshes_the_open_window
    Window window;
    fvtest::seedThreeDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();
    ds::EntryFields more;
    more.text = "one more for the road";
    more.entry_kind = "dictation";
    more.timestamp = LocalStats::noon();
    // Counted on another thread, as the history-save worker counts.
    window.stats.record(more);
    CHECK(fvtest::waitFor([&] { return dialog.heroValue()->text() == q("21 words"); }));
    dialog.close();
}

TEST_CASE("a slower older count never replaces a newer one",
          "[test_personalize_s9_stats_dialog][widget]") {
    // from
    // tests/test_personalize_s9_stats_dialog.py::test_a_slower_older_count_never_replaces_a_newer_one
    LocalStats stats;
    fv::ManualExecutor io;
    StatsDialog dialog(stats.reads(), io);
    dialog.reload();
    const std::uint64_t old = dialog.token();
    dialog.reload();
    const std::uint64_t latest = dialog.token();
    CHECK(io.queued() == 2);
    ds::StatsSummary full;
    full.words_all_time = 5;
    full.entries = 1;

    dialog.apply(latest, full, QString());
    dialog.apply(old, ds::StatsSummary{}, QString());

    REQUIRE(dialog.summary());
    CHECK(*dialog.summary() == full);
    CHECK(dialog.pages()->currentIndex() == 2);
}

TEST_CASE("counting runs off the ui thread", "[test_personalize_s9_stats_dialog][widget]") {
    // from tests/test_personalize_s9_stats_dialog.py::test_counting_runs_off_the_ui_thread
    LocalStats stats;
    fv::StatsReads reads = stats.reads();
    auto threads = std::make_shared<std::vector<std::thread::id>>();
    auto mutex = std::make_shared<std::mutex>();
    reads.load = [load = reads.load, threads, mutex] {
        {
            const std::lock_guard lock(*mutex);
            threads->push_back(std::this_thread::get_id());
        }
        return load();
    };
    fv::ThreadPool io("stats-test", 1);
    {
        StatsDialog dialog(reads, io);
        dialog.show();
        CHECK(fvtest::waitFor([&] { return dialog.pages()->currentIndex() == 1; }));
        dialog.close();
    }
    const std::lock_guard lock(*mutex);
    REQUIRE_FALSE(threads->empty());
    CHECK(threads->front() != std::this_thread::get_id());
}

TEST_CASE("a failed count says so", "[test_personalize_s9_stats_dialog][widget]") {
    // from tests/test_personalize_s9_stats_dialog.py::test_a_failed_count_says_so
    LocalStats stats;
    fv::StatsReads reads = stats.reads();
    reads.load = []() -> ds::StatsSummary {
        throw std::runtime_error("locked");
    };
    fvtest::SyncIo io;
    StatsDialog dialog(reads, io);
    dialog.show();
    CHECK(dialog.pages()->currentIndex() == 0);
    CHECK(dialog.loadingLabel()->text() == q("Stats couldn't be counted right now."));
    dialog.close();
}

TEST_CASE("reset asks first then starts over", "[test_personalize_s9_stats_dialog][widget]") {
    // from tests/test_personalize_s9_stats_dialog.py::test_reset_asks_first_then_starts_over
    Window window;
    fvtest::seedThreeDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();

    QMessageBox::StandardButton answer = QMessageBox::No;
    dialog.askReset = [&answer](QWidget*, const QString&, const QString&) {
        return answer;
    };
    dialog.resetButton()->click();
    CHECK(dialog.pages()->currentIndex() == 2);

    answer = QMessageBox::Yes;
    dialog.resetButton()->click();
    CHECK(dialog.pages()->currentIndex() == 1);
    CHECK(window.stats.load().entries == 0);
    dialog.close();
}

TEST_CASE("show stats keeps one window and leaves settings alone",
          "[test_personalize_s9_stats_dialog][widget]") {
    // from
    // tests/test_personalize_s9_stats_dialog.py::test_show_stats_keeps_one_window_and_leaves_settings_alone
    StatsWiring wiring;
    fv::StatsPresenter* presenter = wiring.presenter();
    REQUIRE(presenter != nullptr);
    const fv::Json before = wiring.settings().contents();
    presenter->showStats();
    QPointer<StatsDialog> first = presenter->dialog();
    REQUIRE(first != nullptr);
    CHECK(first->isVisible());
    CHECK_FALSE(first->isModal());
    presenter->showStats();
    CHECK(presenter->dialog() == first);
    first->close();
    presenter->showStats();
    CHECK(presenter->dialog() == first);
    CHECK(first->isVisible());
    CHECK(wiring.settings().contents() == before);
    first->close();
    delete first.data();
    CHECK(first == nullptr);
    presenter->showStats();
    REQUIRE(presenter->dialog() != nullptr);
    CHECK(presenter->dialog()->isVisible());
    presenter->dialog()->close();
}

TEST_CASE("it fits narrow windows at large fonts", "[test_personalize_s9_stats_dialog][widget]") {
    // from tests/test_personalize_s9_stats_dialog.py::test_it_fits_narrow_windows_at_large_fonts
    // The "omarchy" variant is Linux's Omarchy desktop look, which FioriVoce does not port.
    const int width = GENERATE(720, 460);
    CAPTURE(width);
    const fvtest::AppTheme theme(QStringLiteral("dark"), 130);
    Window window;
    fvtest::seedThreeDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();
    dialog.resize(width, 640);
    for (int round = 0; round < 8; ++round) {
        QApplication::processEvents();
    }
    QWidget* page = dialog.pages()->currentWidget();
    CHECK(dialog.width() == width);
    std::vector<QWidget*> controls;
    for (QAbstractButton* button : dialog.findChildren<QAbstractButton*>()) {
        controls.push_back(button);
    }
    for (QLabel* label : page->findChildren<QLabel*>()) {
        controls.push_back(label);
    }
    for (QWidget* control : controls) {
        if (!control->isVisible()) {
            continue;
        }
        CAPTURE(control->objectName().toStdString());
        const int right = control->mapTo(&dialog, control->rect().bottomRight()).x();
        CHECK(control->mapTo(&dialog, control->rect().topLeft()).x() >= 0);
        CHECK(right < dialog.width());
    }
    for (QLabel* label : page->findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            CAPTURE(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    dialog.close();
}

// ---- tests/test_personalize_fix2_g3_stats_chart.py ----

TEST_CASE("the chart reads out every day", "[test_personalize_fix2_g3_stats_chart][widget]") {
    // from tests/test_personalize_fix2_g3_stats_chart.py::test_the_chart_reads_out_every_day
    Window window;
    fvtest::seedChartDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();
    fv::BarChart* bars = dialog.bars();
    CHECK(bars->accessibleName() == q("Words per day, last 7 days"));
    const QStringList days = bars->accessibleDescription().split(q("; "));
    REQUIRE(days.size() == 7);
    CHECK(days.back() == q("Today: 7 words"));
    // Yesterday is Monday, October 5; the first day Wednesday, September 30.
    CHECK(days[5] == q("Mon, Oct 5: 4 words"));
    CHECK(days[4].endsWith(q(": 0 words")));
    CHECK(days.front().startsWith(q("Wed, Sep 30: ")));
    dialog.close();
}

TEST_CASE("the chart text follows a reload", "[test_personalize_fix2_g3_stats_chart][widget]") {
    // from tests/test_personalize_fix2_g3_stats_chart.py::test_the_chart_text_follows_a_reload
    Window window;
    StatsDialog& dialog = window.make();
    fv::BarChart* bars = dialog.bars();
    bars->setBars({{q("Yesterday"), 1, {}}, {q("Today"), 2, {}}}, q("Words per day, last 2 days"));
    CHECK(bars->accessibleName() == q("Words per day, last 2 days"));
    CHECK(bars->accessibleDescription().endsWith(q("; Today: 2 words")));
    bars->setBars({}, q("Words per day"));
    CHECK(bars->accessibleDescription().isEmpty());
}

TEST_CASE("the activity map summarises the year",
          "[test_personalize_fix2_g3_stats_chart][widget]") {
    // from tests/test_personalize_fix2_g3_stats_chart.py::test_the_activity_map_summarises_the_year
    Window window;
    fvtest::seedChartDays(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();
    fv::ActivityMap* activity = dialog.activity();
    CHECK(activity->accessibleName() == q("Activity, a square per day"));
    CHECK(activity->accessibleDescription() ==
          q("3 days with words; the busiest, October 6, had 7 words"));
    CHECK(std::vector<int>{activity->level(0), activity->level(3), activity->level(4),
                           activity->level(7)} == std::vector<int>{0, 1, 2, 4});
    dialog.close();
}

// ---- tests/test_personalize_fix2_g3_stats_dialog.py ----

namespace {

QStringList defaultButtons(const QWidget& dialog) {
    QStringList out;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->isDefault()) {
            out << button->text();
        }
    }
    return out;
}

} // namespace

TEST_CASE("enter closes the window instead of offering a reset",
          "[test_personalize_fix2_g3_stats_dialog][widget]") {
    // from
    // tests/test_personalize_fix2_g3_stats_dialog.py::test_enter_closes_the_window_instead_of_offering_a_reset
    const Qt::Key key = GENERATE(Qt::Key_Return, Qt::Key_Enter);
    Window window;
    fvtest::seedChartDays(window.stats);
    StatsDialog& dialog = window.make();
    QStringList asked;
    dialog.askReset = [&asked](QWidget*, const QString& title, const QString&) {
        asked << title;
        return QMessageBox::No;
    };
    for (int opening = 0; opening < 2; ++opening) {
        dialog.show();
        CHECK(dialog.resetButton()->isEnabled());
        const Qt::KeyboardModifiers keypad =
            key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier;
        QWidget* target = dialog.focusWidget() != nullptr ? dialog.focusWidget() : &dialog;
        QTest::keyClick(target, key, keypad);
        CHECK(asked.isEmpty());
        CHECK_FALSE(dialog.isVisible());
        CHECK(defaultButtons(dialog) == QStringList{q("Close")});
    }
}

TEST_CASE("enter on the focused reset button still asks first",
          "[test_personalize_fix2_g3_stats_dialog][widget]") {
    // from
    // tests/test_personalize_fix2_g3_stats_dialog.py::test_enter_on_the_focused_reset_button_still_asks_first
    Window window;
    fvtest::seedChartDays(window.stats);
    StatsDialog& dialog = window.make();
    QStringList asked;
    dialog.askReset = [&asked](QWidget*, const QString& title, const QString&) {
        asked << title;
        return QMessageBox::No;
    };
    dialog.show();
    dialog.resetButton()->setFocus();
    QTest::keyClick(dialog.resetButton(), Qt::Key_Return);
    CHECK(asked == QStringList{q("Reset stats")});
    CHECK(dialog.isVisible());
    REQUIRE(dialog.summary());
    CHECK(dialog.summary()->entries != 0);
    dialog.close();
}

// ---- the window half of tests/test_stats_features.py ----

TEST_CASE("the window lists a row for each feature ever used", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_the_window_lists_a_row_for_each_feature_ever_used
    Window window;
    seedWindow(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();

    CHECK_FALSE(dialog.featuresSection()->isHidden());
    using Row = StatsDialog::FeatureRowText;
    CHECK(dialog.featureRows() ==
          std::vector<Row>{
              {q("files"), q("Audio files"), q("1 file · 10 min of audio · 2 words")},
              {q("walkthroughs"), q("Walkthroughs"), q("None in this span")},
              {q("actions"), q("Actions"), q("1 sent · Claude 1")},
              {q("snippets"), q("Snippets"), q("1 used · wrote 12 words for you")},
          });
    dialog.periodBar()->buttons()[1]->click();
    REQUIRE(dialog.featureRows().size() > 1);
    CHECK(dialog.featureRows()[1] ==
          Row{q("walkthroughs"), q("Walkthroughs"), q("1 walkthrough · 3 screenshots")});
    dialog.close();
}

TEST_CASE("the window breaks the words down", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_the_window_breaks_the_words_down
    Window window;
    seedWindow(window.stats);
    StatsDialog& dialog = window.make();
    dialog.show();

    CHECK(dialog.categoryBreakdown().legend()->text() == q("Work 67% · Unrecognized apps 33%"));
    CHECK(dialog.cleanupBreakdown().legend()->text() == q("Medium 67% · Off 33%"));
    CHECK(dialog.languageBreakdown().legend()->text() == q("English 67% · Italian 33%"));
    CHECK(dialog.categoryBreakdown().bar()->accessibleDescription() ==
          q("Work: 67%; Unrecognized apps: 33%"));
    std::vector<bool> named;
    for (const fv::SplitBar::Part& part : dialog.cleanupBreakdown().bar()->parts()) {
        named.push_back(part.named);
    }
    CHECK(named == std::vector<bool>{true, false});
    CHECK_FALSE(dialog.breakdownSection()->isHidden());
    dialog.close();
}

TEST_CASE("the breakdown is hidden when nothing names one", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_the_breakdown_is_hidden_when_nothing_names_one
    Window window;
    ds::EntryFields plain;
    plain.text = "plain words";
    plain.entry_kind = "dictation";
    plain.timestamp = LocalStats::noon();
    plain.language = "en";
    window.stats.record(plain);
    StatsDialog& dialog = window.make();
    dialog.show();
    CHECK(dialog.languageBreakdown().widget()->isHidden());
    CHECK(dialog.breakdownSection()->isHidden());
    dialog.close();
}

TEST_CASE("a window with only a file is not empty", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_a_window_with_only_a_file_is_not_empty
    Window window;
    ds::EntryFields talk;
    talk.text = "a talk";
    talk.entry_kind = "file";
    talk.timestamp = LocalStats::noon();
    window.stats.record(talk);
    StatsDialog& dialog = window.make();
    dialog.show();
    CHECK(dialog.pages()->currentIndex() == 2);
    CHECK(dialog.heroValue()->text() == q("0 words"));
    CHECK(dialog.hourLabel()->text() == q("Nothing dictated in this span yet"));
    REQUIRE(dialog.featureRows().size() == 1);
    CHECK(dialog.featureRows()[0].key == q("files"));
    dialog.close();
}

TEST_CASE("the hour grid reads out its busiest slot", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_the_hour_grid_reads_out_its_busiest_slot
    fv::HourGrid grid;
    CHECK_FALSE(grid.peak());
    fv::stats_view::HourWords words{};
    words[3][10] = 12;
    words[0][9] = 4;
    grid.setGrid(words);
    CHECK(grid.peak() == std::pair{3, 10});
    CHECK(grid.accessibleDescription() == q("Busiest on Thursdays around 10 AM, with 12 words"));
    CHECK(std::vector<int>{grid.level(0), grid.level(4), grid.level(12)} ==
          std::vector<int>{0, 1, 4});
    grid.setGrid(fv::stats_view::HourWords{});
    CHECK(grid.accessibleDescription() == q("No words yet"));
}

TEST_CASE("a feature trend words its columns by what it counts", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_a_feature_trend_words_its_columns_by_what_it_counts
    fv::BarChart trend(22, q("accent"), nullptr,
                       [](std::int64_t count) { return QStringLiteral("%1 sent").arg(count); });
    trend.setBars({{q("Mon"), 2, {}}, {q("Today"), 1, {}}}, q("Actions over this span"));
    CHECK(trend.accessibleDescription() == q("Mon: 2 sent; Today: 1 sent"));
}

TEST_CASE("a feature row opens its feature by click or key", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_a_feature_row_opens_its_feature_by_click_or_key
    Window window;
    seedWindow(window.stats);
    StatsDialog& dialog = window.make();
    QStringList asked;
    QObject::connect(&dialog, &StatsDialog::featureRequested,
                     [&asked](const QString& key) { asked << key; });
    dialog.show();
    std::vector<fv::FeatureRow*> rows;
    for (fv::FeatureRow* row : dialog.findChildren<fv::FeatureRow*>()) {
        if (row->isVisible()) {
            rows.push_back(row);
        }
    }
    QStringList keys;
    for (fv::FeatureRow* row : rows) {
        keys << row->key();
    }
    REQUIRE(keys == QStringList{q("files"), q("walkthroughs"), q("actions"), q("snippets")});
    fv::FeatureRow* files = rows[0];
    fv::FeatureRow* actions = rows[2];
    fv::FeatureRow* snippets = rows[3];
    CHECK(files->toolTip() == q("Open History on the Upload File tab"));
    CHECK(files->accessibleName() == q("Audio files: 1 file · 10 min of audio · 2 words"));

    QApplication::processEvents();
    CHECK(files->width() > 100);

    QTest::mouseClick(files, Qt::LeftButton);
    QTest::mouseClick(actions, Qt::RightButton);
    snippets->setFocus();
    QTest::keyClick(snippets, Qt::Key_Space);
    QTest::keyClick(snippets, Qt::Key_Return);

    CHECK(asked == QStringList{q("files"), q("snippets"), q("snippets")});
    CHECK(dialog.isVisible());
    dialog.close();
}

TEST_CASE("show stats sends a row to the app", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_show_stats_sends_a_row_to_the_app
    StatsWiring wiring;
    fv::StatsPresenter* presenter = wiring.presenter();
    REQUIRE(presenter != nullptr);
    std::vector<int> tabs;
    presenter->pastItemsOpener = [&tabs](int tab) {
        tabs.push_back(tab);
    };
    presenter->showStats();
    REQUIRE(presenter->dialog() != nullptr);
    Q_EMIT presenter->dialog()->featureRequested(q("meetings"));
    CHECK(tabs == std::vector<int>{fv::TabbedContentWidget::TabMeetingMode});
    presenter->dialog()->close();
}

TEST_CASE("each feature opens its past items or its settings page",
          "[test_stats_features][widget]") {
    // from
    // tests/test_stats_features.py::test_each_feature_opens_its_past_items_or_its_settings_page The
    // Settings pages are reached through the window's settingsDestinationRequested, which the
    // Settings feature hands to SettingsWindow::open; this records it in its place.
    StatsWiring wiring;
    fv::StatsPresenter* presenter = wiring.presenter();
    REQUIRE(presenter != nullptr);
    std::vector<int> tabs;
    QStringList pages;
    presenter->pastItemsOpener = [&tabs](int tab) {
        tabs.push_back(tab);
    };
    QObject::connect(&wiring.window(), &fv::MainWindow::settingsDestinationRequested,
                     [&pages](const QString& destination) { pages << destination; });
    for (const std::string_view key : ds::FEATURES) {
        presenter->openStatsFeature(fv::qt::toQString(key));
    }
    presenter->openStatsFeature(q("unknown"));

    CHECK(tabs == std::vector<int>{fv::TabbedContentWidget::TabMeetingMode,
                                   fv::TabbedContentWidget::TabUploadFile,
                                   fv::TabbedContentWidget::TabWalkthrough});
    CHECK(pages == QStringList{q("actions"), q("commands"), q("snippets"), q("dictionary")});
}

TEST_CASE("the main window shows a tab with its list open", "[test_stats_features][widget]") {
    // from tests/test_stats_features.py::test_the_main_window_shows_a_tab_with_its_list_open
    const bool expanded = GENERATE(false, true);
    CAPTURE(expanded);
    StatsWiring wiring;
    fv::MainWindow& window = wiring.window();
    if (expanded) {
        window.toggleSidePanel();
        REQUIRE(window.isSidePanelExpanded());
    }
    wiring.presenter()->showPastItems(fv::TabbedContentWidget::TabWalkthrough);
    CHECK(window.isVisible());
    CHECK(window.tabbedContent()->currentIndex() == fv::TabbedContentWidget::TabWalkthrough);
    // Opened when it was closed, left open (not toggled shut) when it was open.
    CHECK(window.isSidePanelExpanded());
}

TEST_CASE("the footer view menu and tray open the one stats window", "[widget]") {
    // The wiring of ui_controller.py:467 and 492 (main_window.stats_requested and the tray's).
    StatsWiring wiring;
    fv::StatsPresenter* presenter = wiring.presenter();
    REQUIRE(presenter != nullptr);
    Q_EMIT wiring.window().statsRequested();
    QPointer<StatsDialog> dialog = presenter->dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
    dialog->close();
    QAction* tray = wiring.menus().action(fv::tray_id::stats);
    REQUIRE(tray != nullptr);
    tray->trigger();
    CHECK(presenter->dialog() == dialog);
    CHECK(dialog->isVisible());
    dialog->close();
}
