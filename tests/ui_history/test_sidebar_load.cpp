// Loading the History list: one page, off the GUI thread, with one card per entry and a header
// counting the cards on screen (tests/test_history_scalability.py), reusing unchanged cards and
// outliving its own sidebar (tests/test_desktop_performance.py).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "base/executor.h"
#include "history/calendar_source.h"
#include "history_ui_support.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_sidebar.h"

using fv::HistoryEntry;
using fv::HistoryItemWidget;
using fv::HistorySidebar;
using fvtest::FakeStore;
using fvtest::Io;

namespace {

// _entries: TranscriptionHistory.create(text="<prefix> <i>", model="local_whisper",
// source_name="<prefix>-<i>.wav").
std::vector<HistoryEntry> entries(const std::string& prefix, int count) {
    std::vector<HistoryEntry> made;
    for (int index = 0; index < count; ++index) {
        HistoryEntry entry =
            fvtest::makeEntry(prefix + " " + std::to_string(index), "local_whisper");
        entry.source_name = prefix + "-" + std::to_string(index) + ".wav";
        made.push_back(std::move(entry));
    }
    return made;
}

std::vector<HistoryEntry> joined(std::vector<HistoryEntry> first,
                                 const std::vector<HistoryEntry>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// _expanded_sidebar: open at full width, shown, its list never loaded.
struct ExpandedSidebar {
    ExpandedSidebar() : sidebar(store, io.executor(), nullptr) {
        sidebar.resize(HistorySidebar::kExpandedWidth, 600);
        sidebar.setSidebarWidth(HistorySidebar::kExpandedWidth);
        // Opened as the window opens it; the load it starts is never run.
        sidebar.expand();
        sidebar.show();
        QCoreApplication::processEvents();
    }

    void apply(std::vector<HistoryEntry> list) {
        sidebar.applyHistoryResults(sidebar.loadGeneration(), QString(), std::move(list),
                                    QString());
    }

    std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
    Io io;
    HistorySidebar sidebar;
};

// _visible_cards.
std::vector<HistoryItemWidget*> visibleCards(const HistorySidebar& sidebar) {
    QWidget* content = sidebar.scrollArea()->widget();
    std::vector<HistoryItemWidget*> shown;
    for (HistoryItemWidget* card : content->findChildren<HistoryItemWidget*>()) {
        if (card->isVisibleTo(content)) {
            shown.push_back(card);
        }
    }
    return shown;
}

QWidget* cardFor(const HistorySidebar& sidebar, const std::string& id) {
    return sidebar.historyCards().at(id).card;
}

} // namespace

// from
// tests/test_history_scalability.py::test_history_sidebar_loads_only_one_page_off_the_ui_thread
TEST_CASE("history sidebar loads only one page off the ui thread",
          "[test_history_scalability][widget]") {
    fv::ThreadPool pool("history-test", 1);
    fv::Strand io(pool, "io", 1, fv::ShutdownMode::CancelQueued);
    auto store = std::make_shared<FakeStore>();
    std::mutex mutex;
    std::optional<std::int64_t> limit;
    std::thread::id thread;
    std::atomic<bool> called{false};
    store->onGetHistory = [&](std::optional<std::int64_t> asked) {
        {
            const std::lock_guard lock(mutex);
            limit = asked;
            thread = std::this_thread::get_id();
        }
        called = true;
        return std::vector<HistoryEntry>{};
    };
    HistorySidebar sidebar(store, io, nullptr);

    sidebar.loadHistory();
    REQUIRE(fvtest::waitFor([&] { return called.load(); }));
    // The placeholder, then the "No history yet" that replaces it.
    REQUIRE(fvtest::waitFor([&] {
        const QLayoutItem* item = sidebar.historyListLayout()->itemAt(0);
        const auto* label = item != nullptr ? qobject_cast<QLabel*>(item->widget()) : nullptr;
        return label != nullptr && label->text() == QStringLiteral("No history yet");
    }));

    const std::lock_guard lock(mutex);
    CHECK(limit == HistorySidebar::kMaxHistoryItems + 1);
    CHECK(thread != std::this_thread::get_id());
}

// from tests/test_history_scalability.py::test_large_history_does_not_stretch_section_header
TEST_CASE("large history does not stretch section header", "[test_history_scalability][widget]") {
    ExpandedSidebar open;
    std::vector<HistoryEntry> list;
    for (int index = 0; index < HistorySidebar::kMaxHistoryItems + 1; ++index) {
        HistoryEntry entry;
        entry.id = "entry-" + std::to_string(index);
        entry.text = "A transcript preview with enough words to wrap onto another line.";
        entry.raw_text = "Raw transcript";
        entry.timestamp = "2026-09-02T18:54:00";
        entry.model = "local_whisper (turbo | cuda (float16))";
        entry.cleanup_provider = "openrouter";
        entry.cleanup_model = "google/gemini-3.7-flash";
        char name[32];
        std::snprintf(name, sizeof name, "recording_%03d.wav", index);
        entry.source_name = name;
        list.push_back(std::move(entry));
    }
    open.apply({list.begin(), list.begin() + 3});
    QCoreApplication::processEvents();
    const int headerHeight = open.sidebar.historyHeader()->height();

    open.apply(list);
    QCoreApplication::processEvents();

    CHECK(open.sidebar.historyHeader()->height() == headerHeight);
    CHECK(open.sidebar.historyHeader()->text() == QStringLiteral("100+ entries"));
    const QWidget* first = open.sidebar.historyListLayout()->itemAt(0)->widget();
    CHECK(first->y() - open.sidebar.historyHeader()->geometry().bottom() <= 13);
}

// from
// tests/test_history_scalability.py::test_history_header_counts_the_cards_after_a_batch_lands_on_three_uploads
TEST_CASE("history header counts the cards after a batch lands on three uploads",
          "[test_history_scalability][widget]") {
    ExpandedSidebar open;
    const std::vector<HistoryEntry> singles = entries("single", 3);
    open.apply(singles);
    QCoreApplication::processEvents();
    CHECK(open.sidebar.historyHeader()->text() == QStringLiteral("3 entries"));

    const std::vector<HistoryEntry> batch = entries("batch", 20);
    open.apply(joined(batch, singles));
    QCoreApplication::processEvents();

    CHECK(open.sidebar.renderedCardCount() == 23);
    CHECK(visibleCards(open.sidebar).size() == 23);
    CHECK(open.sidebar.historyHeader()->text() == QStringLiteral("23 entries"));
}

// from
// tests/test_history_scalability.py::test_history_header_matches_the_cards_when_a_card_fails_to_build
TEST_CASE("history header matches the cards when a card fails to build",
          "[test_history_scalability][widget]") {
    ExpandedSidebar open;
    const std::vector<HistoryEntry> singles = entries("single", 3);
    open.apply(singles);
    const std::vector<HistoryEntry> batch = entries("batch", 20);
    const std::string failing = batch[5].id;
    FakeStore& store = *open.store;
    open.sidebar.makeCard = [&store, failing](const HistoryEntry& entry) -> HistoryItemWidget* {
        if (entry.id == failing) {
            throw std::runtime_error("card could not be built");
        }
        return new HistoryItemWidget(entry, store);
    };

    CHECK_THROWS_AS(open.apply(joined(batch, singles)), std::runtime_error);
    QCoreApplication::processEvents();

    const auto shown = static_cast<int>(visibleCards(open.sidebar).size());
    CHECK(open.sidebar.renderedCardCount() == shown);
    CHECK(open.sidebar.historyHeader()->text() == QStringLiteral("%1 entries").arg(shown));
}

// from tests/test_history_scalability.py::test_history_shows_one_card_per_entry_id
TEST_CASE("history shows one card per entry id", "[test_history_scalability][widget]") {
    ExpandedSidebar open;
    const std::vector<HistoryEntry> list = entries("entry", 4);
    open.apply({list[0], list[1], list[2], list[3], list[1], list[2]});
    QCoreApplication::processEvents();
    open.apply(list);
    QCoreApplication::processEvents();

    CHECK(visibleCards(open.sidebar).size() == 4);
    CHECK(open.sidebar.historyHeader()->text() == QStringLiteral("4 entries"));
}

// from tests/test_history_scalability.py::test_cleared_history_cards_leave_the_list_at_once
TEST_CASE("cleared history cards leave the list at once", "[test_history_scalability][widget]") {
    ExpandedSidebar open;
    open.apply(entries("entry", 3));
    QCoreApplication::processEvents();
    // Held, as a playing card's player can be, so they outlive the clear.
    std::vector<QPointer<HistoryItemWidget>> cards;
    for (HistoryItemWidget* card : visibleCards(open.sidebar)) {
        cards.emplace_back(card);
    }
    REQUIRE(cards.size() == 3);

    open.apply({});

    // Before the event loop runs the deferred deletes.
    CHECK(visibleCards(open.sidebar).empty());
    for (const QPointer<HistoryItemWidget>& card : cards) {
        REQUIRE(card != nullptr);
        CHECK_FALSE(card->isVisible());
    }
    // The panel title says History; with no cards there is nothing to count.
    CHECK_FALSE(open.sidebar.historyHeader()->isVisibleTo(&open.sidebar));
    QCoreApplication::processEvents();
}

// from
// tests/test_desktop_performance.py::test_history_refresh_reuses_unchanged_cards_and_updates_one_changed_entry
TEST_CASE("history refresh reuses unchanged cards and updates one changed entry",
          "[test_desktop_performance][widget]") {
    auto store = std::make_shared<FakeStore>();
    Io io;
    HistorySidebar sidebar(store, io.executor(), nullptr);
    std::vector<HistoryEntry> list;
    for (int index = 0; index < 3; ++index) {
        list.push_back(fvtest::makeEntry("Entry " + std::to_string(index), "local_whisper"));
    }
    const auto apply = [&sidebar](std::vector<HistoryEntry> shown) {
        sidebar.applyHistoryResults(sidebar.loadGeneration(), QString(), std::move(shown),
                                    QString());
    };
    apply(list);
    std::vector<QWidget*> first;
    for (const HistoryEntry& entry : list) {
        first.push_back(cardFor(sidebar, entry.id));
    }
    std::vector<HistoryEntry> clones = list;
    apply(clones);
    for (std::size_t index = 0; index < list.size(); ++index) {
        CHECK(cardFor(sidebar, list[index].id) == first[index]);
    }

    HistoryEntry changed = clones[1];
    changed.text = "Corrected transcription";
    apply({list[0], changed, list[2]});

    CHECK(cardFor(sidebar, list[0].id) == first[0]);
    CHECK(cardFor(sidebar, list[1].id) != first[1]);
    CHECK(cardFor(sidebar, list[2].id) == first[2]);
    CHECK(sidebar.historyListLayout()->count() == 3);
}

// from
// tests/test_desktop_performance.py::test_delayed_history_query_can_finish_after_qt_owner_is_deleted
TEST_CASE("delayed history query can finish after qt owner is deleted",
          "[test_desktop_performance][widget]") {
    fv::ThreadPool pool("history-test", 1);
    fv::Strand io(pool, "io", 1, fv::ShutdownMode::CancelQueued);
    auto store = std::make_shared<FakeStore>();
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::atomic<bool> finished{false};
    store->onGetHistory = [&](std::optional<std::int64_t>) {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        // Catch2 asserts on the test's thread only: the wait's outcome is read there.
        finished = changed.wait_for(lock, std::chrono::seconds(2), [&] { return released; });
        return std::vector<HistoryEntry>{};
    };
    auto* sidebar = new HistorySidebar(store, io, nullptr);
    sidebar->loadHistory();
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(1), [&] { return entered; }));
    }

    delete sidebar;
    {
        const std::lock_guard lock(mutex);
        released = true;
    }
    changed.notify_all();

    REQUIRE(fvtest::waitFor([&] { return finished.load(); }));
    io.shutdown(std::chrono::seconds(2));
    QCoreApplication::processEvents();
}

