// Replays tests/golden/ui_qt.clipboard.json (tools/parity/cases/ui_qt.clipboard.py):
// html_document, and TemporaryClipboard event traces over a fake clipboard with a change
// counter, its timers and selection polls driven step by step as the Python trace drives them.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMimeData>

#include <catch2/catch_test_macros.hpp>

#include "appkit/qt_bridge.h"
#include "base/testing/fake_clock.h"
#include "clipboard/html.h"
#include "clipboard_support.h"
#include "golden/golden.h"

using namespace fv::clipboard;
using namespace fv::clipboard::testing;
using fv::Json;
using Probe = fv::clipboard::TemporaryClipboardProbe;
using namespace std::chrono_literals;

namespace {

std::optional<std::string> optional_text(bool present, const QString& text) {
    if (!present) {
        return std::nullopt;
    }
    return fv::qt::toUtf8(text);
}

const char* outcome_name(fv::focus::RestoreOutcome outcome) {
    switch (outcome) {
    case fv::focus::RestoreOutcome::Restored:
        return "restored";
    case fv::focus::RestoreOutcome::Skipped:
        return "skipped";
    case fv::focus::RestoreOutcome::Failed:
        return "failed";
    }
    return "";
}

// Runs one trace and compares the state after every step with Python's.
void replay(const fv::golden::Case& c) {
    const bool counter = c.arg<bool>(1);
    const Json& steps = c.args()[2];
    const Json expected = c.result<Json>();
    REQUIRE(expected.size() == steps.size());

    auto owned = std::make_unique<FakeClipboardBackend>();
    FakeClipboardBackend* clipboard = owned.get();
    bool counter_hidden = false;
    SequenceSource sequence;
    if (counter) {
        sequence = [clipboard, &counter_hidden]() -> std::optional<std::uint64_t> {
            if (counter_hidden) {
                return std::nullopt;
            }
            return clipboard->counter;
        };
    }
    auto temporary = temporary_over(std::move(owned), sequence);
    fv::FakeClock clock;
    Probe::setClock(*temporary, clock);
    std::vector<std::optional<std::string>> selected;
    std::vector<std::string> failures;
    QObject::connect(temporary.get(), &TemporaryClipboard::restoreFailed,
                     [&](const QString& message) { failures.push_back(fv::qt::toUtf8(message)); });
    std::vector<fv::focus::ClipboardLease> leases;

    for (std::size_t index = 0; index < steps.size(); ++index) {
        const Json& step = steps[index];
        const std::string op = step[0].get<std::string>();
        const auto text_at = [&](std::size_t at) {
            return fv::qt::toQString(step[at].get<std::string>());
        };
        Json result = nullptr;
        if (op == "copy") {
            clipboard->copy_from_app(text_at(1));
        } else if (op == "copy_quiet" || op == "reown") {
            auto mime = std::make_unique<QMimeData>();
            mime->setText(text_at(1));
            clipboard->set_from_app(std::move(mime), op == "reown");
        } else if (op == "edit") {
            clipboard->current().setText(text_at(1));
        } else if (op == "counter_hidden") {
            counter_hidden = step[1].get<bool>();
        } else if (op == "fail_reads") {
            clipboard->raise_on_read = step[1].get<bool>();
        } else if (op == "fail_writes") {
            clipboard->fail_writes = step[1].get<bool>();
        } else if (op == "stage") {
            std::optional<std::string> html;
            if (!step[2].is_null()) {
                html = step[2].get<std::string>();
            }
            const auto stage =
                temporary->stage_text(step[1].get<std::string>(), html, step[3].get<bool>());
            if (stage.lease) {
                leases.push_back(*stage.lease);
            }
            result =
                Json::array({stage.written, stage.lease.has_value(), stage.restore_unavailable});
        } else if (op == "schedule_restore") {
            result = temporary->schedule_restore(leases.back(),
                                                 std::chrono::milliseconds(step[1].get<int>()));
        } else if (op == "commit") {
            result = temporary->commit_text(leases.back(), step[1].get<std::string>());
        } else if (op == "restore_now") {
            result = outcome_name(temporary->restore_now(leases.back()));
        } else if (op == "fire_restore") {
            Probe::fireRestore(*temporary);
        } else if (op == "prefetch") {
            Probe::startPrefetch(*temporary, std::chrono::milliseconds(step[1].get<int>()));
        } else if (op == "fire_prefetch") {
            Probe::firePrefetch(*temporary);
        } else if (op == "discard_prefetch") {
            Probe::discardPrefetch(*temporary);
        } else if (op == "cleanup") {
            temporary->cleanup();
        } else if (op == "write_text") {
            result = temporary->write_text(step[1].get<std::string>(), step[2].get<std::string>());
        } else if (op == "select") {
            const bool raises = step[2].get<bool>();
            temporary->capture_selection(
                [raises] {
                    if (raises) {
                        throw std::runtime_error("SendInput failed");
                    }
                },
                [&](std::optional<std::string> text) { selected.push_back(std::move(text)); },
                std::chrono::milliseconds(step[1].get<int>()));
        } else if (op == "poll") {
            clock.advance(std::chrono::milliseconds(step[1].get<int>()));
            Probe::poll(*temporary);
        } else {
            FAIL("unknown step " << op);
        }

        const QMimeData& mime = clipboard->current();
        Json selected_json = Json::array();
        for (const auto& text : selected) {
            selected_json.push_back(text ? Json(*text) : Json(nullptr));
        }
        const Json state = {
            {"result", result},
            {"text", optional_text(mime.hasText(), mime.text())},
            {"html", optional_text(mime.hasHtml(), mime.html())},
            {"marker", mime.hasFormat(kAutoPasteMarkerFormat)},
            {"counter", clipboard->counter},
            {"writes", clipboard->writes.size()},
            {"pending", Probe::pending(*temporary)},
            {"restore_timer", Probe::restoreTimerActive(*temporary)},
            {"prefetched", Probe::prefetched(*temporary).has_value()},
            {"prefetch_timer", Probe::prefetchTimerActive(*temporary)},
            {"selecting", Probe::selecting(*temporary)},
            {"captures", Probe::captures(*temporary)},
            {"selected", selected_json},
            {"failures", failures},
        };
        INFO("step " << index << " " << step.dump());
        REQUIRE(state.size() == expected[index].size());
        for (const auto& [key, value] : expected[index].items()) {
            INFO(key << ": C++ " << state.at(key).dump() << ", Python " << value.dump());
            CHECK(state.at(key) == value);
        }
    }
}

} // namespace

TEST_CASE("html_document matches OpenWhisper's _html_document", "[golden][ui_qt.clipboard]") {
    fv::golden::for_each("ui_qt.clipboard", "_html_document", [](const fv::golden::Case& c) {
        CHECK(html_document(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("TemporaryClipboard traces match OpenWhisper's", "[golden][ui_qt.clipboard]") {
    fv::golden::for_each("ui_qt.clipboard", "TemporaryClipboard", replay);
}
