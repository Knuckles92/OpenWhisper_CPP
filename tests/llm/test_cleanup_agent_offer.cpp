#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "agents/installed_agents.h"
#include "llm/cleanup_agent_offer.h"
#include "llm/cleanup_settings.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"

// test_cleanup_agent_offer.py: a detected Claude Code or Codex is offered for AI cleanup, once,
// and only becomes the runner when the user says yes.
namespace offer = fv::llm::cleanup_agent_offer;
using fv::Json;

namespace {

fv::agents::InstalledAgent agent(std::string id, std::optional<bool> signed_in = true,
                                 std::string problem = {}) {
    fv::agents::InstalledAgent made;
    made.id = std::move(id);
    made.path = "/bin/agent";
    made.version = "9.0.0";
    made.signed_in = signed_in;
    made.problem = std::move(problem);
    return made;
}

std::vector<std::string> ids(const std::vector<fv::agents::InstalledAgent>& agents) {
    std::vector<std::string> out;
    for (const auto& item : agents) {
        out.push_back(item.id);
    }
    return out;
}

Json on() {
    return Json{{"transcript_cleanup_enabled", true}};
}

} // namespace

TEST_CASE("test_pending", "[unit]") {
    CHECK(offer::pending(on()));
    CHECK_FALSE(offer::pending(Json::object())); // cleanup is off
    Json answered = on();
    answered["cleanup_agent_offer_answered"] = true;
    CHECK_FALSE(offer::pending(answered));
    // Any runner the user chose, OpenRouter included, is left alone.
    for (const char* provider : {"openrouter", "codex"}) {
        Json chosen = on();
        chosen["transcript_cleanup_provider"] = provider;
        CHECK_FALSE(offer::pending(chosen));
    }
}

TEST_CASE("test_only_signed_in_claude_code_and_codex_are_offered_claude_first", "[unit]") {
    fv::agents::ScanResult scan{{"codex", agent("codex")},
                                {"claude_code", agent("claude_code")},
                                {"opencode_cli", agent("opencode_cli")}};
    CHECK(ids(offer::offerable(&scan)) == std::vector<std::string>{"claude_code", "codex"});
    CHECK(offer::offerable(nullptr).empty());
    fv::agents::ScanResult signed_out{{"claude_code", agent("claude_code", false)},
                                      {"codex", agent("codex", std::nullopt)}};
    CHECK(offer::offerable(&signed_out).empty());
    fv::agents::ScanResult broken{{"claude_code", agent("claude_code", true, "Too old")}};
    CHECK(offer::offerable(&broken).empty());
    fv::agents::ScanResult missing{{"claude_code", std::nullopt}};
    CHECK(offer::offerable(&missing).empty());
}

TEST_CASE("test_accept_switches_cleanup_to_the_agent", "[unit]") {
    fv::MemorySettingsStore store(on());
    offer::accept("codex", store, false);
    const Json settings = *store.snapshot();
    CHECK(fv::llm::resolve_transcript_cleanup_provider(settings) == "codex");
    CHECK(fv::llm::resolve_transcript_cleanup_model(settings) == "");
    CHECK_FALSE(offer::pending(settings));
    // Accepting the offer alone records no consent: the CLI is selected, not authorized.
    CHECK(fv::keys::fv_cleanup_agent_consent.read(settings) == "");
    CHECK_FALSE(fv::llm::cleanup_agent_authorized(settings, "codex"));
    CHECK_THROWS_AS(offer::accept("opencode_cli", store, true), std::invalid_argument);
}

TEST_CASE("accepting the offer with consent authorizes the agent with its remembered model",
          "[unit]") {
    Json initial = on();
    initial["cleanup_model_memory"] = {{"claude_code", "opus"}, {"openrouter", "x/y"}};
    fv::MemorySettingsStore store(initial);
    offer::accept("claude_code", store, true);
    const Json settings = *store.snapshot();
    CHECK(fv::llm::resolve_transcript_cleanup_provider(settings) == "claude_code");
    CHECK(fv::llm::resolve_transcript_cleanup_model(settings) == "opus");
    CHECK(settings.at("cleanup_model_memory") ==
          Json{{"claude_code", "opus"}, {"openrouter", "x/y"}});
    CHECK(settings.at("cleanup_agent_offer_answered") == true);
    CHECK(fv::llm::cleanup_agent_authorized(settings, "claude_code"));
}

TEST_CASE("test_decline_keeps_the_default_and_is_not_asked_again", "[unit]") {
    fv::MemorySettingsStore store(on());
    offer::decline(store);
    const Json settings = *store.snapshot();
    CHECK(fv::llm::resolve_transcript_cleanup_provider(settings) == "openrouter");
    CHECK_FALSE(offer::pending(settings));
    CHECK_FALSE(settings.contains("fv_cleanup_agent_consent"));
}
