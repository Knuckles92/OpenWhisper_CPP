// TemporaryClipboard's stage, restore, prefetch and pre-render, from OpenWhisper's
// tests/test_clipboard.py. Every clipboard here is a FakeClipboardBackend (the Python tests'
// FakeClipboard and SequencedClipboard), never the system one; "offscreen" tests also run the
// real offscreen QClipboard, which is local to this process.

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "appkit/qt_bridge.h"
#include "clipboard/render.h"
#include "clipboard/sequence.h"
#include "clipboard_support.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using fv::focus::RestoreOutcome;
using Probe = fv::clipboard::TemporaryClipboardProbe;
using namespace std::chrono_literals;

namespace {

QThread* main_thread() {
    return QCoreApplication::instance()->thread();
}

// _prefetched: a TemporaryClipboard over backend with a prefetch already taken.
std::unique_ptr<TemporaryClipboard> prefetched(std::unique_ptr<FakeClipboardBackend> owned,
                                               SequenceSource sequence = {}) {
    FakeClipboardBackend* backend = owned.get();
    if (!sequence) {
        sequence = counter_of(*backend);
    }
    auto temporary = temporary_over(std::move(owned), std::move(sequence));
    temporary->request_prefetch(0ms);
    // _wait_for_prefetch: two hops, a queued call and then a zero-delay timer.
    REQUIRE(wait_for([backend] { return !backend->read_threads.empty(); }));
    return temporary;
}

std::unique_ptr<FakeClipboardBackend> rich_backend(const QColor& color, int side) {
    auto mime = std::make_unique<QMimeData>();
    mime->setText("previous text");
    mime->setHtml("<b>previous text</b>");
    mime->setData("application/x-openwhisper-cpp-test", QByteArray("payload"));
    QImage image(side, side, QImage::Format_ARGB32);
    image.fill(color);
    mime->setImageData(image);
    return std::make_unique<FakeClipboardBackend>(std::move(mime));
}

} // namespace

// from tests/test_clipboard.py::test_blank_clipboard_leaves_transcript
TEST_CASE("blank clipboard leaves transcript", "[test_clipboard]") {
    const std::optional<QString> initial = GENERATE(
        std::optional<QString>(), std::optional<QString>(""), std::optional<QString>(" \n\t "));
    auto owned = std::make_unique<FakeClipboardBackend>();
    FakeClipboardBackend* clipboard = owned.get();
    if (initial) {
        clipboard->setText(*initial);
    }
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(stage.written);
    CHECK_FALSE(stage.lease);
    CHECK_FALSE(stage.restore_unavailable);
    CHECK(clipboard->text() == "dictated text");
    CHECK_FALSE(clipboard->current().hasFormat(kAutoPasteMarkerFormat));
}

// from tests/test_clipboard.py::test_nonblank_text_is_restored
TEST_CASE("nonblank text is restored", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(stage.written);
    REQUIRE(stage.lease);
    CHECK(clipboard->text() == "dictated text");
    CHECK(clipboard->current().hasFormat(kAutoPasteMarkerFormat));
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_stage_without_restore_leaves_transcript_and_keeps_no_lease
TEST_CASE("stage without restore leaves transcript and keeps no lease", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt, false);

    CHECK(stage.written);
    CHECK_FALSE(stage.lease);
    CHECK_FALSE(stage.restore_unavailable);
    CHECK(clipboard->text() == "dictated text");
    CHECK_FALSE(clipboard->current().hasFormat(kAutoPasteMarkerFormat));
}