TEST_CASE("a real History is listed newest first, and search narrows it", "[widget]") {
    fvtest::LocalHistory history;
    fv::AddEntry older;
    older.text = "older note about apples";
    older.model = "base";
    const HistoryEntry first = history.add(older);
    fv::AddEntry newer;
    newer.text = "newer note about pears";
    newer.model = "base";
    const HistoryEntry second = history.add(newer);
    Io io;
    HistorySidebar sidebar(history.store(), io.executor(), nullptr);
    sidebar.resize(HistorySidebar::kExpandedWidth, 600);

    sidebar.expand();
    io.drain();

    REQUIRE(sidebar.renderedCardCount() == 2);
    const auto* top =
        qobject_cast<HistoryItemWidget*>(sidebar.historyListLayout()->itemAt(0)->widget());
    REQUIRE(top != nullptr);
    CHECK(top->entry().id == second.id);
    CHECK(sidebar.historyHeader()->text() == QStringLiteral("2 entries"));

    sidebar.searchInput()->setText(QStringLiteral("  APPLES "));
    REQUIRE(fvtest::waitFor([&] {
        io.drain();
        return sidebar.renderedCardCount() == 1;
    }));
    const auto* found =
        qobject_cast<HistoryItemWidget*>(sidebar.historyListLayout()->itemAt(0)->widget());
    REQUIRE(found != nullptr);
    CHECK(found->entry().id == first.id);
    CHECK(sidebar.historyHeader()->text() == QStringLiteral("1 entry"));

    sidebar.searchInput()->setText(QStringLiteral("nothing like this"));
    REQUIRE(fvtest::waitFor([&] {
        io.drain();
        return sidebar.renderedCardCount() == 0;
    }));
    const auto* message = qobject_cast<QLabel*>(sidebar.historyListLayout()->itemAt(0)->widget());
    REQUIRE(message != nullptr);
    CHECK(message->text() == QStringLiteral("No matching entries"));
}

