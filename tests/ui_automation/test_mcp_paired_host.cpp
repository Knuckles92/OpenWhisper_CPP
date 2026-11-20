// The API & MCP page's paired-host frame (McpSettingsView::bindRemote) over FakeRemoteEngine:
// what FioriVoce shows in place of OpenWhisper's This computer / host tabs
// (tests/test_mcp_host_tabs.py, tests/test_remote_mcp_link.py and the host-tab cases of
// tests/test_automation_ui_api_page.py). The peer only checks, starts and ends the session its
// host granted (differences a17-paired-mcp); it never changes the host's MCP settings.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QString>

#include "base/testing/manual_executor.h"
#include "mcp_support.h"
#include "remote/errors.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/live_automation_section.h"

using fv::Json;
using fv::remote::testing::FakeRemoteEngine;
using namespace fvtest;

namespace {

const char* const kTag = "[mcp_paired_host][widget]";

fv::remote::ClientPairing jed() {
    fv::remote::ClientPairing pairing;
    pairing.host = "192.168.1.20";
    pairing.fingerprint = "ab:cd";
    pairing.host_name = "jed";
    pairing.device_id = "laptop";
    pairing.paired_at = "2026-11-19T10:00:00+00:00";
    return pairing;
}

// The start prompt's answers, replacing QMessageBox::question for a test.
class ScriptedQuestion {
public:
    ScriptedQuestion() : previous_(fv::action_prompts::question()) {
        fv::action_prompts::question() = [this](QWidget*, const QString& title, const QString& text,
                                                QMessageBox::StandardButtons,
                                                QMessageBox::StandardButton) {
            asked.emplace_back(title, text);
            if (answers.empty()) {
                return QMessageBox::Cancel;
            }
            const auto next = answers.front();
            answers.pop_front();
            return next;
        };
    }
    ~ScriptedQuestion() { fv::action_prompts::question() = previous_; }
    ScriptedQuestion(const ScriptedQuestion&) = delete;
    ScriptedQuestion& operator=(const ScriptedQuestion&) = delete;