// from tests/test_clipboard.py::test_rich_and_custom_formats_are_restored
TEST_CASE("rich and custom formats are restored", "[test_clipboard]") {
    auto mime = std::make_unique<QMimeData>();
    mime->setText("previous text");
    mime->setHtml("<b>previous text</b>");
    mime->setData("application/x-openwhisper-cpp-test", QByteArray("payload"));
    mime->setUrls({QUrl::fromLocalFile("/tmp/example.wav")});
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(QColor("red"));
    mime->setImageData(image);
    auto owned = std::make_unique<FakeClipboardBackend>(std::move(mime));
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    const auto outcome = temporary->restore_now(*stage.lease);
    const QMimeData& restored = clipboard->current();

    CHECK(outcome == RestoreOutcome::Restored);
    CHECK(restored.text() == "previous text");
    CHECK(restored.html() == "<b>previous text</b>");
    CHECK(restored.data("application/x-openwhisper-cpp-test") == QByteArray("payload"));
    REQUIRE_FALSE(restored.urls().isEmpty());
    CHECK(restored.urls()[0].toLocalFile() == "/tmp/example.wav");
    const QVariant restored_image = restored.imageData();
    REQUIRE(restored_image.metaType() == QMetaType::fromType<QImage>());
    CHECK(restored_image.value<QImage>().pixelColor(0, 0) == QColor("red"));
}

// from tests/test_clipboard.py::test_user_clipboard_change_is_not_overwritten
TEST_CASE("user clipboard change is not overwritten", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    clipboard->setText("new user copy");

    const auto outcome = temporary->restore_now(*stage.lease);

    CHECK(outcome == RestoreOutcome::Skipped);
    CHECK(clipboard->text() == "new user copy");
}

