// Host Mode's dashboard (test_host_mode.py, "the dashboard"): HostDashboard over a scripted
// FakeRemoteEngine shaped like the Python test's FakeHostService, with ManualExecutors for its
// background reads, a pinned clock and an injected clipboard (writing the real one is
// [interactive]).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>

#include "agentapi/testing/fake_mcp_control.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "remote/protocol.h"
#include "remote/testing/fake_remote_engine.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_remote/host_beacon.h"
#include "ui_remote/host_dashboard.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;
using fv::remote::ConnectedClient;
using fv::remote::testing::FakeRemoteEngine;

constexpr auto kTags = "[widget]";

// Processes events until `done` holds or `timeout_ms` passes; whether it held.
bool fvtestWaitFor(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return done();
}

std::chrono::system_clock::time_point referenceTime() {
    using namespace std::chrono;
    return sys_days{year{2026} / November / 14} + hours{10};
}

Json fakeEngine() {
    return {{"family", "parakeet"}, {"model", "v3"},      {"label", "Fake Parakeet"},
            {"device", "cuda"},     {"compute_type", ""}, {"streaming", true},
            {"available", true},    {"status", "ready"}};
}

// The dashboard over FakeHostService's state: sharing on port 47821 as devbox, two paired
// computers, Tailscale running for me@example.com.
struct Board {
    fv::FakeClock clock{referenceTime()};
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    FakeRemoteEngine engine;
    std::optional<QString> clipboard;
    std::unique_ptr<fv::HostDashboard> page;

    explicit Board(bool bound = true) {
        auto& state = engine.state;
        state.enabled = true;
        state.running = true;
        state.port = 47821;
        state.host_name = "devbox";
        state.address = "192.168.1.5:47821";
        state.address_kind = "lan";
        state.fingerprint = "ABCD1234EF567890ABCD1234EF567890";
        state.devices = {
            {.id = "d1",
             .name = "laptop",
             .paired_at = "2026-09-26T21:14:00+00:00",
             .last_seen = "2026-09-28T08:00:00+00:00",
             .via = "tailscale"},
            {.id = "d2",
             .name = "desk",
             .paired_at = "2026-09-27T17:02:00+00:00",
             .last_seen = "",
             .via = "code"},
        };
        state.tailscale = fv::net::TailscaleStatus{.state = "running",
                                                   .name = "devbox",
                                                   .address = "100.64.0.7",
                                                   .owner = "me@example.com"};
        state.tailscale_trust = true;
        state.keep_records = true;
        engine.engine = fakeEngine();
        state.engine = engine.engine;
        engine.activity = emptyActivity(now() - 600);
        engine.models = {{"parakeet", "v3", "Fake Parakeet"},
                         {"local_whisper", "turbo", "Whisper turbo"}};
        engine.summary = {{"dictation", {{"count", 2}, {"bytes", 2048}}}};
        page = std::make_unique<fv::HostDashboard>(bound ? &engine : nullptr, io, ui);
        page->setClock(clock);
        page->setClipboardWriter([this](const QString& text) { clipboard = text; });
    }

    double now() const {
        return std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
    }

    static Json emptyActivity(double since) {
        return {{"since", since},         {"transcriptions", 0}, {"audio_s", 0.0},
                {"host_s", 0.0},          {"previews", 0},       {"errors", 0},
                {"last_at", 0.0},         {"serial", 0},         {"devices", Json::object()},
                {"events", Json::array()}};
    }

    Json event(long long serial, const char* kind, Json fields = Json::object()) const {
        Json made = {{"serial", serial}, {"at", now() - 60}, {"kind", kind}, {"name", "laptop"}};
        made.update(fields);
        return made;
    }

    // Runs the page's background reads and their answers until nothing is left.
    void pump() {
        for (int round = 0; round < 20; ++round) {
            const auto ran = io.run_all() + ui.run_all();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (ran == 0) {
                return;
            }
        }
    }

    // _page(service): bound and read once.
    void refresh() {
        page->refresh();
        pump();
    }

    // _on_service_event(kind) then _flush(force=True).
    void notify(const std::string& kind) {
        engine.emit_changed(kind);
        ui.run_all();
        page->flush();
        pump();
    }

    const fv::HostDashboard::Parts& parts() const { return page->parts(); }
    bool shown(QWidget* widget) const { return widget->isVisibleTo(page.get()); }

    std::vector<fv::host_dashboard::EventRow*> activityRows() const {
        std::vector<fv::host_dashboard::EventRow*> rows;
        auto* layout = parts().activityLayout;
        for (int index = 0; index < layout->count(); ++index) {
            rows.push_back(
                qobject_cast<fv::host_dashboard::EventRow*>(layout->itemAt(index)->widget()));
        }
        return rows;
    }

