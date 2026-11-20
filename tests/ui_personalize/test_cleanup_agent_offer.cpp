// The one-time offer of a signed-in Claude Code or Codex for AI cleanup
// (test_cleanup_agent_offer.py's UI half), with OpenWhisper_CPP's consent question in front of the
// change: the offer and the question are answered by the test, the scan is scripted.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <QApplication>

#include "base/testing/manual_executor.h"
#include "llm/cleanup_agent_offer.h"
#include "llm/cleanup_settings.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_personalize/cleanup_agent_offer.h"

using fv::Json;

namespace {

fv::agents::InstalledAgent agent(std::string id) {
    fv::agents::InstalledAgent made;
    made.id = std::move(id);
    made.path = "/bin/agent";
    made.version = "9.0.0";
    made.signed_in = true;
    return made;
}

struct OfferRig {
    fv::MemorySettingsStore store{Json{{"transcript_cleanup_enabled", true}}};
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    std::optional<fv::agents::ScanResult> cached =
        fv::agents::ScanResult{{"claude_code", agent("claude_code")}};
    std::optional<std::string> choice = "claude_code";
    QMessageBox::StandardButton consent = QMessageBox::Yes;
    int scans = 0;
    std::vector<QString> currents;
    std::vector<std::vector<std::string>> offered;
    std::vector<QString> questions;
    int accepted = 0;
    std::unique_ptr<fv::CleanupAgentOffer> offer;

    OfferRig() {
        fv::CleanupAgentOffer::Ports ports;
        ports.cached = [this] {
            return cached;
        };
        ports.scan = [this] {
            ++scans;
            return fv::agents::ScanResult{{"codex", agent("codex")}};
        };
        ports.ask = [this](QWidget*, const std::vector<fv::agents::InstalledAgent>& agents,
                           const QString& current) {
            std::vector<std::string> ids;
            for (const auto& item : agents) {
                ids.push_back(item.id);
            }
            offered.push_back(ids);
            currents.push_back(current);
            return choice;
        };
        ports.question = [this](QWidget*, const QString& title, const QString& text,
                                QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            questions.push_back(title + "\n" + text);
            return consent;
        };
        offer = std::make_unique<fv::CleanupAgentOffer>(store, io, ui, nullptr, ports);
        offer->onAccepted = [this] {
            ++accepted;
        };
    }
    Json settings() const { return *store.snapshot(); }
};

void processEvents() {
    QApplication::processEvents();
    QApplication::processEvents();
}

} // namespace

TEST_CASE("test_yes_switches_and_refreshes", "[widget]") {
    OfferRig rig;
    rig.offer->maybeOffer();
    CHECK(rig.offered.empty()); // never inside the toggle that caused it
    processEvents();
    REQUIRE(rig.offered.size() == 1);
    CHECK(rig.currents.front() == "OpenRouter");
    // OpenWhisper_CPP: the same consent question the AI cleanup settings ask for that runner.
    REQUIRE(rig.questions.size() == 1);
    CHECK(rig.questions.front().startsWith("Use Claude Code for AI cleanup?\n"));
    CHECK(rig.questions.front().contains("through the Claude Code CLI"));
    const Json settings = rig.settings();
    CHECK(fv::llm::resolve_transcript_cleanup_provider(settings) == "claude_code");
    CHECK(fv::keys::cleanup_agent_offer_answered.read(settings));
    CHECK(fv::llm::cleanup_agent_authorized(settings, "claude_code"));
    CHECK(rig.accepted == 1);
}

TEST_CASE("test_no_declines", "[widget]") {
    OfferRig rig;
    rig.choice = std::nullopt;
    rig.offer->maybeOffer();
    processEvents();
    CHECK(rig.offered.size() == 1);
    CHECK(rig.questions.empty());
    const Json settings = rig.settings();
    CHECK(fv::keys::cleanup_agent_offer_answered.read(settings));
    CHECK(fv::llm::resolve_transcript_cleanup_provider(settings) == "openrouter");
    CHECK_FALSE(settings.contains("transcript_cleanup_provider"));
    CHECK(rig.accepted == 0);
}

TEST_CASE("declining the consent question declines the offer", "[widget]") {
    OfferRig rig;
    rig.consent = QMessageBox::No;
    rig.offer->maybeOffer();
    processEvents();
    REQUIRE(rig.questions.size() == 1);
    const Json settings = rig.settings();
    CHECK(fv::keys::cleanup_agent_offer_answered.read(settings));
    CHECK_FALSE(settings.contains("transcript_cleanup_provider"));
    CHECK(fv::keys::fv_cleanup_agent_consent.read(settings) == "");
    CHECK(rig.accepted == 0);
    // Answered: it is not asked again.
    rig.offer->maybeOffer();
    processEvents();
    CHECK(rig.offered.size() == 1);
}

TEST_CASE("test_nothing_is_asked_without_an_agent_or_when_not_due", "[widget]") {
    OfferRig rig;
    rig.cached = fv::agents::ScanResult{{"claude_code", std::nullopt}};
    rig.offer->maybeOffer();
    processEvents();
    rig.store.save_setting("transcript_cleanup_enabled", Json(false));
    rig.offer->maybeOffer();
    processEvents();
    CHECK(rig.offered.empty());
    // Not answered: an agent installed later is still offered.
    CHECK_FALSE(fv::keys::cleanup_agent_offer_answered.read(rig.settings()));
}

TEST_CASE("without a cached scan the agents are looked for off the GUI thread", "[widget]") {
    OfferRig rig;
    rig.cached = std::nullopt;
    rig.choice = "codex";
    rig.offer->maybeOffer();
    CHECK(rig.scans == 0);
    CHECK(rig.io.run_all() == 1);
    CHECK(rig.scans == 1);
    CHECK(rig.offered.empty());
    rig.ui.run_all();
    REQUIRE(rig.offered.size() == 1);
    CHECK(rig.offered.front() == std::vector<std::string>{"codex"});
    CHECK(fv::llm::cleanup_agent_authorized(rig.settings(), "codex"));
}

TEST_CASE("a scan that comes back after the offer is gone does nothing", "[widget]") {
    OfferRig rig;
    rig.cached = std::nullopt;
    rig.offer->maybeOffer();
    rig.io.run_all();
    rig.offer.reset();
    rig.ui.run_all();
    CHECK(rig.offered.empty());
}
