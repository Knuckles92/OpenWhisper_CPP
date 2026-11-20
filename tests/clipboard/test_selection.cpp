// Reading the selection through a synthetic copy keeps the user's clipboard, from OpenWhisper's
// tests/test_personalize_foundation_clipboard.py and
// tests/test_personalize_fix_f2_clipboard_no_counter.py. Every clipboard here is a
// FakeClipboardBackend, never the system one.

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMimeData>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "appkit/qt_bridge.h"
#include "clipboard_support.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using Probe = fv::clipboard::TemporaryClipboardProbe;
using namespace std::chrono_literals;

namespace {

QThread* main_thread() {
    return QCoreApplication::instance()->thread();
}

// DelayedCopy / AppAnswersCopy: send_copy for an app that answers the copy shortcut 30 ms
// later. With cocoa set, the copy is one change Qt does not signal (QCocoaClipboard for a
// background app); otherwise the clipboard is emptied, then filled, then signalled. A null text
// never lands.
class DelayedCopy {
public:
    DelayedCopy(FakeClipboardBackend& clipboard, std::optional<QString> text, bool cocoa = false)
        : clipboard_(clipboard), text_(std::move(text)), cocoa_(cocoa) {
        timer_.setSingleShot(true);
        timer_.setInterval(30);
        QObject::connect(&timer_, &QTimer::timeout, [this] { land(); });
    }

    std::function<void()> send() {
        return [this] {
            ++sent;
            if (text_) {
                timer_.start();
            }
        };
    }

    int sent = 0;

private:
    void land() {
        if (cocoa_) {
            auto mime = std::make_unique<QMimeData>();
            mime->setText(*text_);
            clipboard_.set_from_app(std::move(mime), false);
        } else {
            clipboard_.copy_from_app(*text_);
        }
    }

    FakeClipboardBackend& clipboard_;
    std::optional<QString> text_;
    bool cocoa_;
    QTimer timer_;
};

// Result: records the text, what the user's clipboard held when it arrived, and the thread.
struct Result {
    using Call = std::tuple<std::optional<std::string>, QString, QThread*>;
    explicit Result(FakeClipboardBackend& clipboard) : clipboard(clipboard) {}

    std::function<void(std::optional<std::string>)> callback() {
        return [this](std::optional<std::string> text) {
            calls.emplace_back(std::move(text), clipboard.text(), QThread::currentThread());
        };
    }
    std::optional<std::string> text(std::size_t index = 0) const {
        return std::get<0>(calls.at(index));
    }
    QString held(std::size_t index = 0) const { return std::get<1>(calls.at(index)); }

    FakeClipboardBackend& clipboard;
    std::vector<Call> calls;
};

std::vector<QString> write_texts(const FakeClipboardBackend& clipboard) {
    std::vector<QString> texts;
    for (const auto& write : clipboard.writes) {
        texts.push_back(write.text);
    }
    return texts;
}

// _temporary: with or without the fake's counter.
std::unique_ptr<TemporaryClipboard> temporary_for(FakeClipboardBackend*& clipboard,
                                                  const QString& text, bool sequence = true) {
    auto owned = text_backend(text);
    clipboard = owned.get();
    return temporary_over(std::move(owned), sequence ? counter_of(*clipboard) : SequenceSource{});
}

// _capture of the f2 tests: the first result.
std::optional<std::string> capture(TemporaryClipboard& temporary, std::function<void()> send_copy,
                                   std::chrono::milliseconds timeout = 700ms) {
    std::vector<std::optional<std::string>> results;
    temporary.capture_selection(
        std::move(send_copy),
        [&](std::optional<std::string> text) { results.push_back(std::move(text)); }, timeout);
    REQUIRE(wait_for([&] { return !results.empty(); }, 3000ms));
    return results[0];
}

} // namespace

// from
// tests/test_personalize_foundation_clipboard.py::test_selection_is_read_and_the_clipboard_put_back_first
TEST_CASE("selection is read and the clipboard put back first",
          "[test_personalize_foundation_clipboard]") {
    const bool sequence = GENERATE(true, false);
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text", sequence);
    DelayedCopy copy(*clipboard, QString("selected words"));
    Result result(*clipboard);

    temporary->capture_selection(copy.send(), result.callback(), 700ms);

    REQUIRE(wait_for([&] { return !result.calls.empty(); }));
    CHECK(copy.sent == 1);
    REQUIRE(result.calls.size() == 1);
    CHECK(result.text() == "selected words");
    CHECK(result.held() == "user text");
    CHECK(std::get<2>(result.calls[0]) == main_thread());
    CHECK(write_texts(*clipboard) == std::vector<QString>{"user text"});
}