// from tests/test_clipboard.py::test_consecutive_stages_restore_the_original_clipboard
TEST_CASE("consecutive stages restore the original clipboard", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto first = temporary->stage_text("first transcript", std::nullopt);

    const auto second = temporary->stage_text("second transcript", std::nullopt);

    CHECK(first.lease);
    REQUIRE(second.lease);
    CHECK(clipboard->text() == "second transcript");
    temporary->restore_now(*second.lease);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_cleanup_restores_pending_clipboard
TEST_CASE("cleanup restores pending clipboard", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);

    temporary->cleanup();

    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_scheduled_restore_runs_after_event_loop_turn
TEST_CASE("scheduled restore runs after event loop turn", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);

    CHECK(temporary->schedule_restore(*stage.lease, 0ms));
    CHECK(clipboard->text() == "dictated text");
    pump(20);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_snapshot_failure_keeps_transcript_and_reports_unavailable
TEST_CASE("snapshot failure keeps transcript and reports unavailable", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->raise_on_read = true;
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(stage.written);
    CHECK_FALSE(stage.lease);
    CHECK(stage.restore_unavailable);
    CHECK(clipboard->text() == "dictated text");
}

// from tests/test_clipboard.py::test_clipboard_stage_write_failure_prevents_paste
TEST_CASE("clipboard stage write failure prevents paste", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->fail_set_mime_call = 1;
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK_FALSE(stage.written);
    CHECK_FALSE(stage.lease);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_restore_failure_emits_warning
TEST_CASE("restore failure emits warning", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->fail_set_mime_call = 2;
    auto temporary = temporary_over(std::move(owned));
    std::vector<QString> failures;
    QObject::connect(temporary.get(), &TemporaryClipboard::restoreFailed,
                     [&](const QString& message) { failures.push_back(message); });
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);

    const auto outcome = temporary->restore_now(*stage.lease);

    CHECK(outcome == RestoreOutcome::Failed);
    CHECK(failures == std::vector<QString>{"clipboard write failed"});
    CHECK(clipboard->text() == "dictated text");
}

// --- Snapshot prefetch ------------------------------------------------------

// from tests/test_clipboard.py::test_prefetched_snapshot_is_reused_when_clipboard_is_unchanged
TEST_CASE("prefetched snapshot is reused when clipboard is unchanged", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = prefetched(std::move(owned));
    CHECK(Probe::captures(*temporary) == 1);

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(Probe::captures(*temporary) == 1); // no second copy on the paste path
    CHECK(stage.written);
    REQUIRE(stage.lease);
    CHECK(clipboard->text() == "dictated text");
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_prefetch_is_recaptured_when_the_clipboard_changed
TEST_CASE("prefetch is recaptured when the clipboard changed", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = prefetched(std::move(owned));

    clipboard->setText("copied while dictating");
    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(Probe::captures(*temporary) == 2);
    REQUIRE(stage.lease);
    temporary->restore_now(*stage.lease);
    CHECK(clipboard->text() == "copied while dictating");
}

// from tests/test_clipboard.py::test_prefetch_is_recaptured_when_the_sequence_is_unknown
TEST_CASE("prefetch is recaptured when the sequence is unknown", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    int reads = 0;
    const std::optional<std::uint64_t> readings[] = {7, std::nullopt};
    auto temporary = prefetched(std::move(owned), [&]() -> std::optional<std::uint64_t> {
        const int index = reads++;
        return index < 2 ? readings[index] : std::nullopt;
    });

    clipboard->setText("copied while dictating");
    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(reads == 2);
    CHECK(Probe::captures(*temporary) == 2);
    REQUIRE(stage.lease);
    temporary->restore_now(*stage.lease);
    CHECK(clipboard->text() == "copied while dictating");
}

// from tests/test_clipboard.py::test_prefetch_restores_rich_content
TEST_CASE("prefetch restores rich content", "[test_clipboard]") {
    auto owned = rich_backend(QColor("blue"), 4);
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = prefetched(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    const auto outcome = temporary->restore_now(*stage.lease);
    const QMimeData& restored = clipboard->current();

    CHECK(Probe::captures(*temporary) == 1);
    CHECK(outcome == RestoreOutcome::Restored);
    CHECK(restored.text() == "previous text");
    CHECK(restored.html() == "<b>previous text</b>");
    CHECK(restored.data("application/x-openwhisper-cpp-test") == QByteArray("payload"));
    CHECK(restored.imageData().value<QImage>().pixelColor(0, 0) == QColor("blue"));
    CHECK_FALSE(restored.hasFormat(kAutoPasteMarkerFormat));
}

// from tests/test_clipboard.py::test_prefetch_of_a_blank_clipboard_leaves_the_transcript
TEST_CASE("prefetch of a blank clipboard leaves the transcript", "[test_clipboard]") {
    auto owned = text_backend("  ");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = prefetched(std::move(owned));

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(Probe::captures(*temporary) == 1);
    CHECK(stage.written);
    CHECK_FALSE(stage.lease);
    CHECK(clipboard->text() == "dictated text");
}

// from tests/test_clipboard.py::test_discarded_prefetch_is_not_used
TEST_CASE("discarded prefetch is not used", "[test_clipboard]") {
    auto temporary = prefetched(text_backend("previous text"));

    temporary->discard_prefetch();
    pump();
    temporary->stage_text("dictated text", std::nullopt);

    CHECK(Probe::captures(*temporary) == 2);
}

// from tests/test_clipboard.py::test_stage_without_restore_drops_the_prefetch
TEST_CASE("stage without restore drops the prefetch", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    // A counter that never moves would pass a stale prefetch off as current.
    auto temporary =
        prefetched(std::move(owned), []() -> std::optional<std::uint64_t> { return 1; });

    temporary->stage_text("dictated text", std::nullopt, false);
    const auto stage = temporary->stage_text("next transcript", std::nullopt);

    CHECK(Probe::captures(*temporary) == 2);
    REQUIRE(stage.lease);
    temporary->restore_now(*stage.lease);
    CHECK(clipboard->text() == "dictated text");
}

// from tests/test_clipboard.py::test_discard_cancels_a_prefetch_not_yet_taken
TEST_CASE("discard cancels a prefetch not yet taken", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));

    temporary->request_prefetch(20ms);
    temporary->discard_prefetch();
    pump(60);

    CHECK(Probe::captures(*temporary) == 0);
}

// from tests/test_clipboard.py::test_prefetch_is_consumed_by_one_stage
TEST_CASE("prefetch is consumed by one stage", "[test_clipboard]") {
    auto temporary = prefetched(text_backend("previous text"));
    const auto first = temporary->stage_text("first transcript", std::nullopt);
    REQUIRE(first.lease);
    temporary->restore_now(*first.lease);

    temporary->stage_text("second transcript", std::nullopt);

    CHECK(Probe::captures(*temporary) == 2);
}

// from tests/test_clipboard.py::test_new_request_drops_the_previous_recordings_prefetch
TEST_CASE("new request drops the previous recordings prefetch", "[test_clipboard]") {
    auto temporary = prefetched(text_backend("previous text"));

    // The next recording's capture is still waiting on its delay.
    temporary->request_prefetch(10'000ms);
    pump();
    temporary->stage_text("dictated text", std::nullopt);

    CHECK(Probe::captures(*temporary) == 2);
}

// from tests/test_clipboard.py::test_prefetch_waits_for_the_event_loop
TEST_CASE("prefetch waits for the event loop", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));

    temporary->request_prefetch(0ms);

    CHECK(Probe::captures(*temporary) == 0);
    REQUIRE(wait_for([clipboard] { return !clipboard->read_threads.empty(); }));
    CHECK(Probe::captures(*temporary) == 1);
}

