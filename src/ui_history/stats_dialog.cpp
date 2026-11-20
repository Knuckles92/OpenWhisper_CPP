#include "ui_history/stats_dialog.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

#include <QBoxLayout>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "history/dictation_stats.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "speech/dictation_language.h"
#include "ui_history/detail/stats_relay.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace stats_detail {

StatsRelay& StatsRelay::instance() {
    // Never deleted: a worker may emit on it after every window, and the application, is gone.
    static StatsRelay* const relay = new StatsRelay;
    return *relay;
}

std::uint64_t StatsRelay::nextToken() {
    static std::atomic<std::uint64_t> tokens{0};
    return tokens.fetch_add(1) + 1;
}

} // namespace stats_detail

namespace stats_view {
namespace {

namespace ds = dictation_stats;

std::string monthDay(ds::Day day) {
    return std::format("{} {}", month_abbr(static_cast<unsigned>(day.month())),
                       static_cast<unsigned>(day.day()));
}

std::int64_t roundHalfEven(double value) {
    return static_cast<std::int64_t>(py::round_half_even(value));
}

std::string joinParts(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += " · ";
        }
        out += part;
    }
    return out;
}

} // namespace

std::string days(std::int64_t count) {
    return std::format("{} day{}", count, count == 1 ? "" : "s");
}

std::string short_day(ds::Day day) {
    return monthDay(day);
}

std::string long_day(ds::Day day) {
    return std::format("{}, {}", monthDay(day), static_cast<int>(day.year()));
}

std::string count(std::int64_t count, std::string_view one, std::string_view many) {
    std::string noun(one);
    if (count != 1) {
        noun = many.empty() ? std::string(one) + "s" : std::string(many);
    }
    return thousands(count) + " " + noun;
}

std::string duration(double minutes) {
    if (0 < minutes && minutes < 1) {
        return "Under a minute";
    }
    const std::int64_t whole = roundHalfEven(minutes);
    if (whole < 60) {
        return std::format("{} min", whole);
    }
    // divmod: whole is at least 60 here, so both parts are non-negative.
    const std::int64_t hours = whole / 60;
    const std::int64_t rest = whole % 60;
    if (hours >= 100 || rest == 0) {
        return thousands(hours) + " h";
    }
    return std::format("{} h {} min", hours, rest);
}

std::pair<std::string, std::string> trend(const ds::PeriodStats& period,
                                          std::optional<ds::Day> first_day) {
    if (!period.days) {
        if (first_day) {
            return {"Since " + long_day(*first_day), "flat"};
        }
        return {"", ""};
    }
    const std::int64_t previous = period.previous_words.value_or(0);
    if (previous == 0) {
        return {"", ""};
    }
    const std::int64_t change = roundHalfEven(static_cast<double>((period.words - previous) * 100) /
                                              static_cast<double>(previous));
    std::string before;
    for (const auto& [key, text] :
         {std::pair{"week", "the 7 days before"}, std::pair{"month", "the 30 days before"}}) {
        if (period.key == key) {
            before = text;
        }
    }
    if (change > 0) {
        return {std::format("↑ {}% vs {}", change, before), "up"};
    }
    if (change < 0) {
        return {std::format("↓ {}% vs {}", -change, before), "down"};
    }
    return {"Same as " + before, "flat"};
}

std::string streak_detail(const ds::StatsSummary& summary) {
    std::string detail;
    if (summary.dictated_today) {
        detail = summary.day_streak == 1 ? "You dictated today" : "Keep it going";
    } else if (summary.day_streak != 0) {
        detail = "Dictate today to keep it";
    } else {
        detail = "Dictate today to start one";
    }
    if (summary.longest_streak > summary.day_streak) {
        return std::format("{} · best {}", detail, summary.longest_streak);
    }
    return detail;
}

std::pair<std::string, std::string> feature_text(const ds::FeatureStats& feature) {
    const std::string& key = feature.key;
    const std::int64_t n = feature.count;
    const std::string length = feature.seconds >= 1 ? duration(feature.seconds / 60) : "";
    if (key == "meetings") {
        std::vector<std::string> parts = {count(n, "meeting"),
                                          length.empty() ? "" : length + " captured"};
        if (n > 1 && !length.empty()) {
            parts.push_back("avg " + duration(feature.seconds / 60 / static_cast<double>(n)));
        }
        return {"Meetings", joinParts(parts)};
    }
    if (key == "files") {
        return {"Audio files",
                joinParts({count(n, "file"), length.empty() ? "" : length + " of audio",
                           feature.words != 0 ? words_text(feature.words) : ""})};
    }
    if (key == "walkthroughs") {
        return {"Walkthroughs",
                joinParts({count(n, "walkthrough"),
                           feature.shots != 0 ? count(feature.shots, "screenshot") : ""})};
    }
    if (key == "actions") {
        // Joined as they are: an action named "" still takes its place.
        std::string line = thousands(n) + " sent";
        for (const auto& [name, times] : feature.top) {
            line += " · " + name + " " + thousands(times);
        }
        return {"Actions", line};
    }
    if (key == "commands") {
        return {"Commands",
                joinParts({thousands(n) + " run",
                           feature.words != 0 ? words_text(feature.words) + " rewritten" : ""})};
    }
    if (key == "snippets") {
        return {"Snippets",
                joinParts(
                    {thousands(n) + " used",
                     feature.words != 0 ? "wrote " + words_text(feature.words) + " for you" : ""})};
    }
    return {"Dictionary", count(n, "correction") + " applied"};
}