    bool called(const std::string& call) const {
        const auto calls = engine.calls();
        return std::ranges::find(calls, call) != calls.end();
    }
};

// An MCP control whose status or token fails, for the copy buttons' failure paths.
class FailingMcp final : public fv::agentapi::McpServerControl {
public:
    bool fail_status = false;
    bool fail_token = false;
    fv::agentapi::ServerStatus current{.state = "running", .message = "Ready"};

    fv::agentapi::ServerStatus status() override {
        if (fail_status) {
            throw std::runtime_error("unavailable");
        }
        return current;
    }
    std::string token() override {
        if (fail_token) {
            throw std::runtime_error("unavailable");
        }
        return "test-mcp-access-token";
    }
    void start(int, bool, std::function<bool()>) override {}
    void stop(bool) override {}
};

fv::agentapi::FakeMcpControl& runningMcp(fv::agentapi::FakeMcpControl& server,
                                         std::string state = "running") {
    server.current.state = std::move(state);
    server.current.message = "Ready";
    server.running_token = "test-mcp-access-token";
    return server;
}

} // namespace

TEST_CASE("test_an_unbound_page_says_host_mode_isnt_available", kTags) {
    Board board(false);
    CHECK(board.parts().heroTitle->text() == QStringLiteral("Host Mode isn't available here"));
    CHECK(board.parts().heroDetail->text() ==
          QStringLiteral("The remote engine hasn't started in this window."));
    CHECK_FALSE(board.shown(board.parts().startButton));
    CHECK_FALSE(board.parts().engineCard->isEnabled());
    board.refresh();
    CHECK(board.parts().heroTitle->text() == QStringLiteral("Host Mode isn't available here"));
}

TEST_CASE("test_the_page_waits_to_be_shown_before_asking_the_service", kTags) {
    Board board;
    board.engine.emit_changed("state");
    board.pump();
    // Asking means asking Tailscale; a window that never shows it shouldn't.
    CHECK_FALSE(board.called("host_state()"));
    board.page->show();
    board.pump();
    CHECK(board.called("host_state()"));
    CHECK(board.parts().heroTitle->text() == QStringLiteral("Sharing Fake Parakeet"));
    // Hidden, every timer stops.
    board.page->hide();
    CHECK_FALSE(board.parts().beacon->isAnimating());
}

TEST_CASE("test_a_closed_page_is_held_by_neither_a_server_thread_nor_its_model_listing", kTags) {
    Board board;
    board.page->refresh(); // a host read and a model listing wait on the io executor
    QPointer<fv::HostDashboard> page = board.page.get();
    board.page.reset();
    CHECK(page.isNull());
    // The service notifies from its own thread after the page closed, and both reads finish.
    board.engine.emit_changed("clients");
    CHECK(board.io.run_all() > 0);
    board.ui.run_all();
    CHECK(board.called("host_models()"));
    CHECK(board.io.run_all() == 0);
}

TEST_CASE("test_sharing_off_offers_to_start", kTags) {
    Board board;
    board.engine.state.enabled = false;
    board.engine.state.running = false;
    board.refresh();
    const auto& parts = board.parts();
    CHECK(parts.heroTitle->text() == QStringLiteral("Sharing is off"));
    CHECK(parts.hero->property("state").toString() == QStringLiteral("off"));
    CHECK(board.shown(parts.startButton));
    CHECK_FALSE(board.shown(parts.stopButton));
    CHECK_FALSE(board.shown(parts.pairButton));
    CHECK(parts.pairNote->text() == QStringLiteral("Start sharing to pair a computer."));
    parts.startButton->click();
    board.pump();
    CHECK(board.called("set_host_enabled(true)"));
    CHECK(parts.heroTitle->text() == QStringLiteral("Sharing Fake Parakeet"));
}

TEST_CASE("test_sharing_names_the_engine_where_to_reach_it_and_the_identity", kTags) {
    Board board;
    board.refresh();
    const auto& parts = board.parts();
    CHECK(parts.hero->property("state").toString() == QStringLiteral("on"));
    CHECK(parts.beacon->state() == QStringLiteral("on"));
    const auto detail = parts.heroDetail->text();
    CHECK(detail.contains(QStringLiteral("devbox at 192.168.1.5:47821")));
    CHECK(detail.contains(QStringLiteral("Tailscale: devbox at 100.64.0.7")));
    CHECK(parts.heroIdentity->text() ==
          QStringLiteral("Identity ") + QString::fromStdString(fv::remote::short_fingerprint(
                                            board.engine.state.fingerprint)));
    CHECK(board.shown(parts.stopButton));
    parts.stopButton->click();
    board.pump();
    CHECK(board.called("set_host_enabled(false)"));
}

