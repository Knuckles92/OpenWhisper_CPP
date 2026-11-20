// History entries a paired host keeps: viewed but not changed here, and their recording fetched
// from the host before it plays (tests/test_personalize_s9_history_ui.py). The entries are built
// as RecordSync::remote_history_entry builds them from the host's listing; that conversion is
// fv_remote_records' and tested there.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

#include <QPushButton>
#include <QTextEdit>

#include "audio/player.h"
#include "history_ui_support.h"
#include "testkit/testkit.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_widgets/buttons.h"

using fv::HistoryEntry;

namespace {

// remote_history_entry({"id": "h1", "text": ..., "timestamp": "2026-10-06T10:00:00+00:00",
// "model": "base", "stored_on": "devbox", ...}).
HistoryEntry hostEntry(std::string text) {
    HistoryEntry entry;
    entry.id = "h1";
    entry.text = std::move(text);
    entry.timestamp = "2026-10-06T10:00:00+00:00";
    entry.model = "base";
    entry.stored_on = "devbox";
    return entry;
}

} // namespace

TEST_CASE("a host kept entry can be viewed but not changed",
          "[test_personalize_s9_history_ui][widget]") {
    // from
    // tests/test_personalize_s9_history_ui.py::test_a_host_kept_entry_can_be_viewed_but_not_changed
    auto entry = hostEntry("Cleaned.");
    entry.raw_text = "cleaned";
    fvtest::Io io;
    fv::HistoryEntryDialog dialog(entry, std::make_shared<fvtest::FakeStore>(), io.executor());
    dialog.rawButton()->click();
    CHECK(dialog.transcriptText()->toPlainText() == QStringLiteral("cleaned"));
    CHECK(dialog.useVersionButton()->isHidden());
    CHECK(dialog.playButton()->isHidden());
}

TEST_CASE("a host kept recording is fetched before it plays",
          "[test_personalize_s9_history_ui][widget]") {
    // from
    // tests/test_personalize_s9_history_ui.py::test_a_host_kept_recording_is_fetched_before_it_plays
    fvtest::FakeDevice device;
    const fv::testkit::TempDataRoot tmp;
    const auto cached = fvtest::writeRecording(tmp.path() / "cached.wav");
    auto entry = hostEntry("On the host.");
    entry.remote_audio = true;
    entry.file_size = 22050;
    fvtest::FakeStore store;
    fvtest::Io io;
    fv::HistoryItemWidget card(entry, store);
    REQUIRE(card.playback() != nullptr);
    int fetches = 0;
    card.playback()->setRemoteAudio(io.executor(), [&] {
        ++fetches;
        return cached;
    });
    card.playback()->button()->click();
    // Nothing plays until the host's copy has arrived.
    CHECK(device.backend().streams().empty());
    io.drain();

    REQUIRE(fvtest::waitFor([&] {
        const auto streams = device.backend().streams();
        return !streams.empty() && streams.front()->started();
    }));
    CHECK(fetches == 1);
    CHECK(card.playback()->isPlaying());
    CHECK(fv::audio::player()->path() == cached);
    card.playback()->stop();
}