std::string trend_text(std::string_view key, std::int64_t n) {
    if (key == "meetings") {
        return count(n, "meeting");
    }
    if (key == "files") {
        return count(n, "file");
    }
    if (key == "walkthroughs") {
        return count(n, "walkthrough");
    }
    if (key == "actions") {
        return thousands(n) + " sent";
    }
    if (key == "commands") {
        return thousands(n) + " run";
    }
    if (key == "snippets") {
        return thousands(n) + " used";
    }
    return count(n, "correction");
}

std::string opens_text(std::string_view key) {
    if (key == "meetings") {
        return "Open Past Meetings";
    }
    if (key == "files") {
        return "Open History on the Upload File tab";
    }
    if (key == "walkthroughs") {
        return "Open your walkthroughs";
    }
    if (key == "actions") {
        return "Open Actions in Settings";
    }
    if (key == "commands") {
        return "Open Commands in Settings";
    }
    if (key == "snippets") {
        return "Open Snippets in Settings";
    }
    return "Open the dictionary in Settings";
}

} // namespace stats_view

namespace {

namespace ds = dictation_stats;
namespace sv = stats_view;
using stats_detail::StatsRelay;

constexpr log::Logger kLog{"ui_qt.dialogs.stats_dialog"};

QString q(const std::string& utf8) {
    return qt::toQString(utf8);
}

QString q(std::string_view utf8) {
    return qt::toQString(utf8);
}

// _PERIOD_TEXT: the switch label, the hero caption and "the span before" of each period key.
struct PeriodText {
    const char* label;
    const char* caption;
};

PeriodText periodText(std::string_view key) {
    if (key == "week") {
        return {"Week", "Last 7 days"};
    }
    if (key == "month") {
        return {"Month", "Last 30 days"};
    }
    return {"All time", "All time"};
}

// _CATEGORY_TEXT and _CLEANUP_TEXT, with str.title() for a key they do not name.
QString categoryText(const std::string& key) {
    static const std::vector<std::pair<std::string_view, const char*>> kNames = {
        {"email", "Email"},
        {"work", "Work"},
        {"personal", "Personal"},
        {"other", "Other apps"},
        {"", "Unrecognized apps"}};
    for (const auto& [name, text] : kNames) {
        if (name == key) {
            return QString::fromUtf8(text);
        }
    }
    return q(py::title(key));
}

QString cleanupText(const std::string& key) {
    static const std::vector<std::pair<std::string_view, const char*>> kNames = {
        {"light", "Light"}, {"medium", "Medium"}, {"high", "High"}, {"", "Off"}};
    for (const auto& [name, text] : kNames) {
        if (name == key) {
            return QString::fromUtf8(text);
        }
    }
    return q(py::title(key));
}

QString languageText(const std::string& code) {
    if (code.empty()) {
        return QStringLiteral("Not set");
    }
    try {
        return q(speech::label(code));
    } catch (const std::exception& error) {
        kLog.debug("Could not label dictation language {}; showing the code: {}", code,
                   error.what());
        return q(code);
    }
}

// _label(text, name): an ElidingLabel, or with eliding=False a plain QLabel.
ElidingLabel* label(const QString& text, const char* name) {
    auto* made = new ElidingLabel(text);
    made->setObjectName(QString::fromLatin1(name));
    return made;
}

QLabel* plainLabel(const QString& text, const char* name) {
    auto* made = new QLabel(text);
    made->setObjectName(QString::fromLatin1(name));
    return made;
}

QLabel* glyph(const QString& icon, const QString& tone, int size, int iconSize) {
    auto* made = new QLabel;
    made->setObjectName(QStringLiteral("statsCardIcon"));
    ui::setStyleProperty(made, ui::prop::tone, tone);
    made->setFixedSize(size, size);
    made->setAlignment(Qt::AlignCenter);
    made->setPixmap(ui::tablerIcon(icon).pixmap(iconSize, iconSize));
    return made;
}

// _section: a rounded section with a heading row; its column and that row come back too.
struct Section {
    QFrame* frame;
    QVBoxLayout* column;
    QHBoxLayout* heading;
};

Section section(const QString& title) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("statsSection"));
    frame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(10);
    auto* heading = new QHBoxLayout;
    heading->setSpacing(8);
    heading->addWidget(plainLabel(title, "statsSectionTitle"));
    column->addLayout(heading);
    return {frame, column, heading};
}