TEST_CASE("Host Mode words a VPN address and a host without one as OpenWhisper does", kTags) {
    Board board;
    board.engine.state.address_kind = "vpn";
    board.engine.state.tailscale.reset();
    board.refresh();
    CHECK(board.parts().heroDetail->text() ==
          QStringLiteral("devbox at 192.168.1.5:47821 (a VPN address; computers on your network "
                         "may not reach it)"));
    board.engine.state.address.reset();
    board.engine.state.host_name.clear();
    board.refresh();
    CHECK(board.parts().heroDetail->text() == QStringLiteral("This computer on port 47821"));
    board.engine.state.enabled = true;
    board.engine.state.running = false;
    board.refresh();
    CHECK(board.parts().heroTitle->text() == QStringLiteral("Starting to share…"));
    CHECK_FALSE(board.shown(board.parts().heroDetail));
    CHECK(board.shown(board.parts().stopButton));
}

TEST_CASE("test_a_port_in_use_is_shown_with_a_way_to_turn_sharing_off", kTags) {
    Board board;
    board.engine.state.running = false;
    board.engine.state.error = "Couldn't listen on port 47821 (in use).";
    board.refresh();
    const auto& parts = board.parts();
    CHECK(parts.hero->property("state").toString() == QStringLiteral("error"));
    CHECK(parts.heroTitle->text() == QStringLiteral("Sharing couldn't start"));
    CHECK(parts.heroDetail->text().startsWith(QStringLiteral("Couldn't listen on port 47821")));
    CHECK(parts.stopButton->text() == QStringLiteral("Turn off sharing"));
}

TEST_CASE("test_connected_computers_are_grouped_and_show_who_is_being_served", kTags) {
    Board board;
    const double now = board.now();
    board.engine.state.clients = {
        {.device_id = "d1",
         .name = "laptop",
         .address = "100.64.0.9",
         .busy = true,
         .since = now - 600},
        {.device_id = "d1", .name = "laptop", .address = "100.64.0.9", .since = now - 60},
        {.device_id = "d2", .name = "desk", .address = "192.168.1.7", .since = now - 300},
    };
    auto activity = Board::emptyActivity(now - 900);
    activity["transcriptions"] = 3;
    activity["audio_s"] = 60.0;
    activity["host_s"] = 3.0;
    activity["devices"] = {{"d1",
                            {{"name", "laptop"},
                             {"transcriptions", 3},
                             {"audio_s", 60.0},
                             {"host_s", 3.0},
                             {"previews", 0},
                             {"errors", 0},
                             {"last_at", now - 30}}}};
    board.engine.activity = activity;
    board.refresh();

    const auto& rows = board.page->clientRows();
    REQUIRE(rows.size() == 2);
    REQUIRE(rows.contains("d1"));
    REQUIRE(rows.contains("d2"));
    QPointer<fv::host_dashboard::ClientRow> laptop = rows.at("d1");
    CHECK(rows.at("d1")->state->text() == QStringLiteral("Transcribing…"));
    CHECK(rows.at("d1")->stats->text().contains(QStringLiteral("3 transcriptions")));
    CHECK(rows.at("d1")->stats->text().contains(QStringLiteral("2 connections")));
    CHECK(rows.at("d1")->stats->text() ==
          QStringLiteral("3 transcriptions · 1 min of audio · last just now · 2 connections"));
    CHECK(rows.at("d2")->state->text() == QStringLiteral("connected for 5 min"));
    CHECK(rows.at("d2")->stats->text() == QStringLiteral("Nothing transcribed yet"));
    CHECK(board.parts().clientsCard->badge->text() == QStringLiteral("2"));
    CHECK(board.parts().statConnected->value->text() == QStringLiteral("2"));
    CHECK(board.parts().statConnected->detail->text() == QStringLiteral("desk, laptop"));
    CHECK(board.parts().beacon->busy());
    for (const auto& [id, row] : board.page->deviceRows()) {
        CHECK(row.status->text() == QStringLiteral("Connected"));
    }

    board.engine.state.clients.clear();
    board.notify("clients");
    CHECK(board.page->clientRows().empty());
    // Gone at once, not left painted until Qt deletes them.
    CHECK((laptop.isNull() || !laptop->isVisibleTo(board.page.get())));
    CHECK(board.shown(board.parts().clientsEmpty));
    CHECK(board.parts().clientsEmpty->text() ==
          QStringLiteral("No computers are connected right now. Paired computers connect when "
                         "they dictate or record a meeting with this engine."));
    CHECK_FALSE(board.parts().beacon->busy());
}

