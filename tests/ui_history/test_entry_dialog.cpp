// The History entry viewer: rendering, the Original/AI toggle and the actions it requests
// (tests/test_history_entry_dialog.py).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>

#include "history_ui_support.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"

using fv::HistoryEntry;
using fv::HistoryEntryDialog;
using fvtest::FakeStore;
using fvtest::Io;
using fvtest::q;

namespace {

struct EntryFields {
    std::string text = "Fixed transcript text";
    std::optional<std::string> rawText;
    std::string model = "local_whisper (turbo | cuda (float16))";
    std::optional<std::string> audioFile;
    std::optional<double> audioDuration = 12.5;
    std::optional<double> transcriptionTime = 1.2;
    std::optional<std::int64_t> fileSize = 2048;
    std::optional<std::string> cleanupProvider;
    std::optional<std::string> cleanupModel;
};

// _make_entry.
HistoryEntry makeEntry(const EntryFields& fields) {
    HistoryEntry entry;
    entry.id = "entry-test-id";
    entry.text = fields.text;
    entry.raw_text = fields.rawText;
    entry.timestamp = "2026-07-17T14:30:00";
    entry.model = fields.model;
    entry.audio_file = fields.audioFile;
    entry.transcription_time = fields.transcriptionTime;
    entry.audio_duration = fields.audioDuration;
    entry.file_size = fields.fileSize;
    entry.cleanup_provider = fields.cleanupProvider;
    entry.cleanup_model = fields.cleanupModel;
    return entry;
}

struct Viewer {
    explicit Viewer(const HistoryEntry& entry, std::shared_ptr<FakeStore> fake = {})
        : store(fake ? std::move(fake) : std::make_shared<FakeStore>()),
          dialog(entry, store, io.executor()) {
        dialog.copyText = [this](const QString& value) {
            clipboard = value;
            return true;
        };
    }

    std::shared_ptr<FakeStore> store;
    Io io;
    HistoryEntryDialog dialog;
    QString clipboard;
};

} // namespace

// from tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_shows_full_transcript_text
TEST_CASE("shows full transcript text", "[test_history_entry_dialog][widget]") {
    EntryFields fields;
    fields.text = "Full transcript body";
    Viewer viewer(makeEntry(fields));

    CHECK(viewer.dialog.isModal());
    CHECK(viewer.dialog.transcriptText()->toPlainText() == QStringLiteral("Full transcript body"));
    CHECK(viewer.dialog.versionToggle()->isHidden());
    CHECK(viewer.dialog.copyRawButton()->isHidden());
}

// from tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_fixed_raw_toggle_swaps_text
TEST_CASE("fixed raw toggle swaps text", "[test_history_entry_dialog][widget]") {
    EntryFields fields;
    fields.text = "Cleaned version";
    fields.rawText = "Raw ASR version";
    fields.cleanupProvider = "openai";
    fields.cleanupModel = "gpt-4o-mini";
    Viewer viewer(makeEntry(fields));
    HistoryEntryDialog& dialog = viewer.dialog;

    CHECK_FALSE(dialog.versionToggle()->isHidden());
    CHECK_FALSE(dialog.copyRawButton()->isHidden());
    CHECK(dialog.transcriptText()->toPlainText() == QStringLiteral("Cleaned version"));

    dialog.rawButton()->click();
    CHECK(dialog.transcriptText()->toPlainText() == QStringLiteral("Raw ASR version"));

    dialog.fixedButton()->click();
    CHECK(dialog.transcriptText()->toPlainText() == QStringLiteral("Cleaned version"));
}

// from
// tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_copy_puts_shown_text_on_clipboard
TEST_CASE("copy puts shown text on clipboard", "[test_history_entry_dialog][widget]") {
    EntryFields fields;
    fields.text = "Fixed copy target";
    fields.rawText = "Raw copy target";
    Viewer viewer(makeEntry(fields));
    HistoryEntryDialog& dialog = viewer.dialog;
    int copied = 0;
    QObject::connect(&dialog, &HistoryEntryDialog::copied, [&copied] { ++copied; });

    dialog.copyButton()->click();
    CHECK(viewer.clipboard == QStringLiteral("Fixed copy target"));
    CHECK(copied == 1);

    dialog.rawButton()->click();
    dialog.copyButton()->click();
    CHECK(viewer.clipboard == QStringLiteral("Raw copy target"));
    CHECK(copied == 2);
}

// from
// tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_copy_raw_button_copies_raw_text
TEST_CASE("copy raw button copies raw text", "[test_history_entry_dialog][widget]") {
    EntryFields fields;
    fields.text = "Fixed";
    fields.rawText = "Only raw";
    Viewer viewer(makeEntry(fields));

    viewer.dialog.copyRawButton()->click();

    CHECK(viewer.clipboard == QStringLiteral("Only raw"));
}