// from
// tests/test_personalize_foundation_clipboard.py::test_the_restored_clipboard_is_kept_for_the_next_paste
TEST_CASE("the restored clipboard is kept for the next paste",
          "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    Result result(*clipboard);

    // Kept alive: the app's reply timer must outlive this call.
    DelayedCopy copy(*clipboard, QString("selected"));
    temporary->capture_selection(copy.send(), result.callback(), 700ms);
    REQUIRE(wait_for([&] { return !result.calls.empty(); }));

    const auto& prefetched = Probe::prefetched(*temporary);
    REQUIRE(prefetched);
    CHECK(prefetched->snapshot->text == QString("user text"));
    CHECK(prefetched->sequence == clipboard->counter);

    const auto stage = temporary->stage_text("rewritten", std::nullopt);
    REQUIRE(stage.lease);
    const auto snapshot = Probe::leaseSnapshot(*temporary, *stage.lease);
    REQUIRE(snapshot);
    CHECK(snapshot->text == QString("user text"));
}

// from
// tests/test_personalize_foundation_clipboard.py::test_no_copy_within_the_timeout_reads_nothing_and_writes_nothing
TEST_CASE("no copy within the timeout reads nothing and writes nothing",
          "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    Result result(*clipboard);
    QElapsedTimer started;
    started.start();

    temporary->capture_selection([] {}, result.callback(), 80ms);

    REQUIRE(wait_for([&] { return !result.calls.empty(); }));
    CHECK(started.elapsed() >= 70);
    CHECK(result.text() == "");
    CHECK(result.held() == "user text");
    CHECK(clipboard->writes.empty());
}

// from tests/test_personalize_foundation_clipboard.py::test_a_failing_copy_shortcut_reads_nothing
TEST_CASE("a failing copy shortcut reads nothing", "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    Result result(*clipboard);

    temporary->capture_selection([] { throw std::runtime_error("SendInput failed"); },
                                 result.callback(), 700ms);

    REQUIRE_FALSE(result.calls.empty());
    CHECK(result.text() == "");
    CHECK(result.held() == "user text");
    CHECK(clipboard->writes.empty());
}

// from
// tests/test_personalize_foundation_clipboard.py::test_a_valid_prefetch_is_the_original_that_comes_back
TEST_CASE("a valid prefetch is the original that comes back",
          "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    temporary->request_prefetch(0ms);
    REQUIRE(wait_for([&] { return Probe::prefetched(*temporary).has_value(); }));
    // Same counter, so the prefetch still counts as the user's clipboard.
    clipboard->current().setText("changed without a write");
    Result result(*clipboard);

    // Kept alive: the app's reply timer must outlive this call.
    DelayedCopy copy(*clipboard, QString("selected"));
    temporary->capture_selection(copy.send(), result.callback(), 700ms);

    REQUIRE(wait_for([&] { return !result.calls.empty(); }));
    CHECK(result.text() == "selected");
    CHECK(result.held() == "user text");
}

// from
// tests/test_personalize_foundation_clipboard.py::test_a_pending_paste_restore_runs_before_the_original_is_kept
TEST_CASE("a pending paste restore runs before the original is kept",
          "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    const auto stage = temporary->stage_text("dictated", std::nullopt);
    REQUIRE(stage.lease);
    REQUIRE(clipboard->text() == "dictated");
    Result result(*clipboard);

    // Kept alive: the app's reply timer must outlive this call.
    DelayedCopy copy(*clipboard, QString("selected"));
    temporary->capture_selection(copy.send(), result.callback(), 700ms);

    REQUIRE(wait_for([&] { return !result.calls.empty(); }));
    CHECK(result.text() == "selected");
    CHECK(result.held() == "user text");
}

// from
// tests/test_personalize_foundation_clipboard.py::test_a_capture_requested_off_the_qt_thread_runs_on_it
TEST_CASE("a capture requested off the qt thread runs on it",
          "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    DelayedCopy copy(*clipboard, QString("selected"));
    Result result(*clipboard);
    std::vector<QThread*> sent_on;
    auto send = copy.send();

    std::thread worker([&] {
        temporary->capture_selection(
            [&] {
                sent_on.push_back(QThread::currentThread());
                send();
            },
            result.callback(), 700ms);
    });
    worker.join();

    REQUIRE(wait_for([&] { return !result.calls.empty(); }));
    CHECK(sent_on == std::vector<QThread*>{main_thread()});
    REQUIRE(result.calls.size() == 1);
    CHECK(result.text() == "selected");
    CHECK(result.held() == "user text");
    CHECK(std::get<2>(result.calls[0]) == main_thread());
}