TEST_CASE("a load that fails says History could not be loaded", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    store->onGetHistory = [](std::optional<std::int64_t>) -> std::vector<HistoryEntry> {
        throw std::runtime_error("database is locked");
    };
    Io io;
    HistorySidebar sidebar(store, io.executor(), nullptr);

    sidebar.expand();
    io.drain();

    const auto* message = qobject_cast<QLabel*>(sidebar.historyListLayout()->itemAt(0)->widget());
    REQUIRE(message != nullptr);
    CHECK(message->text() == QStringLiteral("History could not be loaded"));
    CHECK(sidebar.historyHeader()->isHidden());
}

TEST_CASE("a newer load wins over an older one still running", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    store->history = entries("entry", 2);
    Io io;
    HistorySidebar sidebar(store, io.executor(), nullptr);
    sidebar.expand();
    const std::uint64_t first = sidebar.loadGeneration();
    sidebar.loadHistory();
    CHECK(sidebar.loadGeneration() > first);
    // The older result arrives after the newer request: it is dropped.
    sidebar.applyHistoryResults(first, QString(), entries("stale", 5), QString());
    CHECK(sidebar.renderedCardCount() == 0);
    io.drain();
    CHECK(sidebar.renderedCardCount() == 2);
}

TEST_CASE("typing drops the load in flight at once and searches after a pause", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    store->history = entries("entry", 2);
    Io io;
    HistorySidebar sidebar(store, io.executor(), nullptr);
    sidebar.expand();
    const std::uint64_t inFlight = sidebar.loadGeneration();
    REQUIRE(io.executor().queued() == 1);

    sidebar.searchInput()->setText(QStringLiteral("entry"));

    // The result of the load already running is out of date before the search starts.
    CHECK(sidebar.loadGeneration() > inFlight);
    sidebar.applyHistoryResults(inFlight, QString(), entries("stale", 3), QString());
    CHECK(sidebar.renderedCardCount() == 0);
    // The search waits for the user to stop typing.
    CHECK(io.executor().queued() == 1);
    REQUIRE(fvtest::waitFor([&io] { return io.executor().queued() == 2; }));
    io.drain();
    CHECK(sidebar.renderedCardCount() == 2);
}

