// What the Meeting Mode tab, Home and the consent dialog say about AI insights
// (test_ai_insights_agent_copy.py, test_meeting_mode_tab.py's TestAiInsightsDestination,
// test_meeting_audio_consent_dialog.py's TestCloudConsentCopy).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>

#include <QLabel>
#include <QPushButton>

#include "agents/testing/fake_installed_agents.h"
#include "meeting_agent/route.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_meeting/meeting_consent_dialog.h"

namespace {

using fv::Json;
using fv::agents::InstalledAgent;
using fv::agents::ScanResult;

const std::string kSettings = "Settings \xE2\x86\x92 Meeting Mode \xE2\x86\x92 Intelligence";

// The agents' own configuration stays out: no home folder, so no configured default model.
struct NoAgentConfig {
    NoAgentConfig() { fv::agents::set_installed_agents(fake); }
    ~NoAgentConfig() { fv::agents::set_installed_agents(nullptr); }
    std::shared_ptr<fv::agents::FakeInstalledAgents> fake =
        std::make_shared<fv::agents::FakeInstalledAgents>();
};

fv::AiCopyDeps scanned(std::optional<ScanResult> scan) {
    fv::AiCopyDeps deps;
    deps.cached_scan = [scan] {
        return scan;
    };
    return deps;
}

InstalledAgent claude() {
    InstalledAgent agent;
    agent.id = "claude_code";
    agent.path = "C:/bin/claude.exe";
    agent.version = "2.1.281";
    agent.account = "Claude Team";
    agent.signed_in = true;
    return agent;
}

Json claudeSettings(Json extra = Json::object()) {
    extra["meeting_agent_core"] = "claude_code";
    return extra;
}

QString body(const fv::MeetingConsentDialog& dialog) {
    auto* label = dialog.findChild<QLabel*>(QStringLiteral("consentBodyLabel"));
    REQUIRE(label != nullptr);
    return label->text();
}

fv::MeetingConsentCopy copyFor(const Json& settings) {
    return fv::meetingConsentCopy(fv::meeting_agent::resolve_meeting_agent_route(settings),
                                  settings);
}

} // namespace

TEST_CASE("TestAiInsightsDestination::test_installed_agent_names_agent_and_model", "[widget]") {
    NoAgentConfig config;
    const auto [where, privacy] = fv::ai_insights_destination(
        claudeSettings({{"meeting_agent_models", {{"claude_code", "haiku"}}}}),
        scanned(ScanResult{{"claude_code", claude()}}));
    CHECK(where == "Claude Code \xC2\xB7 Haiku");
    CHECK(privacy == "your agent receives transcript text, never audio");
}

TEST_CASE("TestAiInsightsDestination::test_before_any_scan_it_names_the_choice", "[widget]") {
    NoAgentConfig config;
    const auto [where, privacy] = fv::ai_insights_destination(claudeSettings(), scanned({}));
    CHECK(where == "Claude Code \xC2\xB7 default model");
    CHECK(privacy == "your agent receives transcript text, never audio");
    // Without a seam it reads the set instance's cached scan, which has never run: no lookup.
    CHECK(fv::ai_insights_destination(claudeSettings()).first ==
          "Claude Code \xC2\xB7 default model");
    CHECK(config.fake->lookups() == 0);
}

TEST_CASE("TestAiInsightsDestination::test_missing_agent_points_to_settings", "[widget]") {
    NoAgentConfig config;
    const auto [where, privacy] = fv::ai_insights_destination(
        claudeSettings(), scanned(ScanResult{{"claude_code", std::nullopt}}));
    CHECK(where == "Claude Code not found");
    CHECK(privacy == "choose another agent in " + kSettings);
}

