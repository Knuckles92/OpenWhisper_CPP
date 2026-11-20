#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QDialog>
#include <QFrame>
#include <QMessageBox>
#include <QString>
#include <QWidget>

#include "base/subscription.h"
#include "history/stats_summary.h"
#include "ui_history/stats_charts.h"

class QBoxLayout;
class QGridLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace fv {

class Button;
class ElidingLabel;
class Executor;
class SegmentedBar;
class WrappedLabel;

namespace dictation_stats {
class StatsRepository;
}

// The words of ui_qt/dialogs/stats_dialog.py's text helpers.
namespace stats_view {

// _days: "1 day", "3 days".
std::string days(std::int64_t count);
// _short_day: "Oct 21". _long_day: "Oct 6, 2026".
std::string short_day(dictation_stats::Day day);
std::string long_day(dictation_stats::Day day);
// _count: "1 meeting", "1,234 meetings"; many replaces one + "s" when given.
std::string count(std::int64_t count, std::string_view one, std::string_view many = {});
// _duration: "Under a minute", "12 min", "2 h 5 min", "3 h", "120 h" (from 100 h on, or with no
// minutes over); minutes rounded half to even.
std::string duration(double minutes);
// _trend: the hero chip's text and tone (up, down, flat; "" with no text) for this span against
// the one before; "Since <first day>" for all time.
std::pair<std::string, std::string> trend(const dictation_stats::PeriodStats& period,
                                          std::optional<dictation_stats::Day> first_day);
// _streak_detail: what the Day streak card says under its number.
std::string streak_detail(const dictation_stats::StatsSummary& summary);
// _feature_text: a feature row's name ("Meetings", "Audio files", ...) and its line of detail
// for a span with something in it.
std::pair<std::string, std::string> feature_text(const dictation_stats::FeatureStats& feature);
// What one column of a feature's trend counts (_TREND_TEXT): "2 meetings", "3 sent".
std::string trend_text(std::string_view key, std::int64_t count);
// Where clicking a feature's row leads (_OPENS_TEXT): "Open Past Meetings", ...
std::string opens_text(std::string_view key);

} // namespace stats_view

// What the Stats window reads: the summary, a reset, and news of each counted dictation.
// load and reset block and run on io; listen's callback runs on whichever thread counted.
struct StatsReads {
    std::function<dictation_stats::StatsSummary()> load;
    std::function<void()> reset;
    std::function<Subscription(std::function<void()>)> listen;
};

// load_summary, reset and add_listener over the one StatsRepository of the database, counting
// days in the clock's zone. stats and clock must outlive the reads.
StatsReads statsReadsOf(dictation_stats::StatsRepository& stats, const Clock& clock);

// StatCard: one number with a caption above and a detail below.
// Affinity: GUI thread.
class StatCard : public QWidget {
    Q_OBJECT
public:
    explicit StatCard(const QString& caption, QWidget* parent = nullptr);

    ElidingLabel* captionLabel() const { return caption_; }
    ElidingLabel* valueLabel() const { return value_; }
    ElidingLabel* detailLabel() const { return detail_; }
    void setValues(const QString& value, const QString& detail, const QString& tooltip = {});

private:
    ElidingLabel* caption_;
    ElidingLabel* value_;
    ElidingLabel* detail_;
};

// FeatureRow: a feature's name and detail; a left click, Space, Enter or Return opens where it
// lives.
// Affinity: GUI thread.
class FeatureRow : public QFrame {
    Q_OBJECT
public:
    FeatureRow(const QString& key, const QString& name, const QString& detail,
               QWidget* parent = nullptr);

    const QString& key() const { return key_; }

Q_SIGNALS:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QString key_;
};

// One line of the breakdown section: a caption, its shares in words, and a bar.
// Affinity: GUI thread.
class StatsBreakdown {
public:
    StatsBreakdown(const QString& caption, QVBoxLayout* column);