// from
// tests/test_personalize_foundation_clipboard.py::test_one_capture_at_a_time_and_a_paste_ends_one_in_flight
TEST_CASE("one capture at a time and a paste ends one in flight",
          "[test_personalize_foundation_clipboard]") {
    FakeClipboardBackend* clipboard = nullptr;
    auto temporary = temporary_for(clipboard, "user text");
    Result first(*clipboard);
    Result second(*clipboard);

    temporary->capture_selection([clipboard] { clipboard->copy_from_app("partial"); },
                                 first.callback(), 700ms);
    temporary->capture_selection([] {}, second.callback(), 700ms);
    REQUIRE_FALSE(second.calls.empty());
    CHECK(second.text() == "");

    // A paste arriving mid-capture puts the user's clipboard back first.
    const auto stage = temporary->stage_text("dictated", std::nullopt);

    REQUIRE_FALSE(first.calls.empty());
    CHECK(first.text() == "");
    CHECK(first.held() == "user text");
    REQUIRE(stage.lease);
    const auto snapshot = Probe::leaseSnapshot(*temporary, *stage.lease);
    REQUIRE(snapshot);
    CHECK(snapshot->text == QString("user text"));
}

// --- Without a Windows-style counter ---------------------------------------------------------

// from
// tests/test_personalize_fix_f2_clipboard_no_counter.py::test_with_the_change_count_another_apps_copy_is_read_and_undone
TEST_CASE("with the change count another apps copy is read and undone",
          "[test_personalize_fix_f2_clipboard_no_counter]") {
    auto owned = text_backend("https://example.com/copied-url");
    FakeClipboardBackend* clipboard = owned.get();
    clipboard->counter = 7;
    auto temporary = temporary_over(std::move(owned), counter_of(*clipboard));
    DelayedCopy copy(*clipboard, QString("selected paragraph"), true);

    CHECK(capture(*temporary, copy.send()) == "selected paragraph");
    CHECK(write_texts(*clipboard) == std::vector<QString>{"https://example.com/copied-url"});
    CHECK(clipboard->text() == "https://example.com/copied-url");
}

// from
// tests/test_personalize_fix_f2_clipboard_no_counter.py::test_without_a_counter_or_a_signal_new_text_is_still_seen_and_undone
TEST_CASE("without a counter or a signal new text is still seen and undone",
          "[test_personalize_fix_f2_clipboard_no_counter]") {
    auto owned = text_backend("https://example.com/copied-url");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    DelayedCopy copy(*clipboard, QString("selected paragraph"), true);
    QElapsedTimer started;
    started.start();

    CHECK(capture(*temporary, copy.send()) == "selected paragraph");
    CHECK(started.elapsed() < 600);
    CHECK(clipboard->text() == "https://example.com/copied-url");
}

// from
// tests/test_personalize_fix_f2_clipboard_no_counter.py::test_without_a_counter_the_original_goes_back_even_when_nothing_was_seen
TEST_CASE("without a counter the original goes back even when nothing was seen",
          "[test_personalize_fix_f2_clipboard_no_counter]") {
    // Without a counter nothing proves the copy never landed.
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));
    DelayedCopy copy(*clipboard, std::nullopt, true);

    CHECK(capture(*temporary, copy.send(), 80ms) == "");
    CHECK(write_texts(*clipboard) == std::vector<QString>{"user text"});
}

// from
// tests/test_personalize_fix_f2_clipboard_no_counter.py::test_without_a_counter_a_copy_never_sent_writes_nothing
TEST_CASE("without a counter a copy never sent writes nothing",
          "[test_personalize_fix_f2_clipboard_no_counter]") {
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = temporary_over(std::move(owned));

    CHECK(capture(*temporary, [] {
              throw std::runtime_error("This Wayland desktop can't copy the selection");
          }) == "");
    CHECK(clipboard->writes.empty());
}

TEST_CASE("a clipboard that can't see other apps' copies refuses without copying",
          "[temporary_clipboard]") {
    auto owned = text_backend("user text");
    FakeClipboardBackend* clipboard = owned.get();
    auto temporary = std::make_unique<TemporaryClipboard>(std::move(owned), SequenceSource{},
                                                          Renderer{}, nullptr, false);
    int sent = 0;
    std::vector<std::optional<std::string>> results;

    temporary->capture_selection(
        [&] { ++sent; }, [&](std::optional<std::string> text) { results.push_back(text); }, 700ms);

    CHECK(sent == 0);
    CHECK(results == std::vector<std::optional<std::string>>{std::nullopt});
    CHECK(clipboard->writes.empty());
}