TEST_CASE("TestAiInsightsDestination::test_unusable_agent_says_what_to_fix", "[widget]") {
    NoAgentConfig config;
    const Json codex{{"meeting_agent_core", "codex"}};
    InstalledAgent old;
    old.id = "codex";
    old.path = "codex";
    old.version = "0.100.0";
    old.problem = "Too old.";
    auto [where, privacy] = fv::ai_insights_destination(codex, scanned(ScanResult{{"codex", old}}));
    CHECK(where == "Codex needs an update");
    CHECK(privacy.starts_with("update it, or choose another agent"));
    InstalledAgent signedOut;
    signedOut.id = "codex";
    signedOut.path = "codex";
    signedOut.version = "0.158.0";
    signedOut.signed_in = false;
    std::tie(where, privacy) =
        fv::ai_insights_destination(codex, scanned(ScanResult{{"codex", signedOut}}));
    CHECK(where == "Codex isn't signed in");
    CHECK(privacy.find(kSettings) != std::string::npos);
}

TEST_CASE("TestAiInsightsDestination::test_built_in_engine_is_unchanged", "[widget]") {
    const auto [where, privacy] =
        fv::ai_insights_destination({{"meeting_agent_core", "pi"},
                                     {"meeting_llm_provider", "openrouter"},
                                     {"meeting_llm_model", "deepseek/deepseek-v4.1-flash"}});
    CHECK(where == "OpenRouter \xC2\xB7 deepseek-v4.1-flash");
    CHECK(privacy == "sends transcript text, never audio");
}

TEST_CASE("test_remote_provider_sends_text_but_never_audio", "[unit]") {
    const auto [where, privacy] =
        fv::ai_insights_destination({{"meeting_llm_provider", "openrouter"},
                                     {"meeting_llm_model", "deepseek/deepseek-v4.1-flash"}});
    CHECK(where == "OpenRouter \xC2\xB7 deepseek-v4.1-flash");
    CHECK(privacy == "sends transcript text, never audio");
}

TEST_CASE("test_local_provider_runs_on_this_pc", "[unit]") {
    const auto [where, privacy] = fv::ai_insights_destination(
        {{"meeting_llm_provider", "ollama"}, {"meeting_llm_model", "llama3.1:8b"}});
    CHECK(where == "Ollama on this PC \xC2\xB7 llama3.1:8b");
    CHECK(privacy == "transcript text stays on this computer");
}

TEST_CASE("test_local_fast_judgments_keep_the_privacy_claim", "[unit]") {
    const Json local{{"meeting_llm_provider", "ollama"},
                     {"meeting_llm_model", "llama3.1:8b"},
                     {"typesafe_enabled", true},
                     {"typesafe_provider", "text_model"}};
    CHECK(fv::ai_insights_destination(local).second == "transcript text stays on this computer");
    // OpenWhisper_CPP's meetings judge only on this computer: a hosted judge (the cleanup model
    // here, or TypeSafe) receives nothing from a meeting, so the claim stands where OpenWhisper
    // drops it.
    Json hosted = local;
    hosted["transcript_cleanup_provider"] = "openrouter";
    hosted["transcript_cleanup_model"] = "vendor/model";
    hosted["typesafe_text_model"] = "cleanup";
    CHECK(fv::ai_insights_destination(hosted).second == "transcript text stays on this computer");
    hosted.erase("typesafe_provider");
    CHECK(fv::ai_insights_destination(hosted).second == "transcript text stays on this computer");
}

TEST_CASE("test_unresolvable_profile_points_to_settings", "[unit]") {
    fv::AiCopyDeps deps;
    deps.meeting_profile = [](const Json&) {
        return std::optional<fv::llm::TextLLMProfile>{};
    };
    const auto [where, privacy] = fv::ai_insights_destination(Json::object(), deps);
    CHECK(where == "No AI model chosen");
    CHECK(privacy.find(kSettings) != std::string::npos);
    // A resolver that fails reads the same.
    deps.meeting_profile = [](const Json&) -> std::optional<fv::llm::TextLLMProfile> {
        throw std::runtime_error("unreadable");
    };
    CHECK(fv::ai_insights_destination(Json::object(), deps).first == "No AI model chosen");
}

