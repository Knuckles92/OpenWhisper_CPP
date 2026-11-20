// Reading the selection when the clipboard changes without the copy, or the copy changes nothing
// (tests/test_personalize_fix2_g4_wayland_selection.py's clipboard cases that are not Wayland's):
// a clipboard manager handing the user's own text over again is not the selection, and a
// selection equal to the clipboard still arrives. Every clipboard here is a
// FakeClipboardBackend.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

#include "clipboard/sequence.h"
#include "clipboard_support.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using namespace std::chrono_literals;

namespace {

const QString kUrl = QStringLiteral("https://example.com/the-url-i-copied");
const QString kParagraph = QStringLiteral("Selected paragraph to make formal.");

// The focused app: the copy shortcut puts its selection on the clipboard 30 ms later.
class TargetApp {
public:
    TargetApp(FakeClipboardBackend& clipboard, QString selected)
        : clipboard_(clipboard), selected_(std::move(selected)) {
        timer_.setSingleShot(true);
        timer_.setInterval(30);
        QObject::connect(&timer_, &QTimer::timeout,
                         [this] { clipboard_.copy_from_app(selected_); });
    }
    void operator()() {
        ++copies;
        timer_.start();
    }
    int copies = 0;

private:
    FakeClipboardBackend& clipboard_;
    QString selected_;
    QTimer timer_;
};

std::optional<std::string> capture(TemporaryClipboard& temporary, std::function<void()> send_copy) {
    std::vector<std::optional<std::string>> results;
    temporary.capture_selection(
        std::move(send_copy),
        [&](std::optional<std::string> text) { results.push_back(std::move(text)); }, 700ms);
    REQUIRE(wait_for([&] { return !results.empty(); }, 3000ms));
    return results[0];
}

} // namespace

// from
// tests/test_personalize_fix2_g4_wayland_selection.py::test_the_users_own_text_handed_over_again_is_not_taken_for_the_selection
TEST_CASE("the user's own text handed over again is not taken for the selection",
          "[test_personalize_fix2_g4_wayland_selection]") {
    // Without a change counter, as in Python's test: with Windows' counter every write is the
    // copy, as in Python.
    auto owned = text_backend(kUrl);
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    TargetApp target(*clipboard, kParagraph);

    // A clipboard manager takes over each new clipboard, so a change still holding the user's
    // text can come before the copy lands.
    CHECK(capture(*temporary, [&] {
              clipboard->copy_from_app(kUrl);
              target();
          }) == kParagraph.toStdString());
    CHECK(target.copies == 1);
    CHECK(clipboard->text() == kUrl);
}

// from
// tests/test_personalize_fix2_g4_wayland_selection.py::test_a_selection_equal_to_the_clipboard_still_arrives_by_the_deadline
TEST_CASE("a selection equal to the clipboard still arrives by the deadline",
          "[test_personalize_fix2_g4_wayland_selection]") {
    auto owned = text_backend(kParagraph);
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));
    TargetApp target(*clipboard, kParagraph);

    CHECK(capture(*temporary, [&] { target(); }) == kParagraph.toStdString());
    CHECK(clipboard->text() == kParagraph);
}

// from
// tests/test_personalize_fix2_g4_wayland_selection.py::test_only_wayland_without_data_control_hides_other_apps_copies
// The rows that are not Wayland's: every other platform sees other apps' copies. OpenWhisper_CPP
// does not use Wayland's data-control, so every Wayland row is hidden (docs/port: the clipboard's
// foreign_copies_visible).
TEST_CASE("only wayland hides other apps' copies", "[test_personalize_fix2_g4_wayland_selection]") {
    REQUIRE_FALSE(QGuiApplication::platformName().startsWith(QStringLiteral("wayland")));
    CHECK(foreign_copies_visible());
}