TEST_CASE("Host Mode asks before stopping only while someone is connected", kTags) {
    Board board;
    std::vector<std::pair<QString, QString>> asked;
    bool answer = false;
    board.page->setStopConfirmation([&](const QString& text, const QString& informative) {
        asked.emplace_back(text, informative);
        return answer;
    });
    board.engine.state.clients = {
        {.device_id = "d1", .name = "laptop", .since = board.now()},
        {.device_id = "d1", .name = "laptop", .since = board.now()},
    };
    board.refresh();
    board.parts().stopButton->click();
    board.pump();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].first == QStringLiteral("laptop is connected."));
    CHECK(asked[0].second == QStringLiteral("Stopping cuts them off until you share again."));
    CHECK_FALSE(board.called("set_host_enabled(false)"));

    board.engine.state.clients.push_back({.device_id = "d2", .name = "desk"});
    board.refresh();
    answer = true;
    board.parts().stopButton->click();
    board.pump();
    REQUIRE(asked.size() == 2);
    CHECK(asked[1].first == QStringLiteral("2 computers are connected."));
    CHECK(board.called("set_host_enabled(false)"));
    // Starting needs no confirmation.
    board.parts().startButton->click();
    board.pump();
    CHECK(asked.size() == 2);
}

TEST_CASE("test_stats_and_recent_activity_follow_what_the_host_counted", kTags) {
    Board board;
    const Json events = Json::array({
        board.event(3, "transcribed", {{"audio_s", 8.8}, {"host_s", 0.41}}),
        board.event(2, "connected"),
        board.event(1, "paired", {{"via", "tailscale"}}),
    });
    auto activity = Board::emptyActivity(board.now() - 900);
    activity.update({{"transcriptions", 3},
                     {"audio_s", 90.0},
                     {"host_s", 4.5},
                     {"errors", 1},
                     {"serial", 3},
                     {"events", events}});
    board.engine.activity = activity;
    board.refresh();

    const auto& parts = board.parts();
    CHECK(parts.statTranscriptions->value->text() == QStringLiteral("3"));
    CHECK(parts.statTranscriptions->detail->text() == QStringLiteral("since 9:45 AM · 1 failed"));
    CHECK(parts.statAudio->value->text() == QStringLiteral("1 min"));
    CHECK(parts.statAudio->detail->text() == QStringLiteral("about 30 s each"));
    CHECK(parts.statSpeed->value->text() == QStringLiteral("20×"));
    CHECK(parts.statSpeed->detail->text() == QStringLiteral("1.5 s per request"));
    CHECK(parts.statSpeed->value->toolTip() ==
          QStringLiteral("Each second of decoding here covered 20 seconds of audio."));
    auto lines = board.activityRows();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0]->text->text() == QStringLiteral("laptop · 8.8 s of audio, back in 410 ms"));
    CHECK(lines[1]->text->text() == QStringLiteral("laptop connected"));
    CHECK(lines[2]->text->text() == QStringLiteral("Paired laptop over Tailscale"));
    // Nothing is new on first sight.
    CHECK(std::ranges::none_of(lines, [](auto* line) { return line->glow() > 0; }));

    Json more = Json::array({board.event(4, "failed", {{"reason", "engine busy"}})});
    for (const auto& event : events) {
        more.push_back(event);
    }
    activity["serial"] = 4;
    activity["events"] = more;
    board.engine.activity = activity;
    board.notify("activity");
    lines = board.activityRows();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0]->text->text() == QStringLiteral("laptop · couldn't transcribe: engine busy"));
    CHECK(lines[0]->glow() > 0);
    CHECK(lines[1]->glow() == 0);
}

TEST_CASE("Host Mode's stats say last session once sharing stops", kTags) {
    Board board;
    board.engine.state.enabled = false;
    board.engine.state.running = false;
    auto activity = Board::emptyActivity(board.now() - 900);
    activity["transcriptions"] = 1;
    activity["audio_s"] = 2.0;
    activity["host_s"] = 0.1;
    board.engine.activity = activity;
    board.refresh();
    CHECK(board.parts().statTranscriptions->detail->text() == QStringLiteral("last session"));
    CHECK(board.parts().statConnected->detail->text() == QStringLiteral("sharing is off"));
    CHECK(board.parts().clientsEmpty->text() ==
          QStringLiteral("Nobody can connect while sharing is off."));
}