TEST_CASE("the tooltip explains the switch", "[unit]") {
    CHECK(fv::ai_insights_tooltip() ==
          "On: send transcript text to the AI model chosen\nin Settings for live insights and a "
          "final report.\nOff: keep the recording and transcript only.\nAudio routing is set "
          "separately in Voice & speakers.");
}

TEST_CASE("Home's AI insights line names the model, then who runs it", "[unit]") {
    NoAgentConfig config;
    CHECK(
        fv::meeting_intelligence_home_line(
            {{"meeting_llm_provider", "openrouter"},
             {"meeting_llm_model", "deepseek/deepseek-v4.1-flash"}}) ==
        "deepseek/deepseek-v4.1-flash \xC2\xB7 OpenRouter \xC2\xB7 OpenWhisper_CPP. Turn it on for "
        "each meeting when you start it.");
    CHECK(fv::meeting_intelligence_home_line(
              claudeSettings({{"meeting_agent_models", {{"claude_code", "haiku"}}}}),
              scanned(ScanResult{{"claude_code", claude()}})) ==
          "Haiku \xC2\xB7 Claude Code \xC2\xB7 your Claude Team sign-in. Turn it on for each "
          "meeting when you start it.");
    CHECK(fv::meeting_intelligence_home_line(claudeSettings(), scanned({})) ==
          "default model \xC2\xB7 Claude Code \xC2\xB7 your sign-in. Turn it on for each meeting "
          "when you start it.");
}

TEST_CASE("the Settings rail names who runs AI insights", "[unit]") {
    NoAgentConfig config;
    CHECK(fv::meeting_text_summary(
              {{"meeting_llm_provider", "openrouter"}, {"meeting_llm_model", "openrouter/free"}}) ==
          "OpenRouter \xC2\xB7 openrouter/free");
    CHECK(fv::meeting_text_summary(claudeSettings(),
                                   scanned(ScanResult{{"claude_code", claude()}})) ==
          "Claude Code \xC2\xB7 default model");
    CHECK(fv::meeting_text_summary(claudeSettings(),
                                   scanned(ScanResult{{"claude_code", std::nullopt}})) ==
          "Claude Code \xC2\xB7 not installed");
}

TEST_CASE("TestConsentForAnAgent::test_agent_copy_names_the_agent_not_the_endpoint", "[widget]") {
    const fv::meeting_agent::MeetingAgentRoute route{"claude_code", "claude_code", "", Json()};
    fv::MeetingConsentDialog dialog(fv::meetingConsentCopy(route, Json::object()));
    const QString text = body(dialog);
    CHECK(text.contains(QStringLiteral("sent to Claude Code on this computer")));
    CHECK(text.contains(QStringLiteral("own sign-in")));
    CHECK(text.contains(QStringLiteral("no files, no shell")));
    CHECK_FALSE(text.contains(QStringLiteral("OpenRouter")));
    CHECK(text.contains(QStringLiteral("AI insights do not upload audio")));
    CHECK(text.contains(QStringLiteral("OpenWhisper_CPP's meeting tools")));
    CHECK(dialog.remote());
    CHECK(dialog.agentId() == QStringLiteral("claude_code"));
    CHECK(dialog.accessibleDescription().contains(QStringLiteral("Claude Code")));
}

TEST_CASE("TestConsentForAnAgent::test_saved_agent_is_read_from_settings", "[widget]") {
    fv::MeetingConsentDialog dialog(copyFor({{"meeting_agent_core", "codex"}}));
    CHECK(dialog.agentId() == QStringLiteral("codex"));
    CHECK(body(dialog).contains(QStringLiteral("sent to Codex on this computer")));
}