// _clear: empties a grid; its widgets are hidden at once, so switching periods draws the new rows
// before the old ones are deleted.
void clearGrid(QGridLayout* grid) {
    while (grid->count() != 0) {
        QLayoutItem* item = grid->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

int scaledRound(double px) {
    return static_cast<int>(py::round_half_even(px * ui::currentUiFontScale()));
}

std::int64_t roundHalfEven(double value) {
    return static_cast<std::int64_t>(py::round_half_even(value));
}

std::string thousands(std::int64_t n) {
    return sv::thousands(n);
}

// _day_bars: a column a day; every 7th from today labelled, so 30 days stay legible.
std::vector<ChartBar> dayBars(const ds::PeriodStats& period) {
    std::vector<ChartBar> bars;
    const auto last = static_cast<std::int64_t>(period.bars.size()) - 1;
    for (std::int64_t index = 0; index <= last; ++index) {
        const auto& [day, words] = period.bars[static_cast<std::size_t>(index)];
        const std::string weekday = sv::weekday_abbr(sv::weekday_of(day));
        const bool today = index == last;
        const std::string name = today ? "Today" : weekday + ", " + sv::short_day(day);
        std::string axis;
        if (period.days.value_or(0) <= 7) {
            axis = today ? "Today" : weekday;
        } else if ((last - index) % 7 == 0) {
            axis = today ? "Today" : sv::short_day(day);
        }
        bars.push_back({q(name), words, q(axis)});
    }
    return bars;
}

std::vector<ChartBar> monthBars(const ds::PeriodStats& period) {
    std::vector<ChartBar> bars;
    for (const auto& [month, words] : period.bars) {
        const auto number = static_cast<unsigned>(month.month());
        bars.push_back(
            {q(std::format("{} {}", sv::month_name(number), static_cast<int>(month.year()))), words,
             q(sv::month_abbr(number))});
    }
    return bars;
}

} // namespace

StatsReads statsReadsOf(ds::StatsRepository& stats, const Clock& clock) {
    StatsReads reads;
    reads.load = [&stats, &clock] {
        return ds::load_summary(stats, clock);
    };
    reads.reset = [&stats] {
        ds::reset(stats);
    };
    reads.listen = [&stats](std::function<void()> listener) {
        return ds::add_listener(stats, std::move(listener));
    };
    return reads;
}

// ---- StatCard ----------------------------------------------------------------------------

StatCard::StatCard(const QString& caption, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("statsStat"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMinimumWidth(110);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    caption_ = label(caption, "statsCardCaption");
    column->addWidget(caption_);
    value_ = label(QString(), "statsCardValue");
    column->addWidget(value_);
    detail_ = label(QString(), "statsCardDetail");
    column->addWidget(detail_);
    column->addStretch();
}

void StatCard::setValues(const QString& value, const QString& detail, const QString& tooltip) {
    value_->setText(value);
    detail_->setText(detail);
    setToolTip(tooltip);
}

// ---- FeatureRow --------------------------------------------------------------------------

FeatureRow::FeatureRow(const QString& key, const QString& name, const QString& detail,
                       QWidget* parent)
    : QFrame(parent), key_(key) {
    setObjectName(QStringLiteral("statsFeatureRow"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    const QString opens = q(sv::opens_text(qt::toUtf8(key)));
    setToolTip(opens);
    setAccessibleName(name + QStringLiteral(": ") + detail);
    setAccessibleDescription(opens);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(8, 4, 8, 4);
    column->setSpacing(1);
    auto* line = new WrappedLabel(detail);
    line->setObjectName(QStringLiteral("statsMuted"));
    for (QLabel* text :
         {static_cast<QLabel*>(label(name, "statsAppName")), static_cast<QLabel*>(line)}) {
        // A click on the text is a click on the row.
        text->setAttribute(Qt::WA_TransparentForMouseEvents);
        column->addWidget(text);
    }
}

void FeatureRow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        Q_EMIT clicked();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void FeatureRow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
        event->key() == Qt::Key_Enter) {
        Q_EMIT clicked();
        return;
    }
    QFrame::keyPressEvent(event);
}

// ---- StatsBreakdown ----------------------------------------------------------------------

StatsBreakdown::StatsBreakdown(const QString& caption, QVBoxLayout* column)
    : caption_(caption), widget_(new QWidget) {
    widget_->setObjectName(QStringLiteral("statsContent"));
    auto* lines = new QVBoxLayout(widget_);
    lines->setContentsMargins(0, 0, 0, 0);
    lines->setSpacing(4);
    lines->addWidget(label(caption, "statsInsight"));
    legend_ = new WrappedLabel;
    legend_->setObjectName(QStringLiteral("statsMuted"));
    lines->addWidget(legend_);
    bar_ = new SplitBar;
    lines->addWidget(bar_);
    column->addWidget(widget_);
}

bool StatsBreakdown::show(const std::vector<std::pair<std::string, std::int64_t>>& shares,
                          const std::function<QString(const std::string&)>& nameOf,
                          std::size_t least) {
    std::int64_t total = 0;
    for (const auto& [key, words] : shares) {
        total += words;
    }
    const bool shown = total != 0 && shares.size() >= least;
    widget_->setVisible(shown);
    if (!shown) {
        bar_->setParts({}, caption_);
        legend_->setText(QString());
        return false;
    }
    std::vector<SplitBar::Part> parts;
    QStringList legend;
    for (const auto& [key, words] : shares) {
        const QString name = nameOf(key);
        parts.push_back({name, words, !key.empty()});
        legend << name + QStringLiteral(" ") +
                      QString::number(roundHalfEven(static_cast<double>(words * 100) /
                                                    static_cast<double>(total))) +
                      QStringLiteral("%");
    }
    bar_->setParts(parts, caption_);
    legend_->setText(legend.join(QStringLiteral(" · ")));
    return true;
}

// ---- StatsDialog -------------------------------------------------------------------------

StatsDialog::StatsDialog(StatsReads reads, Executor& io, QWidget* parent)
    : QDialog(parent), reads_(std::move(reads)), io_(io), refresh_(new QTimer(this)) {
    askReset = [](QWidget* owner, const QString& title, const QString& text) {
        return QMessageBox::question(owner, title, text, QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No);
    };
    setObjectName(QStringLiteral("statsDialog"));
    setWindowTitle(QStringLiteral("Stats"));
    setModal(false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setMinimumSize(360, 360);
    resize(scaledRound(640), scaledRound(720));
    refresh_->setSingleShot(true);
    refresh_->setInterval(kRefreshMs);
    connect(refresh_, &QTimer::timeout, this, &StatsDialog::reload);
    StatsRelay& relay = StatsRelay::instance();
    connect(&relay, &StatsRelay::loaded, this,
            [this](quint64 token, const stats_detail::Loaded& loaded, const QString& error) {
                std::optional<ds::StatsSummary> summary;
                if (loaded.summary) {
                    summary = *loaded.summary;
                }
                apply(token, summary, error);
            });
    connect(&relay, &StatsRelay::recorded, this, &StatsDialog::onRecorded);
    if (reads_.listen) {
        // Runs on the history-save worker: it only emits on the session-lived relay.
        recorded_ = reads_.listen([] { Q_EMIT StatsRelay::instance().recorded(); });
    }
    build();
}

StatsDialog::~StatsDialog() {
    // Before the members it could reach are gone: no callback starts after this.
    recorded_.disconnect();
}

void StatsDialog::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("statsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("statsBody"));
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(24, 22, 24, 16);
    column->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("Stats"));
    title->setObjectName(QStringLiteral("statsTitle"));
    column->addWidget(title);
    auto* intro = new WrappedLabel(QStringLiteral("What you do with %1, counted and kept on this "
                                                  "computer.")
                                       .arg(q(identity().product)));
    intro->setObjectName(QStringLiteral("statsIntro"));
    column->addWidget(intro);

    pages_ = new QStackedWidget;
    pages_->setObjectName(QStringLiteral("statsPages"));
    loadingLabel_ = new QLabel(QStringLiteral("Counting your words…"));
    loadingLabel_->setObjectName(QStringLiteral("statsMuted"));
    loadingLabel_->setAlignment(Qt::AlignCenter);
    pages_->addWidget(loadingLabel_);
    pages_->addWidget(buildEmpty());
    pages_->addWidget(buildContent());
    // A stack is as tall as its tallest page, so the short loading and empty pages sat centred in
    // the content page's height, below the fold.
    connect(pages_, &QStackedWidget::currentChanged, this, &StatsDialog::fitPagesToCurrent);
    fitPagesToCurrent(pages_->currentIndex());
    column->addWidget(pages_, 1);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    auto* footer = new QWidget;
    footer->setObjectName(QStringLiteral("statsFooter"));
    auto* row = new QHBoxLayout(footer);
    row->setContentsMargins(24, 10, 24, 16);
    row->setSpacing(8);
    resetButton_ = neutralButton(new Button(QStringLiteral("Reset stats…")));
    resetButton_->setToolTip(
        QStringLiteral("Start your counts over; History and meetings stay as they are"));
    connect(resetButton_, &QPushButton::clicked, this, &StatsDialog::confirmReset);
    resetButton_->setEnabled(false);
    row->addWidget(resetButton_);
    row->addStretch();
    closeButton_ = compactPrimaryButton(new Button(QStringLiteral("Close")));
    connect(closeButton_, &QPushButton::clicked, this, &QWidget::close);
    row->addWidget(closeButton_);
    outer->addWidget(footer);
    // Without a default, QDialog makes the first button in the focus chain, Reset, the one that
    // Enter clicks. Set once the button is in the dialog.
    closeButton_->setDefault(true);
}

void StatsDialog::fitPagesToCurrent(int index) {
    for (int pageIndex = 0; pageIndex < pages_->count(); ++pageIndex) {
        QWidget* page = pages_->widget(pageIndex);
        const QSizePolicy::Policy policy =
            pageIndex == index ? QSizePolicy::Preferred : QSizePolicy::Ignored;
        page->setSizePolicy(policy, policy);
        page->adjustSize();
    }
    pages_->adjustSize();
}

QWidget* StatsDialog::buildEmpty() {
    auto* page = new QFrame;
    page->setObjectName(QStringLiteral("statsEmpty"));
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(24, 36, 24, 36);
    column->setSpacing(8);
    column->addStretch();
    column->addWidget(glyph(QStringLiteral("microphone-blue.svg"), QStringLiteral("blue"), 40, 22),
                      0, Qt::AlignHCenter);
    auto* heading = new QLabel(QStringLiteral("Nothing to count yet"));
    heading->setObjectName(QStringLiteral("statsEmptyTitle"));
    heading->setAlignment(Qt::AlignCenter);
    column->addWidget(heading);
    emptyText_ = new WrappedLabel(QStringLiteral(
        "Dictate something, record a meeting or transcribe a file, and it shows up here."));
    emptyText_->setObjectName(QStringLiteral("statsMuted"));
    emptyText_->setAlignment(Qt::AlignCenter);
    column->addWidget(emptyText_);
    column->addStretch();
    return page;
}

QWidget* StatsDialog::buildContent() {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("statsContent"));
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(14);

    std::vector<std::pair<QString, QString>> choices;
    for (const auto& [key, span] : ds::PERIODS) {
        choices.emplace_back(QString::fromUtf8(periodText(key).label), QString());
    }
    periodBar_ = new SegmentedBar(choices, true);
    periodBar_->setAccessibleName(QStringLiteral("Stats period"));
    periodBar_->setMaximumWidth(scaledRound(330));
    for (SegmentButton* button : periodBar_->buttons()) {
        // Enter still closes the window when a period has focus.
        button->setAutoDefault(false);
    }
    connect(periodBar_, &SegmentedBar::activated, this, &StatsDialog::choosePeriod);
    column->addWidget(periodBar_);

    column->addWidget(buildHeadline());
    featuresSection_ = buildFeatures();
    column->addWidget(featuresSection_);
    pairGrid_ = new QGridLayout;
    pairGrid_->setContentsMargins(0, 0, 0, 0);
    pairGrid_->setSpacing(14);
    pairs_ = {buildRhythm(), buildApps()};
    column->addLayout(pairGrid_);
    breakdownSection_ = buildBreakdown();
    column->addWidget(breakdownSection_);
    column->addWidget(buildActivity());
    column->addStretch();
    setWide(false);
    return page;
}

QFrame* StatsDialog::buildHeadline() {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("statsSection"));
    frame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(10);

    heroRow_ = new QBoxLayout(QBoxLayout::LeftToRight);
    heroRow_->setSpacing(8);
    auto* words = new QVBoxLayout;
    words->setSpacing(2);
    heroCaption_ = label(QString(), "statsHeroCaption");
    words->addWidget(heroCaption_);
    heroValue_ = label(QString(), "statsHeroValue");
    words->addWidget(heroValue_);
    heroRow_->addLayout(words, 1);
    auto* chipRow = new QHBoxLayout;
    chipRow->setContentsMargins(0, 0, 0, 4);
    trendChip_ = plainLabel(QString(), "statsTrend");
    chipRow->addWidget(trendChip_, 0, Qt::AlignBottom);
    chipRow->addStretch();
    heroRow_->addLayout(chipRow);
    column->addLayout(heroRow_);

    auto* heading = new QHBoxLayout;
    heading->setSpacing(8);
    chartTitle_ = plainLabel(QStringLiteral("Words per day"), "statsInsight");
    heading->addWidget(chartTitle_);
    chartAverage_ = plainLabel(QString(), "statsMuted");
    heading->addWidget(chartAverage_);
    bestLabel_ = label(QString(), "statsMuted");
    bestLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    heading->addWidget(bestLabel_, 1);
    column->addLayout(heading);
    bars_ = new BarChart(96, QStringLiteral("accent"));
    column->addWidget(bars_);

    auto* rule = new QFrame;
    rule->setObjectName(QStringLiteral("statsRule"));
    rule->setFixedHeight(1);
    column->addWidget(rule);
    savedCard_ = new StatCard(QStringLiteral("Time saved"));
    paceCard_ = new StatCard(QStringLiteral("Speaking pace"));
    dictationsCard_ = new StatCard(QStringLiteral("Dictations"));
    streakCard_ = new StatCard(QStringLiteral("Day streak"));
    cards_ = {savedCard_, paceCard_, dictationsCard_, streakCard_};
    cardGrid_ = new QGridLayout;
    cardGrid_->setContentsMargins(0, 2, 0, 0);
    cardGrid_->setHorizontalSpacing(14);
    cardGrid_->setVerticalSpacing(12);
    column->addLayout(cardGrid_);
    return frame;
}