TEST_CASE("test_nothing_yet_says_where_activity_will_appear", kTags) {
    Board board;
    board.refresh();
    CHECK(board.shown(board.parts().activityEmpty));
    CHECK(board.parts().activityEmpty->text() ==
          QStringLiteral(
              "Nothing yet. Each request from a paired computer shows up here as it happens."));
    CHECK(board.parts().statSpeed->value->text() == QStringLiteral("—"));
    CHECK(board.parts().statSpeed->detail->text() == QStringLiteral("no requests yet"));
    CHECK(board.parts().statAudio->value->text() == QStringLiteral("0 s"));
    CHECK(board.parts().statConnected->detail->text() == QStringLiteral("none right now"));
    CHECK(board.shown(board.parts().clientsEmpty));
}

TEST_CASE("test_pairing_shows_the_code_until_canceled", kTags) {
    Board board;
    board.refresh();
    const auto& parts = board.parts();
    CHECK(board.shown(parts.pairButton));
    CHECK(parts.pairNote->text().contains(QStringLiteral("me@example.com")));
    parts.pairButton->click();
    board.pump();
    CHECK(board.called("open_pairing()"));
    CHECK(parts.pairingCode->text() == QStringLiteral("123 456"));
    CHECK(board.shown(parts.pairingBox));
    CHECK_FALSE(board.shown(parts.pairButton));
    CHECK(parts.pairingExpiry->text() ==
          QStringLiteral("Enter this code on the other computer. Expires in 5:00."));
    parts.cancelPairingButton->click();
    board.pump();
    CHECK(board.called("cancel_pairing()"));
    CHECK_FALSE(board.shown(parts.pairingBox));
    CHECK(board.shown(parts.pairButton));

    board.engine.state.tailscale_trust = false;
    board.refresh();
    CHECK(parts.pairNote->text() ==
          QStringLiteral("On the other computer, open Settings → Remote engine and pick this "
                         "one; you're asked here to allow it. Or enter the code shown here."));
}

TEST_CASE("test_paired_computers_show_how_they_paired_and_what_they_keep_here", kTags) {
    Board board;
    board.refresh();
    const auto& rows = board.page->deviceRows();
    REQUIRE(rows.size() == 2);
    CHECK(board.parts().devicesCard->badge->text() == QStringLiteral("2"));
    CHECK(rows.at("d2").detail->text() ==
          QStringLiteral("Paired Sep 27, 2026 · keeps 2 dictations (2.0 KB) here"));
    CHECK(rows.at("d1").detail->text().startsWith(
        QStringLiteral("Paired Sep 26, 2026 over Tailscale")));
    CHECK(rows.at("d1").status->text() == QStringLiteral("Seen Sep 28"));
    CHECK(rows.at("d2").status->text().isEmpty());

    board.engine.state.devices.clear();
    board.refresh();
    CHECK(board.page->deviceRows().empty());
    CHECK_FALSE(board.parts().devicesCard->badge->isVisibleTo(board.page.get()));
}

TEST_CASE("test_the_engine_picker_switches_through_the_signal_and_says_why_not", kTags) {
    Board board;
    board.refresh();
    auto* combo = board.parts().modelCombo;
    REQUIRE(combo->count() == 2);
    CHECK(combo->itemText(0) == QStringLiteral("Fake Parakeet"));
    CHECK(combo->itemText(1) == QStringLiteral("Whisper turbo"));
    CHECK(combo->currentText() == QStringLiteral("Fake Parakeet"));
    CHECK(combo->isEnabled());
    CHECK(board.parts().engineName->text() == QStringLiteral("Fake Parakeet"));
    CHECK(board.parts().engineDetail->text() == QStringLiteral("On CUDA · live preview supported"));
    std::vector<std::pair<QString, QString>> asked;
    QObject::connect(
        board.page.get(), &fv::HostDashboard::modelRequested,
        [&](const QString& family, const QString& model) { asked.emplace_back(family, model); });

    Q_EMIT combo->activated(0); // the one already served
    CHECK(asked.empty());
    Q_EMIT combo->activated(1);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == std::pair(QStringLiteral("local_whisper"), QStringLiteral("turbo")));
    CHECK(board.parts().engineDetail->text() == QStringLiteral("Switching to Whisper turbo…"));
    CHECK_FALSE(board.parts().modelCombo->isEnabled());

    board.page->showEngineError(QStringLiteral("This computer is transcribing right now."));
    CHECK(board.parts().engineStatus->text() ==
          QStringLiteral("This computer is transcribing right now."));
    CHECK(board.parts().modelCombo->isEnabled());
}