// from
// tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_delete_emits_after_confirmation
TEST_CASE("delete emits after confirmation", "[test_history_entry_dialog][widget]") {
    Viewer viewer(makeEntry({}));
    HistoryEntryDialog& dialog = viewer.dialog;
    std::vector<QString> deleted;
    QObject::connect(&dialog, &HistoryEntryDialog::deleteRequested,
                     [&deleted](const QString& id) { deleted.push_back(id); });
    QString asked;
    dialog.runMessageBox = [&asked](QMessageBox& box) {
        asked = box.windowTitle() + QStringLiteral(": ") + box.text();
        return int(QMessageBox::Yes);
    };

    dialog.deleteButton()->click();

    CHECK(deleted == std::vector<QString>{QStringLiteral("entry-test-id")});
    CHECK(asked == QStringLiteral("Delete Entry: Delete this transcription from history?\n\nThe "
                                  "saved recording file (if any) will be kept."));
}

TEST_CASE("a delete the user declines is not requested", "[test_history_entry_dialog][widget]") {
    Viewer viewer(makeEntry({}));
    int deleted = 0;
    QObject::connect(&viewer.dialog, &HistoryEntryDialog::deleteRequested,
                     [&deleted](const QString&) { ++deleted; });
    viewer.dialog.runMessageBox = [](QMessageBox& box) {
        CHECK(box.defaultButton() == box.button(QMessageBox::No));
        return int(QMessageBox::No);
    };

    viewer.dialog.deleteButton()->click();

    CHECK(deleted == 0);
}

// from
// tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_retranscribe_visible_when_audio_exists
TEST_CASE("retranscribe visible when audio exists", "[test_history_entry_dialog][widget]") {
    const fvtest::FakeDevice device;
    auto store = std::make_shared<FakeStore>();
    store->recordings["clip.wav"] = std::filesystem::path(LR"(C:\recordings\clip.wav)");
    EntryFields fields;
    fields.audioFile = "clip.wav";
    Viewer viewer(makeEntry(fields), store);
    HistoryEntryDialog& dialog = viewer.dialog;

    CHECK_FALSE(dialog.retranscribeButton()->isHidden());
    std::vector<QString> requests;
    QObject::connect(&dialog, &HistoryEntryDialog::retranscribeRequested,
                     [&requests](const QString& path) { requests.push_back(path); });
    dialog.retranscribeButton()->click();
    CHECK(requests == std::vector<QString>{QStringLiteral(R"(C:\recordings\clip.wav)")});
    CHECK(dialog.retranscribeButton()->menu() == nullptr);
}

// from
// tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_failed_recording_can_be_retried_from_the_card_or_dialog
TEST_CASE("failed recording can be retried from the card or dialog",
          "[test_history_entry_dialog][widget]") {
    const fvtest::FakeDevice device;
    const fv::testkit::TempDataRoot tmp;
    const std::filesystem::path recording = tmp.path() / "clip.wav";
    fvtest::writeRecording(recording);
    auto store = std::make_shared<FakeStore>();
    store->recordings["clip.wav"] = recording;
    EntryFields fields;
    fields.text = "";
    fields.audioFile = "clip.wav";
    HistoryEntry entry = makeEntry(fields);
    entry.entry_kind = std::string(fv::FAILED_RECORDING_KIND);
    Viewer viewer(entry, store);
    HistoryEntryDialog& dialog = viewer.dialog;
    fv::HistoryItemWidget card(entry, *store);

    REQUIRE(card.kindChip() != nullptr);
    CHECK(card.kindChip()->text() == QStringLiteral("Transcription failed"));
    REQUIRE(card.retranscribeButton() != nullptr);
    CHECK(card.retranscribeButton()->text() == QStringLiteral("Retry transcription"));
    CHECK(dialog.retranscribeButton()->text() == QStringLiteral("Retry transcription"));
    CHECK(dialog.copyButton()->isHidden());
    CHECK(dialog.transcriptText()->toPlainText().contains(QStringLiteral("Audio saved")));
    std::vector<QString> requests;
    const auto record = [&requests](const QString& path) {
        requests.push_back(path);
    };
    QObject::connect(&dialog, &HistoryEntryDialog::retranscribeRequested, record);
    QObject::connect(&card, &fv::HistoryItemWidget::retranscribeRequested, record);
    card.retranscribeButton()->click();
    dialog.retranscribeButton()->click();
    const QString path = QString::fromStdWString(recording.wstring());
    CHECK(requests == std::vector<QString>{path, path});
}