QFrame* StatsDialog::buildFeatures() {
    Section built = section(QStringLiteral("Across %1").arg(q(identity().product)));
    // The rows pad their text by 8 for the hover wash, so the section gives up 8 on the left and
    // the heading takes it back: text lines up.
    const QMargins margins = built.column->contentsMargins();
    built.column->setContentsMargins(margins.left() - 8, margins.top(), margins.right(),
                                     margins.bottom());
    built.heading->setContentsMargins(8, 0, 0, 0);
    featuresGrid_ = new QGridLayout;
    featuresGrid_->setContentsMargins(0, 0, 0, 0);
    featuresGrid_->setHorizontalSpacing(12);
    featuresGrid_->setVerticalSpacing(2);
    featuresGrid_->setColumnStretch(0, 1);
    built.column->addLayout(featuresGrid_);
    return built.frame;
}

QFrame* StatsDialog::buildRhythm() {
    Section built = section(QStringLiteral("When you dictate"));
    hourLabel_ = label(QString(), "statsInsight");
    built.column->addWidget(hourLabel_);
    rhythm_ = new HourGrid;
    built.column->addWidget(rhythm_);
    built.column->addStretch();
    return built.frame;
}

QFrame* StatsDialog::buildApps() {
    Section built = section(QStringLiteral("Top apps"));
    appsGrid_ = new QGridLayout;
    appsGrid_->setContentsMargins(0, 0, 0, 0);
    appsGrid_->setHorizontalSpacing(10);
    appsGrid_->setVerticalSpacing(10);
    appsGrid_->setColumnStretch(1, 1);
    built.column->addLayout(appsGrid_);
    noAppsLabel_ = new WrappedLabel(QStringLiteral("Apps show up here as you dictate into them."));
    noAppsLabel_->setObjectName(QStringLiteral("statsMuted"));
    built.column->addWidget(noAppsLabel_);
    built.column->addStretch();
    return built.frame;
}