    std::vector<std::pair<QString, QString>> asked;
    std::deque<QMessageBox::StandardButton> answers;

private:
    fv::action_prompts::Question previous_;
};

bool called(const FakeRemoteEngine& engine, const std::string& call) {
    for (const auto& made : engine.calls()) {
        if (made == call) {
            return true;
        }
    }
    return false;
}

bool calledStartingWith(const FakeRemoteEngine& engine, const std::string& prefix) {
    for (const auto& made : engine.calls()) {
        if (made.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

struct Frame {
    // Declared first, so it outlives the io pool's last task.
    FakeRemoteEngine engine;
    McpRig rig;
    Held<fv::McpSettingsView> view{nullptr};

    explicit Frame(std::optional<fv::remote::ClientPairing> pairing = jed()) {
        engine.pairing = std::move(pairing);
        view = rig.makeView();
        view->bindRemote(&engine, &rig.io());
        view->resize(700, 900);
        view->show();
    }
    bool shown() const { return view->pairedHost()->isVisibleTo(view.get()); }
    QString status() const { return view->hostStatus()->text(); }
    bool statusSays(const QString& text) const {
        return waitFor([&] { return view->hostStatus()->text().contains(text); });
    }
};

} // namespace

TEST_CASE("a computer with no host gets no paired-host frame", kTag) {
    Frame frame(std::nullopt);
    REQUIRE(waitFor([&] { return called(frame.engine, "client_pairing()"); }));
    pump();
    CHECK_FALSE(frame.shown());
    CHECK(frame.view->local()->isVisible());
}

TEST_CASE("bindRemote without a service or an io executor adds nothing", kTag) {
    FakeRemoteEngine engine;
    McpRig rig;
    auto view = rig.makeView();
    view->bindRemote(nullptr, &rig.io());
    view->bindRemote(&engine, nullptr);
    CHECK(view->pairedHost() == nullptr);
    CHECK(engine.calls().empty());
}

TEST_CASE("pairing shows the frame naming the host and unpairing hides it", kTag) {
    Frame frame(std::nullopt);
    REQUIRE(waitFor([&] { return called(frame.engine, "client_pairing()"); }));
    frame.engine.pairing = jed();
    frame.engine.emit_changed("client");
    REQUIRE(waitFor([&] { return frame.shown(); }));
    CHECK(frame.view->pairedHostTitle()->text() == "Paired computer: jed");
    frame.engine.pairing.reset();
    frame.engine.emit_changed("client");
    REQUIRE(waitFor([&] { return !frame.shown(); }));
    CHECK(frame.view->local()->isVisible());
}

TEST_CASE("checking reads the host's session state once and changes nothing", kTag) {
    Frame frame;
    REQUIRE(waitFor([&] { return frame.shown(); }));
    frame.engine.request_reply = Json{{"state", "stopped"}, {"port", 8768}, {"tailscale", false}};
    frame.engine.clear_calls();
    frame.view->checkHostButton()->click();
    REQUIRE(frame.statusSays("MCP on jed is stopped."));
    CHECK(frame.view->checkHostButton()->isEnabled());
    const auto calls = frame.engine.calls();
    CHECK(std::ranges::count(calls, std::string("remote_mcp_request(mcp_state,{},expected)")) == 1);
    CHECK_FALSE(calledStartingWith(frame.engine, "remote_mcp_request(mcp_configure"));
    CHECK(frame.rig.server.starts.empty());
}

TEST_CASE("a host that can't be managed says why, and checking again recovers", kTag) {
    struct Case {
        std::exception_ptr error;
        const char* shown;
    };
    const std::vector<Case> cases{
        {std::make_exception_ptr(
             fv::remote::RemoteRequestError("jed isn't letting paired computers", "forbidden")),
         "jed hasn't allowed this yet."},
        {std::make_exception_ptr(fv::remote::RemoteRequestError("Update", "unsupported")),
         "jed needs an update."},
        {std::make_exception_ptr(fv::remote::RemoteEngineError("Couldn't reach jed.")),
         "Can't reach jed. Couldn't reach jed."},
    };
    for (const auto& [error, shown] : cases) {
        Frame frame;
        REQUIRE(waitFor([&] { return frame.shown(); }));
        frame.engine.fail_next(error);
        frame.view->checkHostButton()->click();
        REQUIRE(frame.statusSays(QString::fromUtf8(shown)));
        if (QString::fromUtf8(shown).contains("allowed")) {
            CHECK(frame.status().contains("grant this computer a 15-minute MCP session"));
        }
        frame.engine.request_reply = Json{{"state", "running"}};
        frame.view->checkHostButton()->click();
        REQUIRE(frame.statusSays("MCP on jed is running."));
    }
}

TEST_CASE("starting asks first, sends only the session switch and never touches this computer",
          kTag) {
    ScriptedQuestion question;
    Frame frame;
    REQUIRE(waitFor([&] { return frame.shown(); }));
    frame.engine.request_reply = Json{{"state", "running"}, {"port", 8768}, {"tailscale", false}};

    // Cancel: nothing is sent.
    frame.view->startHostButton()->click();
    REQUIRE(waitFor([&] { return question.asked.size() == 1; }));
    REQUIRE(waitFor([&] { return frame.view->startHostButton()->isEnabled(); }));
    CHECK(question.asked.front().first == "Start host MCP session?");
    CHECK(question.asked.front().second.contains("Ask jed to start its authorized read-only MCP "
                                                 "session at 127.0.0.1:8768"));
    CHECK_FALSE(calledStartingWith(frame.engine, "remote_mcp_request("));

    question.answers.push_back(QMessageBox::Yes);
    frame.view->startHostButton()->click();
    REQUIRE(frame.statusSays("MCP on jed is running. Use End host session to disconnect."));
    CHECK(called(frame.engine,
                 R"(remote_mcp_request(mcp_configure,{"settings":{"enabled":true}},expected))"));
    CHECK(frame.view->startHostButton()->isEnabled());
    CHECK(frame.view->endHostButton()->isEnabled());
    CHECK(frame.rig.server.starts.empty());
    CHECK(frame.rig.server.current.state == "stopped");
}

TEST_CASE("a start the host refuses is reported in the frame", kTag) {
    ScriptedQuestion question;
    question.answers.push_back(QMessageBox::Yes);
    Frame frame;
    REQUIRE(waitFor([&] { return frame.shown(); }));
    frame.engine.fail_next(std::make_exception_ptr(fv::remote::RemoteRequestError(
        "The host has not granted this computer MCP control.", "forbidden")));
    frame.view->startHostButton()->click();
    REQUIRE(frame.statusSays("jed hasn't allowed this yet."));
    CHECK(frame.view->startHostButton()->isEnabled());
}

TEST_CASE("ending the session disconnects from the host", kTag) {
    Frame frame;
    REQUIRE(waitFor([&] { return frame.shown(); }));
    frame.view->endHostButton()->click();
    REQUIRE(frame.statusSays("Disconnected from the host MCP session."));
    CHECK(called(frame.engine, "close_mcp_control()"));
    CHECK_FALSE(calledStartingWith(frame.engine, "remote_mcp_request("));
}

TEST_CASE("starting while unpaired points at Remote engine", kTag) {
    ScriptedQuestion question;
    Frame frame;
    REQUIRE(waitFor([&] { return frame.shown(); }));
    frame.engine.pairing.reset();
    frame.view->startHostButton()->click();
    REQUIRE(frame.statusSays("Pair with a host in Remote engine settings first."));
    CHECK(question.asked.empty());
    frame.view->checkHostButton()->click();
    REQUIRE(frame.statusSays("Pair a computer in Remote engine settings first."));
}

TEST_CASE("the frame keeps its own controls and no live cards, below this computer's", kTag) {
    Frame frame;
    REQUIRE(waitFor([&] { return frame.shown(); }));
    auto* frameWidget = frame.view->pairedHost();
    CHECK(frameWidget->findChildren<fv::LiveAutomationSection*>().isEmpty());
    CHECK(frameWidget->findChildren<fv::McpSettingsPage*>().isEmpty());
    // This computer's page and its live cards stay where they are while the frame shows.
    CHECK(frame.view->local()->isVisible());
    CHECK(frame.view->live()->isVisible());
    auto* layout = frame.view->layout();
    CHECK(layout->indexOf(frame.view->local()) < layout->indexOf(frameWidget));
    CHECK(layout->indexOf(frame.view->live()) < layout->indexOf(frameWidget));
    QStringList buttons;
    for (auto* button : frameWidget->findChildren<QPushButton*>()) {
        buttons << button->text();
    }
    CHECK(buttons == QStringList{"Check host MCP status", "Start the authorized host session…",
                                 "End host session"});
}

TEST_CASE("a view closed while a request is queued is neither held nor touched", kTag) {
    FakeRemoteEngine engine;
    McpRig rig;
    engine.pairing = jed();
    fv::ManualExecutor io;
    const auto before = engine.changed.subscriber_count();
    auto view = rig.makeView();
    view->bindRemote(&engine, &io);
    CHECK(engine.changed.subscriber_count() == before + 1);
    io.run_all();
    pump();
    view->checkHostButton()->click();
    // The check is queued on io; the view goes before it runs.
    view = Held<fv::McpSettingsView>(nullptr);
    pump();
    CHECK(engine.changed.subscriber_count() == before);
    engine.emit_changed("client");
    io.run_all();
    pump();
    CHECK(called(engine, "remote_mcp_request(mcp_state,{},expected)"));
}
