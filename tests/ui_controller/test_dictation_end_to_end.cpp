// Dictation end to end (section 11.2): the real feature graph, offscreen, over the fakes. kp *
// down and up starts the recorder on the fake microphone, the overlay opens and the capture
// blocks arrive; kp * again stops it, the recording is saved and decoded through the speech
// service, the text is staged on the clipboard and pasted, Quick Record shows it, History and
// the stats save it and the History panel lists it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <QClipboard>
#include <QGuiApplication>
#include <QTextEdit>

#include "controller_support.h"
#include "history/dictation_stats.h"
#include "history/manager.h"
#include "sql/db.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_item.h"
#include "ui_history/history_sidebar.h"
#include "ui_record/stats_display.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

struct Row {
    std::string text, source_name, entry_kind, app_id, audio_file;
};

std::string textOf(const fv::sql::Value& value) {
    const auto* text = std::get_if<std::string>(&value);
    return text != nullptr ? *text : std::string();
}

std::vector<Row> historyRows(fv::Database& database) {
    return fvtest::offGui([&database] {
        return database.read([](fv::sql::Connection& connection) {
            std::vector<Row> rows;
            auto statement =
                connection.prepare("SELECT text, source_name, entry_kind, app_id, audio_file "
                                   "FROM transcription_history");
            while (statement.step()) {
                rows.push_back(Row{textOf(statement.column(0)), textOf(statement.column(1)),
                                   textOf(statement.column(2)), textOf(statement.column(3)),
                                   textOf(statement.column(4))});
            }
            return rows;
        });
    });
}

} // namespace

TEST_CASE("kp * dictation pastes the Parakeet text, saves it to History and lists it",
          "[e2e][widget]") {
    ControllerRig rig;
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(QStringLiteral("the user's own clipboard"));

    QStringList overlayStates;
    QObject::connect(&rig.overlay(), &fv::WaveformOverlay::stateChanged,
                     [&overlayStates](const QString& state) { overlayStates << state; });
    // What the clipboard held at each paste.
    std::vector<QString> heldAtPaste;
    rig.keys().on_send = [&heldAtPaste, clipboard](const std::string& what) {
        if (what == "paste") {
            heldAtPaste.push_back(clipboard->text());
        }
    };

    REQUIRE(rig.startEngine());

    rig.pressKeypad("*");
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(waitUntil([&rig] { return !rig.audio().streams().empty(); }));
    CHECK(rig.speak(1.5) > 0);
    // The record shortcut ignores a press within 300 ms of the last one (HOTKEY_DEBOUNCE_MS).
    waitUntil([] { return false; }, std::chrono::milliseconds(400));

    rig.pressKeypad("*");
    REQUIRE(waitUntil(
        [&rig] { return rig.hasStatus("Ready (Pasted)") && !rig.runtime().has_active_job(); },
        std::chrono::seconds(30)));

    // Pasted once, while the clipboard held the transcript; then the user's clipboard returns.
    CHECK(rig.keys().sent() == std::vector<std::string>{"paste"});
    REQUIRE(heldAtPaste.size() == 1);
    CHECK(heldAtPaste[0] == QStringLiteral("Hello from FioriVoce."));
    CHECK(waitUntil(
        [clipboard] { return clipboard->text() == QStringLiteral("the user's own clipboard"); }));
    CHECK(rig.statuses().back() == "Ready (Pasted)");

    // One History row and one stats row.
    const auto rows = historyRows(rig.database());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].text == "Hello from FioriVoce.");
    CHECK(rows[0].source_name == "Quick Record");
    CHECK(rows[0].entry_kind == "dictation");
    CHECK(rows[0].app_id == "notepad.exe");
    const std::filesystem::path recording =
        rig.paths().data_root() / "recordings" / rows[0].audio_file;
    CHECK(rows[0].audio_file.starts_with("recording_"));
    CHECK(recording.extension() == ".wav");
    CHECK(std::filesystem::exists(recording));
    auto& stats = rig.context().service<fv::dictation_stats::StatsRepository>();
    const auto statRows = fvtest::offGui([&stats] { return stats.rows(); });
    CHECK(std::ranges::count_if(statRows, [](const fv::dictation_stats::StatRow& row) {
              return row.entry_id.has_value();
          }) == 1);

    // Quick Record shows the text and the stats strip.
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("Hello from FioriVoce."));
    CHECK(tab->statsWidget()->isVisibleTo(tab));

    // The History panel's first card.
    auto& panel = rig.context().service<fv::HistoryPanel>();
    REQUIRE(panel.sidebar() != nullptr);
    rig.window().toggleSidePanel();
    REQUIRE(waitUntil([&panel] { return panel.sidebar()->renderedCardCount() == 1; }));
    const auto cards = panel.sidebar()->findChildren<fv::HistoryItemWidget*>();
    REQUIRE_FALSE(cards.empty());
    CHECK(cards.front()->entry().text == "Hello from FioriVoce.");

    // The overlay's trace: OverlayRouter showed recording, then processing, then transcribing
    // (stateChanged), and hid it: the hide fades out, then the overlay is idle and hidden
    // (finishHide emits no stateChanged, so idle is read from the overlay).
    REQUIRE(waitUntil([&rig] {
        return rig.overlay().currentState() == fv::WaveformOverlay::kStateIdle &&
               !rig.overlay().isVisible();
    }));
    overlayStates << rig.overlay().currentState();
    CHECK(overlayStates == QStringList{QStringLiteral("recording"), QStringLiteral("processing"),
                                       QStringLiteral("transcribing"), QStringLiteral("idle")});
}