QFrame* StatsDialog::buildBreakdown() {
    Section built = section(QStringLiteral("How your words break down"));
    cleanupLabel_ = label(QString(), "statsMuted");
    cleanupLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cleanupLabel_->setToolTip(QStringLiteral("Words AI cleanup replaced, added or removed"));
    built.heading->addWidget(cleanupLabel_, 1);
    categoryBreakdown_ =
        std::make_unique<StatsBreakdown>(QStringLiteral("Kind of app"), built.column);
    cleanupBreakdown_ =
        std::make_unique<StatsBreakdown>(QStringLiteral("AI cleanup"), built.column);
    languageBreakdown_ = std::make_unique<StatsBreakdown>(QStringLiteral("Language"), built.column);
    return built.frame;
}

QFrame* StatsDialog::buildActivity() {
    Section built = section(QStringLiteral("Activity"));
    activeDaysLabel_ = label(QString(), "statsMuted");
    activeDaysLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    built.heading->addWidget(activeDaysLabel_, 1);
    activity_ = new ActivityMap;
    built.column->addWidget(activity_);
    auto* milestone = new QHBoxLayout;
    milestone->setSpacing(8);
    milestoneLabel_ = label(QString(), "statsHeroDetail");
    milestone->addWidget(milestoneLabel_, 1);
    milestoneLeft_ = plainLabel(QString(), "statsHeroDetail");
    milestoneLeft_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    milestone->addWidget(milestoneLeft_);
    built.column->addLayout(milestone);
    milestoneBar_ = new ShareBar(0.0, QStringLiteral("accent"), 6);
    built.column->addWidget(milestoneBar_);
    return built.frame;
}

