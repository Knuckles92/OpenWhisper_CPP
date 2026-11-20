// A host-kept recording's download that outlives the card or dialog that asked for it
// (tests/test_personalize_fix2_g3_playback_fetch.py), and one download shared by playing and
// transcribing again (tests/test_personalize_fix3_h3_audio_fetch.py). The io executor is the
// test's: draining it is the host finishing the download.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>

#include "history_ui_support.h"
#include "host_records_support.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_sidebar.h"

using fv::HistoryEntry;
using fv::HistoryItemWidget;
using fv::HistorySidebar;
using fvtest::FakeStore;
using fvtest::Io;

namespace {

// _remote: a host-kept entry with a recording there.
HistoryEntry remoteEntry() {
    auto entry = fvtest::makeEntry("On the host.");
    entry.id = "h1";
    entry.timestamp = "2026-10-06T10:00:00+00:00";
    entry.stored_on = "devbox";
    entry.remote_audio = true;
    entry.file_size = 22050;
    return entry;
}

// The `slow_host` fixture: audio_for counts its calls, and keeps what it fetched, as
// RecordSync::audio_for does (one download per record, shared by its callers).
struct SlowHost {
    SlowHost() {
        fv::playback::forgetHostRecordings();
        audio = fvtest::writeRecording(folder.path() / "cached.wav", 0.5);
    }
    ~SlowHost() { fv::playback::forgetHostRecordings(); }

    std::function<std::filesystem::path()> fetch() {
        return [this] {
            ++calls;
            if (!error.empty()) {
                throw std::runtime_error(error);
            }
            return audio;
        };
    }

    fv::testkit::TempDataRoot folder;
    std::filesystem::path audio;
    int calls = 0;
    std::string error;
};

void pump() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

// _show: a History sidebar shown with these entries, as a load would apply them.
struct Shown {
    explicit Shown(SlowHost& host) : host(host), sidebar(store, io.executor(), nullptr) {
        sidebar.makeCard = [this](const HistoryEntry& entry) {
            auto* card = new HistoryItemWidget(entry, *store);
            card->setRemoteAudio(io.executor(), this->host.fetch());
            return card;
        };
        sidebar.resize(HistorySidebar::kExpandedWidth, 600);
        sidebar.setSidebarWidth(HistorySidebar::kExpandedWidth);
        sidebar.expand();
        sidebar.show();
        // The load the first expand starts finds nothing; the test shows its own entries.
        io.drain();
    }
    void show(std::vector<HistoryEntry> entries) {
        sidebar.applyHistoryResults(sidebar.loadGeneration(), QString(), std::move(entries),
                                    QString());
    }
    HistoryItemWidget* card() {
        return qobject_cast<HistoryItemWidget*>(sidebar.historyCards().at("h1").card);
    }

    SlowHost& host;
    std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
    Io io;
    HistorySidebar sidebar;
};

// A card of its own over the test's io.
std::unique_ptr<HistoryItemWidget> card(FakeStore& store, Io& io, SlowHost& host) {
    auto made = std::make_unique<HistoryItemWidget>(remoteEntry(), store);
    made->setRemoteAudio(io.executor(), host.fetch());
    return made;
}

} // namespace

TEST_CASE("test_a_refresh_mid_download_hands_the_click_to_the_rebuilt_card",
          "[test_personalize_fix2_g3_playback_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    Shown shown(host);
    shown.show({remoteEntry()});
    QPointer<HistoryItemWidget> clicked = shown.card();
    clicked->playback()->button()->click();
    CHECK(clicked->playback()->button()->text() == QStringLiteral("Getting it…"));

    // A host listing slower than the merge wait shows the local entries first, which deletes
    // the host-kept card; the merged list rebuilds it.
    shown.show({});
    pump();
    CHECK(clicked.isNull());
    shown.show({remoteEntry()});
    HistoryItemWidget* rebuilt = shown.card();
    CHECK(rebuilt->playback()->button()->text() == QStringLiteral("Getting it…"));
    CHECK_FALSE(rebuilt->playback()->button()->isEnabled());

    shown.io.drain();
    CHECK(fvtest::waitFor([&] { return device.backend().streams().size() == 1; }));
    CHECK(rebuilt->playback()->isPlaying());
    CHECK(rebuilt->playback()->button()->text() == QStringLiteral("Stop"));
    CHECK(host.calls == 1);
    rebuilt->playback()->stop();
}

TEST_CASE("test_a_card_deleted_mid_download_plays_nothing_but_keeps_the_file",
          "[test_personalize_fix2_g3_playback_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    FakeStore store;
    Io io;
    auto first = card(store, io, host);
    first->playback()->button()->click();
    first.reset();
    io.drain();
    CHECK(device.backend().streams().empty());

    auto later = card(store, io, host);
    CHECK(later->playback()->button()->text() == QStringLiteral("Play"));
    CHECK(later->playback()->button()->isEnabled());
    later->playback()->button()->click();
    CHECK(later->playback()->isPlaying());
    CHECK(fvtest::waitFor([&] { return device.backend().streams().size() == 1; }));
    CHECK(host.calls == 1);
    later->playback()->stop();
}

