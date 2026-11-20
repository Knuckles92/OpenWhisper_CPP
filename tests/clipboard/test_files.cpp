// Files on the clipboard: a walkthrough's PDF with its Markdown as text, from OpenWhisper's
// tests/test_clipboard_files.py. Every clipboard here is a FakeClipboardBackend, never the
// system one.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QMimeData>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>

#include "base/utf8.h"
#include "clipboard_support.h"
#include "testkit/testkit.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using fv::focus::RestoreOutcome;
using Probe = fv::clipboard::TemporaryClipboardProbe;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// _files: every URL is a local file; their paths.
std::vector<fs::path> files(const QMimeData& mime) {
    std::vector<fs::path> paths;
    for (const QUrl& url : mime.urls()) {
        REQUIRE(url.isLocalFile());
        paths.push_back(fv::path_from_utf8(url.toLocalFile().toStdString()).make_preferred());
    }
    return paths;
}

std::vector<fs::path> preferred(std::vector<fs::path> paths) {
    for (auto& path : paths) {
        path.make_preferred();
    }
    return paths;
}

} // namespace

// from tests/test_clipboard_files.py::test_files_and_their_text_go_on_together
TEST_CASE("files and their text go on together", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path pdf = tmp.path() / "walkthrough.pdf";
    const fs::path markdown = tmp.path() / "walkthrough.md";
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    CHECK(temporary->writeFiles({pdf, markdown}, "# Walkthrough\n"));

    const QMimeData& mime = clipboard->current();
    CHECK(files(mime) == preferred({pdf, markdown}));
    CHECK(mime.text() == "# Walkthrough\n");
    CHECK_FALSE(mime.hasImage());
}

// from tests/test_clipboard_files.py::test_text_only_has_no_file_format
TEST_CASE("text only has no file format", "[test_clipboard_files]") {
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    CHECK(temporary->writeFiles({}, "Agent instructions"));

    const QMimeData& mime = clipboard->current();
    CHECK(mime.text() == "Agent instructions");
    CHECK_FALSE(mime.hasUrls());
    CHECK_FALSE(mime.formats().contains("text/uri-list"));
}

// from tests/test_clipboard_files.py::test_without_text_only_the_files_go_on
TEST_CASE("without text only the files go on", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    CHECK(temporary->writeFiles({tmp.path() / "walkthrough.pdf"}));

    const QMimeData& mime = clipboard->current();
    CHECK(files(mime) == preferred({tmp.path() / "walkthrough.pdf"}));
    CHECK_FALSE(mime.formats().contains("text/plain"));
}

// from tests/test_clipboard_files.py::test_an_image_goes_on_beside_the_file_and_its_path
TEST_CASE("an image goes on beside the file and its path", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path shot = tmp.path() / "2026-10-21_14-32-05.png";
    const QString shot_text = QString::fromStdString(fv::path_to_utf8(shot));
    QImage image(4, 3, QImage::Format_RGB32);
    image.fill(QColor("#ff4f3a"));
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    CHECK(temporary->writeFiles({shot}, shot_text, image));

    const QMimeData& mime = clipboard->current();
    const QVariant pasted = mime.imageData();
    CHECK(files(mime) == preferred({shot}));
    CHECK(mime.text() == shot_text);
    REQUIRE(pasted.metaType() == QMetaType::fromType<QImage>());
    CHECK(pasted.value<QImage>().size() == image.size());
    CHECK(pasted.value<QImage>().pixelColor(0, 0) == QColor("#ff4f3a"));
}

// from tests/test_clipboard_files.py::test_a_pending_paste_restore_is_dropped
TEST_CASE("a pending paste restore is dropped", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    CHECK(temporary->schedule_restore(*stage.lease, 60'000ms));

    CHECK(temporary->writeFiles({tmp.path() / "walkthrough.pdf"}, "notes"));

    // The same state write_text clears: no lease left, no restore timed.
    CHECK_FALSE(Probe::pending(*temporary));
    CHECK_FALSE(Probe::restoreTimerActive(*temporary));
    CHECK(clipboard->text() == "notes");
}

// from tests/test_clipboard_files.py::test_a_selection_capture_in_flight_ends_first
TEST_CASE("a selection capture in flight ends first", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    std::vector<std::optional<std::string>> results;
    temporary->capture_selection(
        [] {}, [&](std::optional<std::string> text) { results.push_back(std::move(text)); },
        60'000ms);

    CHECK(temporary->writeFiles({tmp.path() / "walkthrough.pdf"}, "notes"));

    CHECK(results == std::vector<std::optional<std::string>>{std::string()});
    CHECK(clipboard->text() == "notes");
}

// from tests/test_clipboard_files.py::test_no_clipboard_writes_nothing_like_write_text
TEST_CASE("no clipboard writes nothing like write_text", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    auto temporary = temporary_over(nullptr);

    CHECK_FALSE(temporary->write_text("notes"));
    CHECK_FALSE(temporary->writeFiles({tmp.path() / "walkthrough.pdf"}, "notes"));
}

// from tests/test_clipboard_files.py::test_a_failed_write_keeps_the_pending_restore
TEST_CASE("a failed write keeps the pending restore", "[test_clipboard_files]") {
    fv::testkit::TempDataRoot tmp;
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    clipboard->fail_writes = true;

    CHECK_FALSE(temporary->write_text("notes"));
    CHECK_FALSE(temporary->writeFiles({tmp.path() / "walkthrough.pdf"}, "notes"));

    clipboard->fail_writes = false;
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "user text");
}

TEST_CASE("file paths go on as os.path.abspath gives them", "[temporary_clipboard]") {
    fv::testkit::TempDataRoot tmp;
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    // Relative to the current directory, "." and ".." folded, a trailing separator dropped.
    CHECK(temporary->writeFiles({"walkthrough.pdf", tmp.path() / "notes" / ".." / "." / "a.md",
                                 tmp.path() / "folder" / ""}));

    CHECK(files(clipboard->current()) == preferred({fs::current_path() / "walkthrough.pdf",
                                                    tmp.path() / "a.md", tmp.path() / "folder"}));
}

TEST_CASE("the port's write_files carries files, text, a PNG image and a marker",
          "[temporary_clipboard]") {
    fv::testkit::TempDataRoot tmp;
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(QColor("#123456"));
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        REQUIRE(image.save(&buffer, "PNG"));
    }
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    fv::focus::ClipboardFiles payload;
    payload.paths = {tmp.path() / "a.pdf"};
    payload.text = "notes";
    payload.png_image.assign(png.begin(), png.end());
    payload.marker = "token";
    CHECK(temporary->write_files(payload));

    const QMimeData& mime = clipboard->current();
    CHECK(files(mime) == preferred({tmp.path() / "a.pdf"}));
    CHECK(mime.text() == "notes");
    CHECK(mime.imageData().value<QImage>().pixelColor(1, 1) == QColor("#123456"));
    CHECK(mime.data(kAutoPasteMarkerFormat) == QByteArray("token"));
}
