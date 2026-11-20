// Rich-text paste: staging, committing and restoring a transcript with HTML, from OpenWhisper's
// tests/test_personalize_s5_clipboard.py. Every clipboard here is a FakeClipboardBackend, never
// the system one.

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <QGuiApplication>
#include <QMimeData>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "appkit/qt_bridge.h"
#include "clipboard/render.h"
#include "clipboard_support.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using fv::focus::RestoreOutcome;
using Probe = fv::clipboard::TemporaryClipboardProbe;

namespace {

const std::string HTML = "Thanks, <b>Dana Lee</b><br>Product designer";
const std::string PLAIN = "Thanks, Dana Lee\nProduct designer";
const std::string DOCUMENT = R"(<html><head><meta charset="utf-8"></head><body>)"
                             "<!--StartFragment-->" +
                             HTML + "<!--EndFragment--></body></html>";

QString q(const std::string& text) {
    return fv::qt::toQString(text);
}

std::unique_ptr<FakeClipboardBackend> backend_with(const std::optional<QString>& text) {
    auto backend = std::make_unique<FakeClipboardBackend>();
    if (text) {
        backend->current().setText(*text);
    }
    return backend;
}

// The renders fixture: each pre-render records its kind and what the clipboard held then.
struct Renders {
    using Call = std::tuple<std::string, QString, QString>;
    std::vector<Call> calls;
    FakeClipboardBackend* clipboard = nullptr;

    std::function<void()> recorder(std::string kind) {
        return [this, kind] {
            const QMimeData* mime = clipboard->mimeData();
            calls.emplace_back(kind, mime->text(), mime->html());
        };
    }
    Renderer renderer() { return Renderer{recorder("text"), recorder("html")}; }
};

} // namespace

// from
// tests/test_personalize_s5_clipboard.py::test_staged_html_sits_beside_the_text_and_the_ownership_marker
TEST_CASE("staged html sits beside the text and the ownership marker",
          "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text(PLAIN, HTML);

    const QMimeData& staged = clipboard->current();
    CHECK(stage.written);
    REQUIRE(stage.lease);
    CHECK(staged.text() == q(PLAIN));
    CHECK(staged.html() == q(DOCUMENT));
    CHECK(staged.data(kAutoPasteMarkerFormat) == Probe::leaseToken(*temporary, *stage.lease));
    CHECK(staged.data(kAutoPasteMarkerFormat).size() == 16);
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->current().text() == "previous text");
    CHECK_FALSE(clipboard->current().hasHtml());
}

// from tests/test_personalize_s5_clipboard.py::test_a_whole_html_document_is_staged_as_it_is
TEST_CASE("a whole html document is staged as it is", "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));
    FakeClipboardBackend* clipboard = owned.get();
    const std::string document = "<html><body><p>Hi</p></body></html>";

    auto temporary = temporary_over(std::move(owned));
    temporary->stage_text("Hi", document);

    CHECK(clipboard->current().html() == q(document));
}

// from tests/test_personalize_s5_clipboard.py::test_without_html_the_stage_is_plain_text_as_before
TEST_CASE("without html the stage is plain text as before", "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    const auto stage = temporary->stage_text("dictated", std::string());

    CHECK(clipboard->current().text() == "dictated");
    CHECK_FALSE(clipboard->current().hasHtml());
    REQUIRE(stage.lease);
    CHECK(stage.lease->html.empty());
}

// from tests/test_personalize_s5_clipboard.py::test_a_blank_clipboard_keeps_the_html
TEST_CASE("a blank clipboard keeps the html", "[test_personalize_s5_clipboard]") {
    const std::optional<QString> initial =
        GENERATE(std::optional<QString>(), std::optional<QString>(" \n "));
    auto owned = backend_with(initial);
    FakeClipboardBackend* clipboard = owned.get();

    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text(PLAIN, HTML);

    CHECK(stage.written);
    CHECK_FALSE(stage.lease);
    CHECK(clipboard->current().text() == q(PLAIN));
    CHECK(clipboard->current().html() == q(DOCUMENT));
    CHECK_FALSE(clipboard->current().hasFormat(kAutoPasteMarkerFormat));
}

// from tests/test_personalize_s5_clipboard.py::test_an_unreadable_clipboard_still_gets_the_html
TEST_CASE("an unreadable clipboard still gets the html", "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->raise_on_read = true;

    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text(PLAIN, HTML);

    clipboard->raise_on_read = false;
    CHECK(stage.written);
    CHECK(stage.restore_unavailable);
    CHECK(clipboard->current().text() == q(PLAIN));
    CHECK(clipboard->current().html() == q(DOCUMENT));
}

// from
// tests/test_personalize_s5_clipboard.py::test_a_failed_paste_leaves_text_and_html_without_the_marker
TEST_CASE("a failed paste leaves text and html without the marker",
          "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text(PLAIN, HTML);
    REQUIRE(stage.lease);

    CHECK(temporary->commit_text(*stage.lease, PLAIN));

    const QMimeData& committed = clipboard->current();
    CHECK(committed.text() == q(PLAIN));
    CHECK(committed.html() == q(DOCUMENT));
    CHECK_FALSE(committed.hasFormat(kAutoPasteMarkerFormat));
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Skipped);
}

// from tests/test_personalize_s5_clipboard.py::test_a_plain_commit_stays_a_plain_text_write
TEST_CASE("a plain commit stays a plain text write", "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated", std::nullopt);
    REQUIRE(stage.lease);
    clipboard->writes.clear();

    CHECK(temporary->commit_text(*stage.lease, "dictated"));
    REQUIRE(clipboard->writes.size() == 1);
    CHECK(clipboard->writes[0].kind == "text");
}

// from
// tests/test_personalize_s5_clipboard.py::test_rich_text_is_rendered_after_it_is_written_and_only_when_staged
TEST_CASE("rich text is rendered after it is written and only when staged",
          "[test_personalize_s5_clipboard]") {
    Renders renders;
    auto owned = backend_with(QString("previous text"));
    renders.clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), {}, renders.renderer());

    const auto first = temporary->stage_text(PLAIN, HTML);
    REQUIRE(first.lease);
    temporary->restore_now(*first.lease);
    temporary->stage_text("plain only", std::nullopt);

    CHECK(renders.calls == std::vector<Renders::Call>{
                               {"text", q(PLAIN), q(DOCUMENT)},
                               {"html", q(PLAIN), q(DOCUMENT)},
                               {"text", "plain only", ""},
                           });
}

// from
// tests/test_personalize_s5_clipboard.py::test_a_failing_rich_text_render_does_not_fail_the_stage
TEST_CASE("a failing rich text render does not fail the stage", "[test_personalize_s5_clipboard]") {
    auto owned = backend_with(QString("previous text"));

    const auto stage = temporary_over(std::move(owned), {},
                                      Renderer{{},
                                               [] {
                                                   throw std::runtime_error("clipboard busy");
                                               }})
                           ->stage_text(PLAIN, HTML);

    CHECK(stage.written);
    CHECK(stage.lease);
}

// from tests/test_personalize_s5_clipboard.py::test_offscreen_platform_has_no_rich_text_renderer
TEST_CASE("offscreen platform has no rich text renderer", "[test_personalize_s5_clipboard]") {
    REQUIRE(QGuiApplication::platformName() == "offscreen");
    CHECK_FALSE(system_html_renderer());
}