TEST_CASE("test_a_second_click_joins_the_download_and_plays_where_it_was_made",
          "[test_personalize_fix2_g3_playback_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    auto store = std::make_shared<FakeStore>();
    Io io;
    fv::HistoryEntryDialog dialog(remoteEntry(), store, io.executor());
    dialog.playback()->setRemoteAudio(io.executor(), host.fetch());
    auto other = card(*store, io, host);
    other->playback()->button()->click();
    dialog.playback()->button()->click();
    CHECK(dialog.playback()->button()->text() == QStringLiteral("Getting it…"));

    io.drain();
    CHECK(host.calls == 1);
    CHECK(dialog.playback()->isPlaying());
    CHECK_FALSE(other->playback()->isPlaying());
    CHECK(other->playback()->button()->text() == QStringLiteral("Play"));
    dialog.reject();
}

TEST_CASE("test_a_failed_download_is_reported_on_the_card_that_asked",
          "[test_personalize_fix2_g3_playback_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    host.error = "host offline";
    FakeStore store;
    Io io;
    auto asked = card(store, io, host);
    asked->playback()->button()->click();
    auto other = card(store, io, host);
    CHECK(other->playback()->button()->text() == QStringLiteral("Getting it…"));

    io.drain();
    CHECK(asked->playback()->note() == QStringLiteral("Couldn't get the recording: host offline"));
    CHECK(other->playback()->note().isEmpty());
    for (auto* each : {asked.get(), other.get()}) {
        CHECK(each->playback()->button()->text() == QStringLiteral("Play"));
        CHECK(each->playback()->button()->isEnabled());
    }
    // Nothing was kept: the next click asks the host again.
    host.error.clear();
    other->playback()->button()->click();
    io.drain();
    CHECK(host.calls == 2);
    CHECK(other->playback()->isPlaying());
    other->playback()->stop();
}

namespace {

// _click_then_rebuild: edited on the host while the download runs, one refresh rebuilds the
// card in place.
HistoryItemWidget* clickThenRebuild(Shown& shown) {
    shown.show({remoteEntry()});
    HistoryItemWidget* clicked = shown.card();
    clicked->playback()->button()->click();
    auto edited = remoteEntry();
    edited.text = "Edited on the host.";
    shown.show({edited});
    HistoryItemWidget* rebuilt = shown.card();
    REQUIRE(rebuilt != clicked);
    CHECK(rebuilt->playback()->button()->text() == QStringLiteral("Getting it…"));
    return rebuilt;
}

} // namespace

TEST_CASE("test_an_in_place_rebuild_mid_download_still_plays_the_click",
          "[test_personalize_fix2_g3_playback_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    Shown shown(host);
    HistoryItemWidget* rebuilt = clickThenRebuild(shown);
    shown.io.drain();
    CHECK(rebuilt->playback()->isPlaying());
    CHECK(rebuilt->playback()->button()->text() == QStringLiteral("Stop"));
    CHECK(host.calls == 1);
    rebuilt->playback()->stop();
}

TEST_CASE("test_stopping_before_a_rebuilt_cards_download_finishes_plays_nothing",
          "[test_personalize_fix2_g3_playback_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    Shown shown(host);
    HistoryItemWidget* rebuilt = clickThenRebuild(shown);
    fv::playback::stopAll();
    shown.io.drain();
    CHECK(device.backend().streams().empty());
    CHECK(rebuilt->playback()->button()->text() == QStringLiteral("Play"));
    CHECK(rebuilt->playback()->button()->isEnabled());
}

TEST_CASE("test_transcribe_again_during_play_waits_for_the_whole_recording",
          "[test_personalize_fix3_h3_audio_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    FakeStore store;
    Io io;
    auto shown = card(store, io, host);
    std::vector<std::uintmax_t> got;
    QObject::connect(shown.get(), &HistoryItemWidget::retranscribeRequested,
                     [&got](const QString& path) {
                         got.push_back(std::filesystem::file_size(path.toStdString()));
                     });
    shown->playback()->button()->click();
    shown->retranscribeButton()->click();
    pump();
    CHECK(got.empty());

    io.drain();
    CHECK(got == std::vector<std::uintmax_t>{std::filesystem::file_size(host.audio)});
    CHECK(shown->playback()->isPlaying());
    shown->playback()->stop();
}

TEST_CASE("test_play_during_transcribe_again_waits_for_the_whole_recording",
          "[test_personalize_fix3_h3_audio_fetch][widget]") {
    fvtest::FakeDevice device;
    SlowHost host;
    FakeStore store;
    Io io;
    auto shown = card(store, io, host);
    std::vector<std::uintmax_t> got;
    QObject::connect(shown.get(), &HistoryItemWidget::retranscribeRequested,
                     [&got](const QString& path) {
                         got.push_back(std::filesystem::file_size(path.toStdString()));
                     });
    shown->retranscribeButton()->click();
    shown->playback()->button()->click();
    pump();
    CHECK(device.backend().streams().empty());
    CHECK(shown->playback()->button()->text() == QStringLiteral("Getting it…"));

    io.drain();
    CHECK(got == std::vector<std::uintmax_t>{std::filesystem::file_size(host.audio)});
    CHECK(shown->playback()->isPlaying());
    shown->playback()->stop();
}