TEST_CASE("TestConsentForAnAgent::test_chosen_backup_is_named_in_the_consent", "[widget]") {
    {
        fv::MeetingConsentDialog dialog(copyFor({{"meeting_backup_agent", "codex"}}));
        CHECK(body(dialog).contains(QStringLiteral("Codex, your chosen backup")));
    }
    fv::MeetingConsentDialog dialog(copyFor({{"meeting_backup_agent", ""}}));
    CHECK_FALSE(body(dialog).contains(QStringLiteral("backup")));
}

TEST_CASE("TestConsentForAnAgent::test_built_in_engine_keeps_the_endpoint_copy", "[widget]") {
    fv::MeetingConsentCopy copy;
    copy.destination = "Work gateway";
    copy.remote = true;
    fv::MeetingConsentDialog dialog(copy);
    CHECK(dialog.agentId().isEmpty());
    CHECK(body(dialog).contains(QStringLiteral("sent to Work gateway")));
    CHECK(body(dialog).contains(QStringLiteral("leaves this computer")));
    // The built-in engine over the saved settings names the endpoint, not an agent.
    fv::MeetingConsentDialog saved(copyFor({{"meeting_agent_core", "pi"}}));
    CHECK(saved.agentId().isEmpty());
    CHECK(body(saved).contains(QStringLiteral("sent to OpenRouter")));
}

TEST_CASE("TestCloudConsentCopy::test_cloud_consent_does_not_claim_audio_never_leaves",
          "[widget]") {
    fv::MeetingConsentDialog dialog(copyFor(Json::object()));
    const QString text = body(dialog);
    CHECK_FALSE(text.contains(QStringLiteral("Your audio never leaves this computer")));
    CHECK(text.contains(QStringLiteral("AI insights do not upload audio")));
    CHECK(text.contains(QStringLiteral("Past-meeting recall")));
    CHECK(text.contains(QStringLiteral("Remote computer")));
}

TEST_CASE("the consent names a local endpoint and says the text stays", "[widget]") {
    fv::MeetingConsentCopy local;
    local.destination = "your local server at 127.0.0.1:1234";
    local.remote = false;
    fv::MeetingConsentDialog dialog(local);
    CHECK(body(dialog).contains(QStringLiteral("127.0.0.1:1234")));
    CHECK(body(dialog).contains(QStringLiteral("does not leave this machine")));
    CHECK_FALSE(body(dialog).contains(QStringLiteral("OpenRouter")));
    // OpenWhisper_CPP's meetings use no hosted judge, so Fast judgments never weaken the claim.
    fv::MeetingConsentDialog judged(copyFor({{"meeting_llm_provider", "ollama"},
                                             {"meeting_llm_model", "qwen3:4b"},
                                             {"typesafe_enabled", true}}));
    CHECK(body(judged).contains(QStringLiteral("does not leave this machine")));
    CHECK_FALSE(body(judged).contains(QStringLiteral("Fast judgments")));
}

TEST_CASE("the consent's buttons: Not now and the default Turn on AI insights", "[widget]") {
    fv::MeetingConsentDialog dialog(copyFor(Json::object()));
    CHECK(dialog.windowTitle() == QStringLiteral("Turn On AI Insights"));
    CHECK(dialog.accessibleName() == QStringLiteral("Turn on AI insights for meetings"));
    auto* header = dialog.findChild<QLabel*>(QStringLiteral("headerLabel"));
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Turn on AI insights for meetings?"));
    auto* notNow = dialog.findChild<QPushButton*>(QStringLiteral("meetingConsentNotNowButton"));
    auto* enable = dialog.findChild<QPushButton*>(QStringLiteral("meetingConsentEnableButton"));
    REQUIRE(notNow != nullptr);
    REQUIRE(enable != nullptr);
    CHECK(notNow->text() == QStringLiteral("Not now"));
    CHECK(enable->text() == QStringLiteral("Turn on AI insights"));
    CHECK(enable->isDefault());
    CHECK(dialog.resultAction() == QStringLiteral("cancel"));
    notNow->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK(dialog.resultAction() == QStringLiteral("cancel"));
    enable->click();
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.resultAction() == QStringLiteral("enable"));
}