void StatsDialog::setWide(bool wide) {
    if (wide_ && *wide_ == wide) {
        return;
    }
    wide_ = wide;
    const int columns = wide ? 4 : 2;
    for (StatCard* card : cards_) {
        cardGrid_->removeWidget(card);
    }
    for (std::size_t index = 0; index < cards_.size(); ++index) {
        const int at = static_cast<int>(index);
        cardGrid_->addWidget(cards_[index], at / columns, at % columns);
    }
    for (int col = 0; col < 4; ++col) {
        cardGrid_->setColumnStretch(col, col < columns ? 1 : 0);
    }
    for (QFrame* pair : pairs_) {
        pairGrid_->removeWidget(pair);
    }
    for (std::size_t index = 0; index < pairs_.size(); ++index) {
        const int at = static_cast<int>(index);
        if (wide) {
            pairGrid_->addWidget(pairs_[index], 0, at);
        } else {
            pairGrid_->addWidget(pairs_[index], at, 0);
        }
    }
    for (int col = 0; col < 2; ++col) {
        pairGrid_->setColumnStretch(col, wide || col == 0 ? 1 : 0);
    }
}

void StatsDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    const double scale = ui::currentUiFontScale();
    setWide(width() >= kWideWidth * scale);
    heroRow_->setDirection(width() >= kHeroRowWidth * scale ? QBoxLayout::LeftToRight
                                                            : QBoxLayout::TopToBottom);
}

void StatsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!event->spontaneous()) {
        reload();
    }
}

void StatsDialog::showStats() {
    if (isVisible()) {
        reload();
    }
    show();
    raise();
    activateWindow();
}

void StatsDialog::onRecorded() {
    if (isVisible()) {
        refresh_->start();
    }
}

void StatsDialog::reload() {
    token_ = StatsRelay::nextToken();
    if (!summary_) {
        pages_->setCurrentIndex(0);
    }
    run(reads_.load, token_);
}

void StatsDialog::run(std::function<ds::StatsSummary()> work, std::uint64_t token) {
    io_.post([work = std::move(work), token] {
        StatsRelay& relay = StatsRelay::instance();
        stats_detail::Loaded loaded;
        QString error;
        try {
            if (!work) {
                throw std::runtime_error("Stats have no database");
            }
            loaded.summary = std::make_shared<const ds::StatsSummary>(work());
        } catch (const std::exception& failure) {
            kLog.error_with(failure, "Could not load dictation stats");
            error = q(std::string_view(failure.what()));
            if (error.isEmpty()) {
                error = QStringLiteral("Exception");
            }
        } catch (...) {
            kLog.error("Could not load dictation stats");
            error = QStringLiteral("Exception");
        }
        Q_EMIT relay.loaded(token, loaded, error);
    });
}

void StatsDialog::apply(std::uint64_t token, const std::optional<ds::StatsSummary>& summary,
                        const QString& error) {
    if (token != token_) {
        return;
    }
    if (!error.isEmpty() || !summary) {
        loadingLabel_->setText(QStringLiteral("Stats couldn't be counted right now."));
        pages_->setCurrentIndex(0);
        resetButton_->setEnabled(summary_ && summary_->entries != 0);
        return;
    }
    summary_ = summary;
    resetButton_->setEnabled(summary_->entries != 0);
    if (summary_->entries == 0) {
        pages_->setCurrentIndex(1);
        return;
    }
    render(*summary_);
    pages_->setCurrentIndex(2);
}