// from tests/test_clipboard.py::test_prefetch_from_another_thread_reads_on_the_qt_thread
TEST_CASE("prefetch from another thread reads on the qt thread", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));

    std::thread worker([&] { temporary->request_prefetch(0ms); });
    worker.join();
    REQUIRE(wait_for([clipboard] { return !clipboard->read_threads.empty(); }));

    CHECK(Probe::captures(*temporary) == 1);
    CHECK(clipboard->read_threads == std::set<QThread*>{main_thread()});
}

// from tests/test_clipboard.py::test_prefetch_is_skipped_while_a_restore_is_pending
TEST_CASE("prefetch is skipped while a restore is pending", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));
    const auto first = temporary->stage_text("first transcript", std::nullopt);
    REQUIRE(first.lease);
    CHECK(temporary->schedule_restore(*first.lease, 10'000ms));

    temporary->request_prefetch(0ms);
    pump();
    CHECK(Probe::captures(*temporary) == 1); // only the first stage's own capture

    const auto second = temporary->stage_text("second transcript", std::nullopt);
    REQUIRE(second.lease);
    temporary->restore_now(*second.lease);

    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_restore_after_prefetch_still_respects_a_newer_user_copy
TEST_CASE("restore after prefetch still respects a newer user copy", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = prefetched(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);

    clipboard->setText("new user copy");

    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Skipped);
    CHECK(clipboard->text() == "new user copy");
}

// from tests/test_clipboard.py::test_prefetch_is_disabled_without_a_change_counter
TEST_CASE("prefetch is disabled without a change counter", "[test_clipboard]") {
    // system_clipboard_sequence() gives no counter offscreen; the app passes what it returns.
    auto temporary = temporary_over(text_backend("previous text"), system_clipboard_sequence());

    temporary->request_prefetch(0ms);
    pump();

    CHECK(Probe::captures(*temporary) == 0);
    temporary->stage_text("dictated text", std::nullopt);
    CHECK(Probe::captures(*temporary) == 1);
}

// from tests/test_clipboard.py::test_prefetch_failure_falls_back_to_capturing_at_stage
TEST_CASE("prefetch failure falls back to capturing at stage", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->raise_on_read = true;
    auto temporary = prefetched(std::move(owned));
    clipboard->raise_on_read = false;

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(Probe::captures(*temporary) == 1); // the failed read never reached a capture
    CHECK_FALSE(stage.restore_unavailable);
    REQUIRE(stage.lease);
    temporary->restore_now(*stage.lease);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_cleanup_drops_the_prefetch
TEST_CASE("cleanup drops the prefetch", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));
    temporary->request_prefetch(20ms);
    pump(5);

    temporary->cleanup();
    pump(60);

    CHECK(Probe::captures(*temporary) == 0);
}