TEST_CASE("test_a_finished_load_ends_the_switch", kTags) {
    Board board;
    board.refresh();
    Q_EMIT board.parts().modelCombo->activated(1);
    board.page->setEngineBusy(true);
    board.page->flush();
    board.pump();
    CHECK(board.parts().engineDot->isAnimating() == board.parts().engineDot->isVisible());
    board.engine.engine.update({{"family", "local_whisper"},
                                {"model", "turbo"},
                                {"label", "Whisper turbo"},
                                {"streaming", false}});
    board.page->setEngineBusy(false);
    board.page->flush();
    board.pump();
    CHECK_FALSE(board.page->switching().has_value());
    CHECK(board.parts().engineName->text() == QStringLiteral("Whisper turbo"));
    CHECK(board.parts().modelCombo->currentText() == QStringLiteral("Whisper turbo"));
    CHECK(board.parts().engineDetail->text() == QStringLiteral("On CUDA · no live preview"));
}

TEST_CASE("Host Mode's engine card reads a compute type and a load in progress", kTags) {
    Board board;
    board.engine.engine["compute_type"] = "int8_float32";
    board.engine.engine["device"] = "cpu";
    board.refresh();
    CHECK(board.parts().engineDetail->text() ==
          QStringLiteral("On CPU (int8_float32) · live preview supported"));
    CHECK(board.parts().engineDot->status() == fv::EngineStatus::Ready);
    board.page->setEngineBusy(true);
    board.page->flush();
    board.pump();
    CHECK(board.parts().engineDetail->text() == QStringLiteral("Loading…"));
    CHECK_FALSE(board.parts().modelCombo->isEnabled());
    board.engine.engine["available"] = false;
    board.engine.engine["status"] = "not loaded";
    board.page->setEngineBusy(false);
    board.page->flush();
    board.pump();
    CHECK(board.parts().engineDot->status() == fv::EngineStatus::Attention);
    CHECK(board.parts().engineStatus->text() == QStringLiteral("not loaded"));
}

TEST_CASE("test_without_ready_models_the_picker_points_to_downloads", kTags) {
    Board board;
    board.engine.models.clear();
    board.engine.engine = {{"family", ""},
                           {"model", ""},
                           {"label", ""},
                           {"device", ""},
                           {"streaming", false},
                           {"available", false},
                           {"status", "Select a local engine there."}};
    board.refresh();
    const auto& parts = board.parts();
    CHECK(parts.modelCombo->currentText() == QStringLiteral("No models are ready"));
    CHECK_FALSE(parts.modelCombo->isEnabled());
    CHECK(parts.engineName->text() == QStringLiteral("No local engine selected"));
    CHECK(parts.engineStatus->text() == QStringLiteral("Select a local engine there."));
    CHECK(parts.engineHint->text() ==
          QStringLiteral("Download a speech model and its runtime here first."));
    CHECK(board.shown(parts.modelsLink));
    QStringList opened;
    QObject::connect(board.page.get(), &fv::HostDashboard::settingsRequested,
                     [&](const QString& destination) { opened.append(destination); });
    parts.modelsLink->click();
    parts.manageLink->click();
    CHECK(opened == QStringList{QStringLiteral("voice_model"), QStringLiteral("remote_engine")});
}

TEST_CASE("test_the_beacon_ripples_when_a_transcription_comes_back", kTags) {
    Board board;
    board.page->show();
    board.pump();
    CHECK(board.parts().beacon->ripples() == 0);
    auto activity = board.engine.activity;
    activity["transcriptions"] = 1;
    activity["serial"] = 1;
    activity["events"] =
        Json::array({board.event(1, "transcribed", {{"audio_s", 2.0}, {"host_s", 0.1}})});
    board.engine.activity = activity;
    board.notify("activity");
    CHECK(board.parts().beacon->ripples() == 1);
    CHECK(board.parts().beacon->isAnimating());
    board.page->hide();
    CHECK_FALSE(board.parts().beacon->isAnimating());
}

TEST_CASE("test_without_an_mcp_server_there_is_no_mcp_card", kTags) {
    Board board;
    board.refresh();
    CHECK_FALSE(board.shown(board.parts().mcpCard));
    CHECK_FALSE(board.parts().mcpCopyToken->isEnabled());
    CHECK_FALSE(board.parts().mcpCopyPrompt->isEnabled());
}

TEST_CASE("test_the_page_finds_the_apps_mcp_server_when_first_shown", kTags) {
    Board board;
    fv::agentapi::FakeMcpControl server;
    int lookups = 0;
    board.page->setMcpLookup([&]() -> fv::agentapi::McpServerControl* {
        ++lookups;
        return &server;
    });
    board.refresh();
    CHECK(board.shown(board.parts().mcpCard));
    CHECK(board.parts().mcpStatus->text() == QStringLiteral("Off"));
    board.refresh();
    CHECK(lookups == 1);
}

