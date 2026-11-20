// Sweep H2: open item "clipboard/temporary_clipboard.cpp: a lease whose marker reappears after a
// failed restore is reported Skipped where Python restores it". OpenWhisper's ClipboardLease
// carries its own snapshot and token, so whenever the clipboard holds that token again (a
// clipboard history putting the staged transcript back, say) restoring the lease restores the
// user's clipboard. Every clipboard here is a FakeClipboardBackend.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

#include <QMimeData>
#include <QString>

#include "clipboard_support.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using fv::focus::RestoreOutcome;

namespace {

// A copy of every format the clipboard holds now (marker included), as a clipboard history
// keeps an entry to paste back later.
std::unique_ptr<QMimeData> copy_of(FakeClipboardBackend& clipboard) {
    auto copy = std::make_unique<QMimeData>();
    for (const QString& format : clipboard.current().formats()) {
        copy->setData(format, clipboard.current().data(format));
    }
    return copy;
}

} // namespace

TEST_CASE("a failed restore can be tried again while the marker is there", "[sweep_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->fail_set_mime_call = 2;
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);

    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Failed);
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "previous text");
}

TEST_CASE("a lease whose marker reappears after a failed restore is restored",
          "[sweep_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->fail_set_mime_call = 2;
    auto temporary = temporary_over(std::move(owned));
    const auto first = temporary->stage_text("first transcript", std::nullopt);
    REQUIRE(first.lease);
    CHECK(temporary->restore_now(*first.lease) == RestoreOutcome::Failed);

    // The next dictation snapshots the clipboard as it is (the first transcript, marker and
    // all) and restores it: the first lease's marker is back.
    const auto second = temporary->stage_text("second transcript", std::nullopt);
    REQUIRE(second.lease);
    CHECK(clipboard->text() == "second transcript");
    CHECK(temporary->restore_now(*second.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "first transcript");

    CHECK(temporary->restore_now(*first.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "previous text");
}

TEST_CASE("a lease whose marker reappears after the user copied is restored", "[sweep_clipboard]") {
    auto owned = text_backend("previous text");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->fail_set_mime_call = 2;
    auto temporary = temporary_over(std::move(owned));
    const auto stage = temporary->stage_text("dictated text", std::nullopt);
    REQUIRE(stage.lease);
    auto kept = copy_of(*clipboard);
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Failed);

    // The user's own copy (no lease of ours) and then the history's paste back.
    CHECK(temporary->write_text("user copy"));
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Skipped);
    clipboard->set_from_app(std::move(kept));
    CHECK(temporary->restore_now(*stage.lease) == RestoreOutcome::Restored);
    CHECK(clipboard->text() == "previous text");
}