void StatsDialog::choosePeriod(int index) {
    periodKey_ = q(ds::PERIODS.at(static_cast<std::size_t>(index)).first);
    if (summary_ && summary_->entries != 0) {
        render(*summary_);
    }
}

void StatsDialog::render(const ds::StatsSummary& summary) {
    const ds::PeriodStats period = summary.period(qt::toUtf8(periodKey_));
    showHero(summary, period);
    showCards(summary, period);
    const std::vector<ChartBar> bars = showChart(period);
    showFeatures(summary, period, bars);
    showRhythm(period);
    showApps(period);
    showBreakdown(period);
    showActivity(summary);
}

void StatsDialog::showHero(const ds::StatsSummary& summary, const ds::PeriodStats& period) {
    heroCaption_->setText(QStringLiteral("Dictated · ") +
                          QString::fromUtf8(periodText(period.key).caption).toLower());
    heroValue_->setText(q(sv::words_text(period.words)));
    const auto [chip, tone] = sv::trend(period, summary.first_day);
    trendChip_->setText(q(chip));
    trendChip_->setVisible(!chip.empty());
    ui::setStyleProperty(trendChip_, ui::prop::tone, q(tone.empty() ? "flat" : tone));
    const std::int64_t total = summary.words_all_time;
    const std::int64_t goal = ds::next_milestone(total);
    milestoneLabel_->setText(q("Next milestone: " + thousands(goal) + " words"));
    milestoneLeft_->setText(q(thousands(goal - total) + " to go"));
    milestoneBar_->setFraction(static_cast<double>(total) / static_cast<double>(goal));
    milestoneBar_->setToolTip(
        q(thousands(total) + " of " + thousands(goal) + " words dictated, all time"));
}

void StatsDialog::showCards(const ds::StatsSummary& summary, const ds::PeriodStats& period) {
    const int typing = ds::TYPING_WPM;
    const QString savedTip =
        QStringLiteral(
            "Typing these words at %1 words a minute, less the time you spent saying them")
            .arg(typing);
    const double saved = period.minutes_saved();
    if (saved != 0.0) {
        savedCard_->setValues(q(sv::duration(saved)),
                              QStringLiteral("vs typing at %1 wpm").arg(typing), savedTip);
    } else {
        savedCard_->setValues(QStringLiteral("—"), QStringLiteral("Shows after a longer dictation"),
                              savedTip);
    }
    if (period.average_wpm != 0.0) {
        const double faster = period.average_wpm / typing;
        paceCard_->setValues(q(thousands(roundHalfEven(period.average_wpm)) + " wpm"),
                             faster >= 1.1 ? q(py::format_fixed(faster, 1) + "× faster than typing")
                                           : QStringLiteral("Your speaking pace"),
                             QStringLiteral("Words per minute of recording, pauses included"));
    } else {
        paceCard_->setValues(QStringLiteral("—"), QStringLiteral("Shows after a longer dictation"));
    }
    dictationsCard_->setValues(
        q(thousands(period.dictations)),
        period.dictations != 0
            ? q(thousands(roundHalfEven(static_cast<double>(period.words) /
                                        static_cast<double>(period.dictations))) +
                " words each")
            : QStringLiteral("None in this span"));
    streakCard_->setValues(
        q(sv::days(summary.day_streak)), q(sv::streak_detail(summary)),
        q("Days in a row with a dictation; your longest is " + sv::days(summary.longest_streak)));
}

std::vector<ChartBar> StatsDialog::showChart(const ds::PeriodStats& period) {
    std::vector<ChartBar> bars;
    QString unit;
    QString name;
    if (!period.days) {
        unit = QStringLiteral("month");
        bars = monthBars(period);
        name = QStringLiteral("Words per month, last %1 months").arg(bars.size());
    } else {
        unit = QStringLiteral("day");
        bars = dayBars(period);
        name = QStringLiteral("Words per day, last ") +
               q(sv::days(static_cast<std::int64_t>(bars.size())));
    }
    chartTitle_->setText(QStringLiteral("Words per ") + unit);
    double average = 0.0;
    if (!bars.empty()) {
        std::int64_t sum = 0;
        for (const ChartBar& bar : bars) {
            sum += bar.words;
        }
        average = static_cast<double>(sum) / static_cast<double>(bars.size());
    }
    chartAverage_->setText(average != 0.0 ? q("· avg " + thousands(roundHalfEven(average)))
                                          : QString());
    bars_->setBars(bars, name, static_cast<int>(bars.size()) - 1, average);
    // max(bars, key=words): the first of the busiest.
    const ChartBar* best = nullptr;
    for (const ChartBar& bar : bars) {
        if (best == nullptr || bar.words > best->words) {
            best = &bar;
        }
    }
    bestLabel_->setText(best != nullptr && best->words != 0
                            ? QStringLiteral("Best ") + unit + QStringLiteral(": ") + best->name +
                                  QStringLiteral(" · ") + q(sv::words_text(best->words))
                            : QStringLiteral("No words yet"));
    return bars;
}