TEST_CASE("test_mcp_turns_on_and_off_with_the_setting_settings_uses", kTags) {
    Board board;
    fv::MemorySettingsStore settings(Json::object());
    fv::agentapi::FakeMcpControl server;
    board.page->setSettings(&settings);
    board.page->bindMcp(&server);
    board.refresh();
    const auto& parts = board.parts();
    CHECK(parts.mcpButton->text() == QStringLiteral("Turn on"));
    CHECK_FALSE(parts.mcpCopyToken->isEnabled());
    parts.mcpButton->click();
    CHECK(settings.get(fv::keys::mcp_enabled));
    REQUIRE(server.calls().size() == 1);
    CHECK(server.calls()[0] == fv::agentapi::FakeMcpControl::Call{"start", 8767, false});
    CHECK(parts.mcpStatus->text() == QStringLiteral("Running at http://127.0.0.1:8767/mcp"));
    CHECK(parts.mcpButton->text() == QStringLiteral("Turn off"));
    CHECK(parts.mcpCopyToken->isEnabled());
    parts.mcpButton->click();
    CHECK_FALSE(settings.get(fv::keys::mcp_enabled));
    CHECK(server.calls().back().method == "stop");
    CHECK(parts.mcpStatus->text() == QStringLiteral("Off"));
    CHECK_FALSE(parts.mcpCopyToken->isEnabled());
}

TEST_CASE("test_mcp_that_couldnt_start_says_why_and_offers_to_try_again", kTags) {
    Board board;
    fv::agentapi::FakeMcpControl server;
    const std::string message =
        "Could not listen on port 8767. Choose another port or close the app using it.";
    server.current.state = "error";
    server.current.message = message;
    board.page->bindMcp(&server);
    board.refresh();
    CHECK(board.parts().mcpStatus->text() == QStringLiteral("Couldn't start"));
    CHECK(board.parts().mcpDetail->text() == QString::fromStdString(message));
    CHECK(board.parts().mcpButton->text() == QStringLiteral("Try again"));
    CHECK(board.parts().mcpDot->status() == fv::EngineStatus::Attention);
}

TEST_CASE("test_mcp_says_it_reaches_what_paired_computers_keep_here", kTags) {
    Board board;
    fv::agentapi::FakeMcpControl server;
    board.page->bindMcp(&server);
    board.refresh();
    CHECK(board.parts().mcpIntro->text() ==
          QStringLiteral("Agents on this computer, such as Claude Code or Cursor, can search the "
                         "dictations and meetings saved here. Choose permissions for title and "
                         "settings changes in Settings → MCP. That includes the records paired "
                         "computers keep here."));
    board.engine.state.keep_records = false;
    board.refresh();
    CHECK_FALSE(board.parts().mcpIntro->text().contains(QStringLiteral("paired computers")));
    QStringList opened;
    QObject::connect(board.page.get(), &fv::HostDashboard::settingsRequested,
                     [&](const QString& destination) { opened.append(destination); });
    board.parts().mcpLink->click();
    CHECK(opened == QStringList{QStringLiteral("mcp")});
}

TEST_CASE("test_host_copy_prompt_targets_tailscale_and_explains_local_only", kTags) {
    Board board;
    fv::agentapi::FakeMcpControl server;
    runningMcp(server).current.remote_url = "http://100.82.22.3:8767/mcp";
    board.page->bindMcp(&server);
    board.refresh();
    CHECK(board.parts().mcpStatus->text().contains(QStringLiteral("http://100.82.22.3:8767/mcp")));
    CHECK(board.parts().mcpIntro->text().startsWith(
        QStringLiteral("Agents on this computer and over Tailscale")));
    board.parts().mcpCopyPrompt->click();
    REQUIRE(board.clipboard);
    CHECK(board.clipboard->contains(QStringLiteral("http://100.82.22.3:8767/mcp")));
    CHECK(board.clipboard->contains(QStringLiteral("same Tailscale network")));
    CHECK_FALSE(board.clipboard->contains(QStringLiteral("127.0.0.1")));
    server.current.remote_url.clear();
    board.refresh();
    CHECK(board.parts().mcpDetail->text().contains(QStringLiteral("Allow agents over Tailscale")));
    board.parts().mcpCopyPrompt->click();
    CHECK(board.clipboard->contains(QStringLiteral("Do not connect to or enable a different")));
}