TEST_CASE("more than a page says how to find older entries", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    store->history = entries("entry", HistorySidebar::kMaxHistoryItems + 1);
    Io io;
    HistorySidebar sidebar(store, io.executor(), nullptr);
    sidebar.expand();
    io.drain();

    CHECK(sidebar.renderedCardCount() == HistorySidebar::kMaxHistoryItems);
    const QVBoxLayout* list = sidebar.historyListLayout();
    const auto* note = qobject_cast<QLabel*>(list->itemAt(list->count() - 1)->widget());
    REQUIRE(note != nullptr);
    CHECK(note->text() ==
          QStringLiteral("Showing the first 100 matches — refine your search to find older "
                         "entries"));
}

namespace {

class PairedHistorySource final : public fv::history::RemoteRecordSource {
public:
    fv::HistoryEntry hostEntry;
    std::string localCopy;
    std::string requestedQuery;

    bool listing_wanted(std::string_view kind) override { return kind == "dictation"; }
    std::vector<fv::Json> list_remote(std::string_view, std::string_view query, int) override {
        requestedQuery = query;
        return {fv::Json::object()};
    }
    std::vector<std::string> copies_on_host(std::string_view,
                                            const std::vector<std::string>&) override {
        return {localCopy};
    }
    std::string host_name() override { return "Synthetic host"; }
    fv::HistoryEntry remote_history_entry(const fv::Json&) override { return hostEntry; }
};

} // namespace