void StatsDialog::showFeatures(const ds::StatsSummary& summary, const ds::PeriodStats& period,
                               const std::vector<ChartBar>& bars) {
    clearGrid(featuresGrid_);
    featureRows_.clear();
    const ds::PeriodStats ever = summary.period("all");
    for (const std::string_view key : ds::FEATURES) {
        if (ever.feature(key).count == 0) {
            continue;
        }
        const ds::FeatureStats feature = period.feature(key);
        auto [name, detail] = sv::feature_text(feature);
        if (feature.count == 0) {
            detail = "None in this span";
        }
        const int row = static_cast<int>(featureRows_.size());
        const QString keyText = q(key);
        auto* opener = new FeatureRow(keyText, q(name), q(detail));
        connect(opener, &FeatureRow::clicked, this,
                [this, keyText] { Q_EMIT featureRequested(keyText); });
        featuresGrid_->addWidget(opener, row, 0);
        const std::string trendKey(key);
        auto* trendChart =
            new BarChart(22, QStringLiteral("accent"), nullptr,
                         [trendKey](std::int64_t n) { return q(sv::trend_text(trendKey, n)); });
        trendChart->setFixedWidth(scaledRound(72));
        std::vector<ChartBar> columns;
        const std::size_t shown = std::min(bars.size(), feature.trend.size());
        for (std::size_t index = 0; index < shown; ++index) {
            columns.push_back({bars[index].name, feature.trend[index], QString()});
        }
        trendChart->setBars(columns, q(name) + QStringLiteral(" over this span"));
        featuresGrid_->addWidget(trendChart, row, 1, Qt::AlignVCenter);
        featureRows_.push_back({keyText, q(name), q(detail)});
    }
    featuresSection_->setVisible(!featureRows_.empty());
}

void StatsDialog::showRhythm(const ds::PeriodStats& period) {
    rhythm_->setGrid(period.grid);
    const std::optional<std::pair<int, int>> peak = rhythm_->peak();
    if (!peak) {
        hourLabel_->setText(QStringLiteral("Nothing dictated in this span yet"));
        return;
    }
    hourLabel_->setText(q(std::format("Most on {}s around {}", sv::weekday_name(peak->first),
                                      sv::hour_text(peak->second))));
}

void StatsDialog::showApps(const ds::PeriodStats& period) {
    clearGrid(appsGrid_);
    appRows_.clear();
    std::int64_t peak = 0;
    for (const auto& [name, words] : period.top_apps) {
        appRows_.emplace_back(q(name), words);
        peak = std::max(peak, words);
    }
    int row = 0;
    for (const auto& [name, words] : period.top_apps) {
        QLabel* app = label(q(name), "statsAppName");
        app->setMinimumWidth(50);
        appsGrid_->addWidget(app, row, 0);
        appsGrid_->addWidget(
            new ShareBar(peak != 0 ? static_cast<double>(words) / static_cast<double>(peak) : 0.0,
                         QStringLiteral("accent")),
            row, 1, Qt::AlignVCenter);
        QLabel* countLabel = plainLabel(q(thousands(words)), "statsAppWords");
        countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        appsGrid_->addWidget(countLabel, row, 2);
        const std::int64_t share = period.words != 0
                                       ? roundHalfEven(static_cast<double>(words * 100) /
                                                       static_cast<double>(period.words))
                                       : 0;
        QLabel* percent = plainLabel(QStringLiteral("%1%").arg(share), "statsAppShare");
        percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        percent->setToolTip(
            QStringLiteral("%1% of the words you dictated in this span").arg(share));
        appsGrid_->addWidget(percent, row, 3);
        ++row;
    }
    appsGrid_->setColumnMinimumWidth(0, scaledRound(70));
    appsGrid_->setColumnMinimumWidth(3, fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    noAppsLabel_->setVisible(period.top_apps.empty());
}

void StatsDialog::showBreakdown(const ds::PeriodStats& period) {
    const bool shown[] = {
        categoryBreakdown_->show(period.categories, categoryText),
        cleanupBreakdown_->show(period.cleanup_levels, cleanupText),
        // One language says nothing a share bar could show.
        languageBreakdown_->show(period.languages, languageText, 2),
    };
    const std::int64_t changed = period.words_cleaned_up;
    cleanupLabel_->setText(changed != 0 ? q("AI cleanup changed " + sv::words_text(changed))
                                        : QString());
    breakdownSection_->setVisible(shown[0] || shown[1] || shown[2] || changed != 0);
}

void StatsDialog::showActivity(const ds::StatsSummary& summary) {
    activity_->setDays(summary.activity);
    const auto active = std::count_if(summary.activity.begin(), summary.activity.end(),
                                      [](const auto& item) { return item.second != 0; });
    activeDaysLabel_->setText(
        QStringLiteral("%1 active %2 this past year")
            .arg(active)
            .arg(active == 1 ? QStringLiteral("day") : QStringLiteral("days")));
}

void StatsDialog::confirmReset() {
    const QMessageBox::StandardButton reply =
        askReset(this, QStringLiteral("Reset stats"),
                 QStringLiteral("Reset your stats?\n\nEvery count here starts over. History and "
                                "your meetings aren't changed."));
    if (reply != QMessageBox::Yes) {
        return;
    }
    token_ = StatsRelay::nextToken();
    resetButton_->setEnabled(false);
    run(
        [reads = reads_] {
            if (!reads.reset || !reads.load) {
                throw std::runtime_error("Stats have no database");
            }
            reads.reset();
            return reads.load();
        },
        token_);
}

} // namespace fv