    QWidget* widget() const { return widget_; }
    WrappedLabel* legend() const { return legend_; }
    SplitBar* bar() const { return bar_; }
    // Draws shares (key, words), each named by nameOf; hidden, and false, with fewer than least
    // of them or no words.
    bool show(const std::vector<std::pair<std::string, std::int64_t>>& shares,
              const std::function<QString(const std::string&)>& nameOf, std::size_t least = 1);

private:
    QString caption_;
    QWidget* widget_;
    WrappedLabel* legend_;
    SplitBar* bar_;
};

// The non-modal Stats window (ui_qt/dialogs/stats_dialog.py StatsDialog): the words dictated over
// a chosen span, then a row for each other thing the app did. Counting runs on io and returns
// through a session-lived relay with a token, so a slower older count never replaces a newer one
// and no worker holds the window. It counts again when shown (not spontaneously) and 400 ms after
// a dictation is counted while it is visible; the period switch re-renders the held summary.
// Affinity: GUI thread.
class StatsDialog : public QDialog {
    Q_OBJECT
public:
    // Width, at 100 % font, from which the four numbers share a row and sections sit side by
    // side; from which the trend sits beside the words.
    static constexpr int kWideWidth = 720;
    static constexpr int kHeroRowWidth = 520;
    static constexpr int kRefreshMs = 400;

    // Without listen the window hears of no new dictation.
    StatsDialog(StatsReads reads, Executor& io, QWidget* parent = nullptr);
    ~StatsDialog() override;

    // show_stats on an existing window: counts again when visible, then shows and raises it.
    void showStats();
    // Count again on io; an older count that finishes later is dropped.
    void reload();
    // The Reset stats… button: asks first, then resets and counts again.
    void confirmReset();
    // Asks the reset question; QMessageBox::question by default.
    std::function<QMessageBox::StandardButton(QWidget* parent, const QString& title,
                                              const QString& text)>
        askReset;

    const QString& periodKey() const { return periodKey_; }
    const std::optional<dictation_stats::StatsSummary>& summary() const { return summary_; }
    const std::vector<std::pair<QString, std::int64_t>>& appRows() const { return appRows_; }
    struct FeatureRowText {
        QString key;
        QString name;
        QString detail;
        friend bool operator==(const FeatureRowText&, const FeatureRowText&) = default;
    };
    const std::vector<FeatureRowText>& featureRows() const { return featureRows_; }

    QStackedWidget* pages() const { return pages_; }
    QLabel* loadingLabel() const { return loadingLabel_; }
    WrappedLabel* emptyText() const { return emptyText_; }
    QPushButton* resetButton() const { return resetButton_; }
    QPushButton* closeButton() const { return closeButton_; }
    SegmentedBar* periodBar() const { return periodBar_; }
    ElidingLabel* heroCaption() const { return heroCaption_; }
    ElidingLabel* heroValue() const { return heroValue_; }
    QLabel* trendChip() const { return trendChip_; }
    QLabel* chartTitle() const { return chartTitle_; }
    QLabel* chartAverage() const { return chartAverage_; }
    ElidingLabel* bestLabel() const { return bestLabel_; }
    BarChart* bars() const { return bars_; }
    StatCard* savedCard() const { return savedCard_; }
    StatCard* paceCard() const { return paceCard_; }
    StatCard* dictationsCard() const { return dictationsCard_; }
    StatCard* streakCard() const { return streakCard_; }
    QFrame* featuresSection() const { return featuresSection_; }
    ElidingLabel* hourLabel() const { return hourLabel_; }
    HourGrid* rhythm() const { return rhythm_; }
    WrappedLabel* noAppsLabel() const { return noAppsLabel_; }
    QFrame* breakdownSection() const { return breakdownSection_; }
    ElidingLabel* cleanupLabel() const { return cleanupLabel_; }
    StatsBreakdown& categoryBreakdown() { return *categoryBreakdown_; }
    StatsBreakdown& cleanupBreakdown() { return *cleanupBreakdown_; }
    StatsBreakdown& languageBreakdown() { return *languageBreakdown_; }
    ElidingLabel* activeDaysLabel() const { return activeDaysLabel_; }
    ActivityMap* activity() const { return activity_; }
    ElidingLabel* milestoneLabel() const { return milestoneLabel_; }
    QLabel* milestoneLeft() const { return milestoneLeft_; }
    ShareBar* milestoneBar() const { return milestoneBar_; }
    QTimer* refreshTimer() const { return refresh_; }
    // Whether the cards share one row (the wide layout).
    bool isWide() const { return wide_.value_or(false); }