// from tests/test_clipboard.py::test_offscreen_platform_has_no_system_change_counter
TEST_CASE("offscreen platform has no system change counter", "[test_clipboard]") {
    // The offscreen clipboard is in-process; the Windows counter never sees it change, so
    // trusting that counter would reuse stale snapshots.
    REQUIRE(QGuiApplication::platformName() == "offscreen");
    CHECK_FALSE(system_clipboard_sequence());
    CHECK_FALSE(system_text_renderer());
}

// --- Staged text handed to Windows before the paste -------------------------

namespace {

// The renders fixture: each pre-render records the clipboard text at that moment.
struct Renders {
    std::vector<QString> calls;
    FakeClipboardBackend* clipboard = nullptr;

    Renderer renderer() {
        return Renderer{[this] { calls.push_back(clipboard->text()); }, {}};
    }
};

} // namespace

// from tests/test_clipboard.py::test_staged_text_is_rendered_after_it_is_written
TEST_CASE("staged text is rendered after it is written", "[test_clipboard]") {
    Renders renders;
    auto owned = text_backend("previous text");
    renders.clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(stage.lease);
    CHECK(renders.calls == std::vector<QString>{"dictated text"});
}

// from tests/test_clipboard.py::test_blank_clipboard_stage_is_rendered
TEST_CASE("blank clipboard stage is rendered", "[test_clipboard]") {
    const std::optional<QString> initial =
        GENERATE(std::optional<QString>(), std::optional<QString>(" "));
    Renders renders;
    auto owned = std::make_unique<FakeClipboardBackend>();
    renders.clipboard = owned.get();
    if (initial) {
        owned->setText(*initial);
    }
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());

    temporary->stage_text("dictated text", std::nullopt);

    CHECK(renders.calls == std::vector<QString>{"dictated text"});
}

// from tests/test_clipboard.py::test_unsnapshotted_stage_is_rendered
TEST_CASE("unsnapshotted stage is rendered", "[test_clipboard]") {
    Renders renders;
    auto owned = text_backend("previous text");
    renders.clipboard = owned.get();
    owned->raise_on_read = true;
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(stage.restore_unavailable);
    CHECK(renders.calls == std::vector<QString>{"dictated text"});
}

// from tests/test_clipboard.py::test_stage_without_restore_is_rendered
TEST_CASE("stage without restore is rendered", "[test_clipboard]") {
    Renders renders;
    auto owned = text_backend("previous text");
    renders.clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());

    temporary->stage_text("dictated text", std::nullopt, false);

    CHECK(renders.calls == std::vector<QString>{"dictated text"});
}

// from tests/test_clipboard.py::test_failed_stage_is_not_rendered
TEST_CASE("failed stage is not rendered", "[test_clipboard]") {
    Renders renders;
    auto owned = text_backend("previous text");
    renders.clipboard = owned.get();
    owned->fail_set_mime_call = 1;
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK_FALSE(stage.written);
    CHECK(renders.calls.empty());
}

// from tests/test_clipboard.py::test_render_failure_does_not_fail_the_stage
TEST_CASE("render failure does not fail the stage", "[test_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(
        std::move(owned), {}, Renderer{[] { throw std::runtime_error("clipboard busy"); }, {}});

    const auto stage = temporary->stage_text("dictated text", std::nullopt);

    CHECK(stage.written);
    REQUIRE(stage.lease);
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "previous text");
}

// from tests/test_clipboard.py::test_restore_and_plain_copy_are_not_rendered
TEST_CASE("restore and plain copy are not rendered", "[test_clipboard]") {
    Renders renders;
    auto owned = text_backend("previous text");
    renders.clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    renders.calls.clear();

    temporary->restore_now(*stage.lease);
    temporary->write_text("history copy");

    CHECK(renders.calls.empty());
}

// --- The real offscreen QClipboard -----------------------------------------------------------