// from
// tests/test_history_entry_dialog.py::TestHistoryEntryDialog::test_metadata_facts_render_when_present
TEST_CASE("metadata facts render when present", "[test_history_entry_dialog][widget]") {
    EntryFields fields;
    fields.audioDuration = 65.0;
    fields.transcriptionTime = 2.5;
    fields.fileSize = 1536;
    Viewer viewer(makeEntry(fields));
    const auto& facts = viewer.dialog.factLabels();

    CHECK(facts.at(QStringLiteral("Audio duration"))->text() == QStringLiteral("1m 5s"));
    CHECK(facts.at(QStringLiteral("Transcription time"))->text() == QStringLiteral("2.5s"));
    CHECK(facts.at(QStringLiteral("File size"))->text() == QStringLiteral("1.5 KB"));
}

TEST_CASE("an entry without facts has no details section", "[widget]") {
    EntryFields fields;
    fields.model = "";
    fields.audioDuration.reset();
    fields.transcriptionTime.reset();
    fields.fileSize.reset();
    Viewer viewer(makeEntry(fields));
    CHECK(viewer.dialog.factLabels().empty());
}

TEST_CASE("closing the dialog with nothing playing leaves the player alone", "[widget]") {
    Viewer viewer(makeEntry({}));
    CHECK(viewer.dialog.playback() != nullptr);
    CHECK(viewer.dialog.playButton()->isHidden());
    viewer.dialog.reject();
    CHECK_FALSE(viewer.dialog.playback()->isPlaying());
}

TEST_CASE("Closing History entry before host audio arrives cancels playback", "[widget]") {
    fvtest::FakeDevice device;
    const fv::testkit::TempDataRoot tmp;
    const auto recording = fvtest::writeRecording(tmp.path() / "remote.wav");
    auto entry = makeEntry({});
    entry.stored_on = "Synthetic host";
    entry.remote_audio = true;
    Viewer viewer(entry);
    int fetches = 0;
    viewer.dialog.playback()->setRemoteAudio(viewer.io.executor(), [&] {
        ++fetches;
        return recording;
    });
    viewer.dialog.show();
    viewer.dialog.playButton()->click();
    CHECK_FALSE(viewer.dialog.playButton()->isEnabled());
    // history_playback._set_idle while fetching.
    CHECK(viewer.dialog.playButton()->text() == QStringLiteral("Getting it…"));
    viewer.dialog.reject();
    viewer.io.drain();

    CHECK(fetches == 1);
    CHECK(viewer.dialog.playButton()->isEnabled());
    CHECK(viewer.dialog.playButton()->text() == QStringLiteral("Play"));
    CHECK(device.backend().streams().empty());
}

TEST_CASE("Deleting the History card before host audio arrives drops the playback reply",
          "[widget]") {
    fvtest::FakeDevice device;
    const fv::testkit::TempDataRoot tmp;
    const auto recording = fvtest::writeRecording(tmp.path() / "remote.wav");
    auto entry = makeEntry({});
    entry.stored_on = "Synthetic host";
    entry.remote_audio = true;
    FakeStore store;
    Io io;
    auto card = std::make_unique<fv::HistoryItemWidget>(entry, store);
    int fetches = 0;
    card->playback()->setRemoteAudio(io.executor(), [&] {
        ++fetches;
        return recording;
    });
    card->playback()->button()->click();
    card.reset();
    io.drain();

    CHECK(fetches == 1);
    CHECK(device.backend().streams().empty());
}

TEST_CASE("A host audio failure returns Play and names the failed download", "[widget]") {
    fvtest::FakeDevice device;
    auto entry = makeEntry({});
    entry.stored_on = "Synthetic host";
    entry.remote_audio = true;
    Viewer viewer(entry);
    viewer.dialog.playback()->setRemoteAudio(viewer.io.executor(), []() -> std::filesystem::path {
        throw std::runtime_error("host offline");
    });
    viewer.dialog.playButton()->click();
    viewer.io.drain();
    CHECK(viewer.dialog.playButton()->isEnabled());
    CHECK(viewer.dialog.playback()->note() ==
          QStringLiteral("Couldn't get the recording: host offline"));
    CHECK(device.backend().streams().empty());
}

TEST_CASE("A completed host audio fetch plays only in the open History viewer", "[widget]") {
    fvtest::FakeDevice device;
    const fv::testkit::TempDataRoot tmp;
    const auto recording = fvtest::writeRecording(tmp.path() / "remote.wav");
    auto entry = makeEntry({});
    entry.stored_on = "Synthetic host";
    entry.remote_audio = true;
    Viewer viewer(entry);
    viewer.dialog.playback()->setRemoteAudio(viewer.io.executor(),
                                             [recording] { return recording; });
    viewer.dialog.show();
    viewer.dialog.playButton()->click();
    CHECK(device.backend().streams().empty());
    viewer.io.drain();

    REQUIRE(fvtest::waitFor([&] {
        const auto streams = device.backend().streams();
        return !streams.empty() && streams.front()->started();
    }));
    CHECK(viewer.dialog.playback()->isPlaying());
    viewer.dialog.reject();
    CHECK_FALSE(viewer.dialog.playback()->isPlaying());
}