TEST_CASE("test_host_copies_the_current_access_token_with_independent_feedback", kTags) {
    Board board;
    fv::agentapi::FakeMcpControl server;
    runningMcp(server);
    board.page->bindMcp(&server);
    board.refresh();
    server.running_token = "updated-mcp-access-token";
    const auto& parts = board.parts();
    parts.mcpCopyToken->click();
    CHECK(board.clipboard == QStringLiteral("updated-mcp-access-token"));
    CHECK(parts.mcpCopyToken->text() == QStringLiteral("Copied"));
    CHECK(parts.mcpCopyPrompt->text() == QStringLiteral("Copy agent install prompt"));
    CHECK_FALSE(parts.mcpStatus->text().contains(QStringLiteral("updated-mcp-access-token")));
    CHECK_FALSE(parts.mcpDetail->text().contains(QStringLiteral("updated-mcp-access-token")));
    // "Copied" lasts 2,000 ms.
    REQUIRE(fvtestWaitFor([&] { return parts.mcpCopyToken->text() != QStringLiteral("Copied"); },
                          3000));
    CHECK(parts.mcpCopyToken->text() == QStringLiteral("Copy access token"));
    parts.mcpCopyPrompt->click();
    CHECK(parts.mcpCopyPrompt->text() == QStringLiteral("Copied"));
    CHECK(parts.mcpCopyToken->text() == QStringLiteral("Copy access token"));
    CHECK(board.clipboard->contains(
        QStringLiteral("Authorization: Bearer updated-mcp-access-token")));
}

TEST_CASE("test_host_does_not_copy_a_token_after_mcp_leaves_running", kTags) {
    for (const char* state : {"stopped", "starting", "stopping", "error"}) {
        CAPTURE(state);
        Board board;
        fv::agentapi::FakeMcpControl server;
        runningMcp(server);
        board.page->bindMcp(&server);
        board.refresh();
        board.clipboard = QStringLiteral("existing clipboard");
        server.current.state = state;
        board.parts().mcpCopyToken->click();
        CHECK(board.clipboard == QStringLiteral("existing clipboard"));
        CHECK_FALSE(board.parts().mcpCopyToken->isEnabled());
        CHECK_FALSE(board.parts().mcpCopyPrompt->isEnabled());
        CHECK(board.parts().mcpCopyToken->text() == QStringLiteral("Copy access token"));
    }
}

TEST_CASE("test_host_token_unavailable_leaves_the_clipboard_unchanged", kTags) {
    for (const char* failure : {"empty_token", "token_error", "status_error", "unbound"}) {
        CAPTURE(failure);
        Board board;
        fv::agentapi::FakeMcpControl fake;
        FailingMcp failing;
        const std::string kind = failure;
        if (kind == "empty_token") {
            runningMcp(fake).running_token.clear();
            board.page->bindMcp(&fake);
        } else {
            board.page->bindMcp(&failing);
        }
        board.refresh();
        board.clipboard = QStringLiteral("existing clipboard");
        if (kind == "token_error") {
            failing.fail_token = true;
        } else if (kind == "status_error") {
            failing.fail_status = true;
            board.refresh();
            CHECK_FALSE(board.parts().mcpCopyToken->isEnabled());
            CHECK_FALSE(board.parts().mcpCopyPrompt->isEnabled());
        } else if (kind == "unbound") {
            board.page->bindMcp(nullptr);
            CHECK_FALSE(board.parts().mcpCopyToken->isEnabled());
        }
        // The button may be disabled by now; the slot behind it is what must refuse.
        QMetaObject::invokeMethod(board.parts().mcpCopyToken, "clicked", Qt::DirectConnection,
                                  Q_ARG(bool, false));
        CHECK(board.clipboard == QStringLiteral("existing clipboard"));
        CHECK(board.parts().mcpCopyToken->text() == QStringLiteral("Copy access token"));
    }
}

TEST_CASE("test_host_mcp_actions_fit_narrow_windows_and_reflow_when_widened", kTags) {
    Board board;
    fv::agentapi::FakeMcpControl server;
    runningMcp(server);
    board.page->bindMcp(&server);
    board.refresh();
    board.page->resize(420, 600);
    board.page->show();
    board.pump();
    const auto& parts = board.parts();
    CHECK(board.page->width() == 420);
    CHECK(parts.mcpActions->direction() == QBoxLayout::TopToBottom);
    const std::array<QPushButton*, 2> buttons = {parts.mcpCopyToken, parts.mcpCopyPrompt};
    auto* viewport = parts.scroll->viewport();
    for (auto* button : buttons) {
        CHECK(button->isVisibleTo(board.page.get()));
        const int left = button->mapTo(viewport, button->rect().topLeft()).x();
        CHECK(viewport->rect().contains(QRect(left, 0, button->width(), 1)));
    }
    CHECK(buttons[1]->y() > buttons[0]->y());
    // Two stats across below 700 px, four from there.
    CHECK(parts.statAudio->y() > parts.statConnected->y());
    board.page->resize(1200, 600);
    board.pump();
    CHECK(parts.mcpActions->direction() == QBoxLayout::LeftToRight);
    CHECK(buttons[1]->x() > buttons[0]->x());
    CHECK(parts.statAudio->y() == parts.statConnected->y());
}