    // _apply: what a count delivers, for its token. A stale token is dropped.
    void apply(std::uint64_t token, const std::optional<dictation_stats::StatsSummary>& summary,
               const QString& error);
    // The token of the newest count asked for.
    std::uint64_t token() const { return token_; }

Q_SIGNALS:
    // A feature row was activated; carries its FEATURES key.
    void featureRequested(const QString& key);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void build();
    QWidget* buildEmpty();
    QWidget* buildContent();
    QFrame* buildHeadline();
    QFrame* buildFeatures();
    QFrame* buildRhythm();
    QFrame* buildApps();
    QFrame* buildBreakdown();
    QFrame* buildActivity();
    void fitPagesToCurrent(int index);
    void setWide(bool wide);
    void onRecorded();
    void choosePeriod(int index);
    void render(const dictation_stats::StatsSummary& summary);
    void showHero(const dictation_stats::StatsSummary& summary,
                  const dictation_stats::PeriodStats& period);
    void showCards(const dictation_stats::StatsSummary& summary,
                   const dictation_stats::PeriodStats& period);
    std::vector<ChartBar> showChart(const dictation_stats::PeriodStats& period);
    void showFeatures(const dictation_stats::StatsSummary& summary,
                      const dictation_stats::PeriodStats& period,
                      const std::vector<ChartBar>& bars);
    void showRhythm(const dictation_stats::PeriodStats& period);
    void showApps(const dictation_stats::PeriodStats& period);
    void showBreakdown(const dictation_stats::PeriodStats& period);
    void showActivity(const dictation_stats::StatsSummary& summary);
    // _run: work on io, its result delivered through the relay with token.
    void run(std::function<dictation_stats::StatsSummary()> work, std::uint64_t token);

    StatsReads reads_;
    Executor& io_;
    Subscription recorded_;
    std::uint64_t token_ = 0;
    std::optional<bool> wide_;
    QString periodKey_ = QStringLiteral("week");
    std::optional<dictation_stats::StatsSummary> summary_;
    std::vector<std::pair<QString, std::int64_t>> appRows_;
    std::vector<FeatureRowText> featureRows_;
    QTimer* refresh_;

    QStackedWidget* pages_ = nullptr;
    QLabel* loadingLabel_ = nullptr;
    WrappedLabel* emptyText_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    SegmentedBar* periodBar_ = nullptr;
    QBoxLayout* heroRow_ = nullptr;
    ElidingLabel* heroCaption_ = nullptr;
    ElidingLabel* heroValue_ = nullptr;
    QLabel* trendChip_ = nullptr;
    QLabel* chartTitle_ = nullptr;
    QLabel* chartAverage_ = nullptr;
    ElidingLabel* bestLabel_ = nullptr;
    BarChart* bars_ = nullptr;
    StatCard* savedCard_ = nullptr;
    StatCard* paceCard_ = nullptr;
    StatCard* dictationsCard_ = nullptr;
    StatCard* streakCard_ = nullptr;
    std::vector<StatCard*> cards_;
    QGridLayout* cardGrid_ = nullptr;
    QFrame* featuresSection_ = nullptr;
    QGridLayout* featuresGrid_ = nullptr;
    QGridLayout* pairGrid_ = nullptr;
    std::vector<QFrame*> pairs_;
    ElidingLabel* hourLabel_ = nullptr;
    HourGrid* rhythm_ = nullptr;
    QGridLayout* appsGrid_ = nullptr;
    WrappedLabel* noAppsLabel_ = nullptr;
    QFrame* breakdownSection_ = nullptr;
    ElidingLabel* cleanupLabel_ = nullptr;
    std::unique_ptr<StatsBreakdown> categoryBreakdown_;
    std::unique_ptr<StatsBreakdown> cleanupBreakdown_;
    std::unique_ptr<StatsBreakdown> languageBreakdown_;
    ElidingLabel* activeDaysLabel_ = nullptr;
    ActivityMap* activity_ = nullptr;
    ElidingLabel* milestoneLabel_ = nullptr;
    QLabel* milestoneLeft_ = nullptr;
    ShareBar* milestoneBar_ = nullptr;
};

} // namespace fv