// The offscreen platform's clipboard is local to this process: these tests never touch the
// user's. Anywhere else they would, so they skip.
TEST_CASE("the offscreen QClipboard stages, restores and keeps rich formats",
          "[temporary_clipboard]") {
    if (QGuiApplication::platformName() != "offscreen") {
        SKIP("needs the offscreen platform's process-local clipboard");
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    auto original = std::make_unique<QMimeData>();
    original->setText("previous text");
    original->setHtml("<b>previous text</b>");
    original->setData("application/x-openwhisper-cpp-test", QByteArray("payload"));
    clipboard->setMimeData(original.release());
    TemporaryClipboard temporary(clipboard, {}, {}, nullptr, true);

    const auto stage = temporary.stage_text("dictated text", std::string("<i>dictated</i>"));
    REQUIRE(stage.written);
    REQUIRE(stage.lease);
    CHECK(clipboard->text() == "dictated text");
    CHECK(clipboard->mimeData()->hasFormat(kAutoPasteMarkerFormat));
    CHECK(clipboard->mimeData()->html().contains("<!--StartFragment--><i>dictated</i>"));
    CHECK(temporary.schedule_restore(*stage.lease, 0ms));
    REQUIRE(wait_for([&] { return clipboard->text() == "previous text"; }));
    CHECK(clipboard->mimeData()->html() == "<b>previous text</b>");
    CHECK(clipboard->mimeData()->data("application/x-openwhisper-cpp-test") ==
          QByteArray("payload"));
    CHECK_FALSE(clipboard->mimeData()->hasFormat(kAutoPasteMarkerFormat));
    clipboard->clear();
}

TEST_CASE("the marker format is named after the product", "[temporary_clipboard]") {
    CHECK(kAutoPasteMarkerFormat == "application/x-openwhisper-cpp-auto-paste");
    CHECK(kPrefetchDelay == 100ms);
    CHECK(kSelectionPoll == 10ms);
    CHECK(kSelectionTextPolls == 5);
}

TEST_CASE("restore_failed reaches port subscribers with the message", "[temporary_clipboard]") {
    auto owned = text_backend("previous text");
    owned->fail_set_mime_call = 2;
    auto temporary = temporary_over(std::move(owned));
    fv::qt::QtUiExecutor executor(temporary.get());
    std::vector<std::string> failures;
    auto subscription = temporary->restore_failed.connect(
        executor, [&](std::string message) { failures.push_back(std::move(message)); });
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);

    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Failed);

    CHECK(failures == std::vector<std::string>{"clipboard write failed"});
}

TEST_CASE("a throwing restore_failed subscriber neither escapes nor skips the outcome",
          "[temporary_clipboard]") {
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));
    fv::qt::QtUiExecutor executor(temporary.get());
    int failures = 0;
    auto subscription = temporary->restore_failed.connect(executor, [&](std::string) {
        ++failures;
        throw std::runtime_error("status line gone");
    });

    // A paste restore that fails still reports Failed.
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    clipboard->fail_writes = true;
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Failed);
    CHECK(failures == 1);

    // A selection whose original cannot be written back still answers its callback.
    clipboard->fail_writes = false;
    temporary->write_text("user text");
    std::vector<std::optional<std::string>> results;
    temporary->capture_selection(
        [&] {
            clipboard->copy_from_app("selected");
            clipboard->fail_writes = true;
        },
        [&](std::optional<std::string> text) { results.push_back(std::move(text)); }, 700ms);
    REQUIRE(wait_for([&] { return !results.empty(); }));
    CHECK(results == std::vector<std::optional<std::string>>{std::string("selected")});
    CHECK(failures == 2);
}

TEST_CASE("without a clipboard nothing is staged or captured", "[temporary_clipboard]") {
    auto temporary = temporary_over(nullptr);
    std::vector<std::optional<std::string>> results;

    CHECK_FALSE(temporary->stage_text("dictated", std::nullopt).written);
    temporary->capture_selection(
        [] {}, [&](std::optional<std::string> text) { results.push_back(std::move(text)); }, 700ms);

    CHECK(results == std::vector<std::optional<std::string>>{std::string()});
}