TEST_CASE("History merges host-kept rows and annotates local copies", "[widget]") {
    PairedHistorySource remote;
    remote.hostEntry = fvtest::makeEntry("host transcript", "parakeet");
    remote.hostEntry.timestamp = "2026-11-17T08:00:00Z";
    remote.hostEntry.stored_on = "Synthetic host";
    auto local = fvtest::makeEntry("local transcript", "parakeet");
    local.timestamp = "2026-11-16T08:00:00Z";
    remote.localCopy = local.id;

    ExpandedSidebar harness;
    harness.store->history = {local};
    harness.sidebar.setRemoteRecords(&remote, nullptr);
    harness.sidebar.setRemoteMergeWait(std::chrono::milliseconds(1));
    harness.sidebar.loadHistory();
    harness.io.drain();

    REQUIRE(harness.sidebar.renderedCardCount() == 2);
    const auto cards = visibleCards(harness.sidebar);
    REQUIRE(cards.size() == 2);
    CHECK(cards.front()->entry().id == remote.hostEntry.id);
    CHECK(cards.back()->entry().also_on == "Synthetic host");

    harness.store->history.clear();
    harness.sidebar.searchInput()->setText(QStringLiteral("host"));
    harness.sidebar.loadHistory();
    harness.io.drain();
    CHECK(harness.sidebar.renderedCardCount() == 1);
    CHECK(harness.sidebar.historyCards().contains(remote.hostEntry.id));
    // The host searches its own (records_list carries the query).
    CHECK(remote.requestedQuery == "host");
}

TEST_CASE("Collapsing History before host audio arrives cancels playback", "[widget]") {
    fvtest::FakeDevice device;
    const fv::testkit::TempDataRoot tmp;
    const auto recording = fvtest::writeRecording(tmp.path() / "remote.wav");
    auto entry = fvtest::makeEntry("Remote dictation");
    entry.stored_on = "Synthetic host";
    entry.remote_audio = true;
    ExpandedSidebar harness;
    harness.io.drain();
    harness.apply({entry});
    QPointer<HistoryItemWidget> card =
        qobject_cast<HistoryItemWidget*>(cardFor(harness.sidebar, entry.id));
    REQUIRE(card);
    card->playback()->setRemoteAudio(harness.io.executor(), [recording] { return recording; });
    card->playback()->button()->click();
    CHECK_FALSE(card->playback()->button()->isEnabled());
    harness.sidebar.collapse();
    harness.io.drain();

    REQUIRE(card);
    CHECK(card->playback()->button()->isEnabled());
    CHECK(device.backend().streams().empty());
}
