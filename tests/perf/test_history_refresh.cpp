// The History list with 100 cards (OpenWhisper's performance-implementation-2026-10-03 "Unchanged
// 100-card history refresh", desktop.after_settings_and_history.history): the first fill, then
// a refresh whose entries are fresh copies of the same values, which must reuse every card. The
// sidebar is the real one, styled as the app styles it, over a store that answers from memory;
// results are handed to it as a finished load hands them (applyHistoryResults).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>

#include "base/testing/manual_executor.h"
#include "history/entry.h"
#include "support/perf_support.h"
#include "ui_history/history_item.h"
#include "ui_history/history_sidebar.h"
#include "ui_history/history_store.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace {

constexpr int kCards = 100;
constexpr int kRefreshes = 20;

// A store nobody asks during the measurement: the test applies the results itself.
class StillStore final : public fv::HistoryStore {
public:
    std::vector<fv::HistoryEntry> get_history(std::optional<std::int64_t>) override { return {}; }
    std::vector<fv::HistoryEntry> search(std::string_view, std::optional<std::int64_t>) override {
        return {};
    }
    std::optional<fv::HistoryEntry> get_entry(std::string_view) override { return std::nullopt; }
    std::optional<fv::HistoryEntry> last_dictation() override { return std::nullopt; }
    std::optional<fv::HistoryEntry> use_version(std::string_view, std::string_view) override {
        return std::nullopt;
    }
    bool delete_entry(std::string_view, bool) override { return false; }
    void clear_history() override {}
    void clear_history_and_recordings() override {}
    std::optional<std::filesystem::path> get_recording_path(std::string_view) override {
        return std::nullopt;
    }
    std::filesystem::path recordings_folder() const override { return {}; }
};

std::vector<fv::HistoryEntry> entries() {
    std::vector<fv::HistoryEntry> made;
    for (int index = 0; index < kCards; ++index) {
        fv::NewEntry fields;
        fields.text = "Dictation number " + std::to_string(index) +
                      ": the quarterly plan moves to Friday after the budget review.";
        fields.model = "parakeet";
        fields.source_name = "Quick Record";
        made.push_back(fv::create_entry(fields));
    }
    return made;
}

bool settleUntil(const std::function<bool()>& done, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return done();
}

} // namespace

TEST_CASE("history refresh of 100 unchanged cards", "[perf]") {
    fv::ui::ThemeManager theme(QStringLiteral("dark"));
    fv::ui::applyUiTheme(QStringLiteral("dark"), *qApp, theme);
    fv::ui::applyUiFontScale(100, *qApp, theme);

    auto store = std::make_shared<StillStore>();
    fv::ManualExecutor io;
    fv::HistorySidebar sidebar(store, io, nullptr);
    sidebar.resize(fv::HistorySidebar::kExpandedWidth, 900);
    sidebar.setSidebarWidth(fv::HistorySidebar::kExpandedWidth);
    sidebar.expand();
    sidebar.show();
    QApplication::processEvents();

    const std::vector<fv::HistoryEntry> list = entries();
    const auto apply = [&sidebar](std::vector<fv::HistoryEntry> shown) {
        sidebar.applyHistoryResults(sidebar.loadGeneration(), QString(), std::move(shown),
                                    QString());
    };

    // The first fill builds every card.
    const auto firstStarted = fvperf::Clock::now();
    apply(list);
    REQUIRE(settleUntil([&sidebar] { return sidebar.renderedCardCount() == kCards; }));
    const double firstMs = fvperf::msSince(firstStarted);

    std::map<std::string, fv::HistoryItemWidget*> firstCards;
    for (auto* card : sidebar.findChildren<fv::HistoryItemWidget*>()) {
        firstCards[card->entry().id] = card;
    }
    REQUIRE(firstCards.size() == static_cast<std::size_t>(kCards));

    // Unchanged refreshes: fresh copies of the same values, as a reload reads them again.
    std::vector<double> refresh;
    for (int run = 0; run < kRefreshes; ++run) {
        std::vector<fv::HistoryEntry> copies = list;
        const auto started = fvperf::Clock::now();
        apply(std::move(copies));
        QApplication::processEvents();
        refresh.push_back(fvperf::msSince(started));
        REQUIRE(sidebar.renderedCardCount() == kCards);
    }
    int reused = 0;
    for (auto* card : sidebar.findChildren<fv::HistoryItemWidget*>()) {
        const auto found = firstCards.find(card->entry().id);
        reused += found != firstCards.end() && found->second == card ? 1 : 0;
    }
    CHECK(reused == kCards);

    const std::string test = "perf.history refresh of 100 unchanged cards";
    const auto first = fvperf::record("history_first_100_cards_ms", "ms", {firstMs}, test,
                                      fv::Json{{"cards", kCards}});
    CHECK(fvperf::underCeiling(first, first.p50));
    const auto unchanged = fvperf::record(
        "history_refresh_100_ms", "ms", refresh, test,
        fv::Json{{"cards", kCards}, {"cards_reused", reused}, {"refreshes", kRefreshes}});
    CHECK(fvperf::underCeiling(unchanged, unchanged.p50));
    qApp->setStyleSheet(QString());
}
