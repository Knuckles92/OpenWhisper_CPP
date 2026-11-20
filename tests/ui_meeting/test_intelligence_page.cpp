// The Intelligence page's insights model (tests/test_settings_models.py TestMeetingDestinations,
// TestMeetingAgentChoice and TestCleanupSettingsOwnership, the meeting half): the meeting
// picker's activation, who runs AI insights (the agent tiles and the built-in one), the agents'
// models and the backup agent. OpenWhisper_CPP's built-in tile is the product (M8 lead decision 1)
// and it has no Agent core combo or Pi/OpenCode SDK payload state (M8 change 3).
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QLabel>

#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_meeting/intelligence_page.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/agent_picker_text.h"
#include "ui_widgets/agent_tile.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::agents::AgentModel;
using fv::agents::InstalledAgent;
using fv::agents::ScanResult;

namespace {

InstalledAgent agent(std::string id, std::string version, std::string account) {
    InstalledAgent found;
    found.id = std::move(id);
    found.path = "C:/bin/" + found.id + ".exe";
    found.version = std::move(version);
    found.account = std::move(account);
    found.signed_in = true;
    return found;
}

// _agents_found: Claude Code and Codex are found; OpenCode is not installed.
ScanResult found() {
    return ScanResult{{"claude_code", agent("claude_code", "2.1.281", "Claude Team")},
                      {"codex", agent("codex", "0.158.0", "ChatGPT")},
                      {"opencode_cli", std::nullopt}};
}

// _settings_values' meeting half, then `extra`.
Json values(const Json& extra = Json::object()) {
    Json settings{{"meeting_llm_provider", "openrouter"},
                  {"meeting_llm_model", "deepseek/test-model"},
                  {"meeting_agent_core", "pi"}};
    for (const auto& [key, value] : extra.items()) {
        settings[key] = value;
    }
    return settings;
}

QString q(std::string_view text) {
    return fv::qt::toQString(text);
}

QString builtin() {
    return q(fv::legacy_builtin_agent_id);
}

struct PageRig {
    fv::agent_picker::ScanSeam original = fv::agent_picker::scanSeam();
    int scans = 0;
    fv::MemorySettingsStore settings;
    fv::AppServices services;
    std::optional<fv::AppContext> context;
    fv::SettingsPage container;
    std::optional<fv::IntelligencePage> page;

    explicit PageRig(const Json& extra = Json::object()) : settings(values(extra)) {
        auto& seam = fv::agent_picker::scanSeam();
        seam.cached_agents = [] {
            return std::optional<ScanResult>(found());
        };
        seam.scan_installed_agents = [this](bool) {
            ++scans;
            return found();
        };
        seam.list_agent_models = [](const InstalledAgent& agent) {
            if (agent.id == "claude_code") {
                return std::vector<AgentModel>{{"", "Claude Code default"},
                                               {"haiku", "Haiku (fastest)"},
                                               {"sonnet", "Sonnet"}};
            }
            return std::vector<AgentModel>{{"", "Codex default"}};
        };
        services.settings = &settings;
        context.emplace(services);
        page.emplace(container, *context);
        container.resize(900, 1400);
        container.show();
        QApplication::processEvents();
    }
    ~PageRig() {
        page.reset();
        fv::agent_picker::waitForWorkers(std::chrono::seconds(5));
        fv::agent_picker::scanSeam() = std::move(original);
    }

    fv::AgentPicker& agents() { return *page->findChild<fv::AgentPicker*>(); }
    fv::TextModelPicker& models() { return *page->findChild<fv::TextModelPicker*>(); }
    fv::InfoTile& tile() {
        return *page->findChild<fv::InfoTile*>(QStringLiteral("meeting_insights_model_tile"));
    }
    QComboBox& combo(const char* name) {
        return *page->findChild<QComboBox*>(QString::fromUtf8(name));
    }
    Json saved() const { return *settings.snapshot(); }
    // The Settings rail's value for Intelligence, over the same scan.
    std::string rail() const {
        fv::AiCopyDeps deps;
        deps.cached_scan = [] {
            return std::optional<ScanResult>(found());
        };
        return fv::meeting_text_summary(saved(), deps);
    }
    void choose(const QString& tileId) {
        fv::AgentTile* chosen = agents().tile(tileId);
        REQUIRE(chosen != nullptr);
        Q_EMIT chosen->clicked(tileId);
        QApplication::processEvents();
    }
};

void activate(fv::TextModelPicker& picker, const QString& provider, const QString& model) {
    picker.setProvider(provider);
    picker.modelCombo()->setCurrentText(model);
    picker.setStagedModel(provider, model);
    Q_EMIT picker.activationRequested(provider);
}

} // namespace

// from
// tests/test_settings_models.py::TestMeetingDestinations::test_meeting_llm_activation_persists_provider_and_model
TEST_CASE("test_meeting_llm_activation_persists_provider_and_model", "[widget]") {
    PageRig rig;
    activate(rig.models(), QStringLiteral("openai"), QStringLiteral("gpt-4o-mini"));

    CHECK(rig.saved()["meeting_llm_provider"] == "openai");
    CHECK(rig.saved()["meeting_llm_model"] == "gpt-4o-mini");
    CHECK(rig.models().modelCombo()->badgeText() == QStringLiteral("Active"));
    CHECK(rig.rail() == "OpenAI \xC2\xB7 gpt-4o-mini");
}

// from
// tests/test_settings_models.py::TestMeetingDestinations::test_meeting_custom_endpoint_activation_persists
TEST_CASE("test_meeting_custom_endpoint_activation_persists", "[widget]") {
    PageRig rig(
        Json{{"meeting_llm_provider", "custom_abcd1234"},
             {"meeting_llm_model", "local-qwen"},
             {"text_llm_profiles", Json::array({Json{{"id", "custom_abcd1234"},
                                                     {"name", "LM Studio"},
                                                     {"base_url", "http://127.0.0.1:1234/v1"},
                                                     {"api_key_env", ""}}})}});
    fv::TextModelPicker& picker = rig.models();
    CHECK(picker.provider() == QStringLiteral("custom_abcd1234"));
    CHECK(picker.urlLabel()->toolTip() == QStringLiteral("http://127.0.0.1:1234/v1"));

    activate(picker, QStringLiteral("custom_abcd1234"), QStringLiteral("other-local"));

    CHECK(rig.saved()["meeting_llm_provider"] == "custom_abcd1234");
    CHECK(rig.saved()["meeting_llm_model"] == "other-local");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_saved_agent_is_chosen_and_hides_the_built_in_insights_model
TEST_CASE("test_saved_agent_is_chosen_and_hides_the_built_in_insights_model", "[widget]") {
    PageRig rig(Json{{"meeting_agent_core", "claude_code"},
                     {"meeting_agent_models", {{"claude_code", "haiku"}}}});
    fv::AgentPicker& picker = rig.agents();

    CHECK(picker.choice() == QStringLiteral("claude_code"));
    CHECK(picker.tile(QStringLiteral("claude_code"))->isSelected());
    CHECK_FALSE(rig.models().isVisibleTo(&rig.tile()));
    CHECK(rig.rail() == "Claude Code \xC2\xB7 Haiku");
    CHECK(picker.modelCombo()->currentData().toString() == QStringLiteral("haiku"));
    CHECK(picker.usageLabel()->text().contains(QStringLiteral("Claude Team")));
}

// from tests/test_settings_models.py::TestMeetingAgentChoice::test_backup_agent_is_off_until_chosen
TEST_CASE("test_backup_agent_is_off_until_chosen", "[widget]") {
    PageRig rig;
    QComboBox& combo = rig.combo("meeting_backup_agent");

    CHECK(combo.currentData().toString().isEmpty());
    CHECK_FALSE(rig.saved().contains("meeting_backup_agent"));
    combo.setCurrentIndex(combo.findData(QStringLiteral("codex")));
    CHECK(rig.saved()["meeting_backup_agent"] == "codex");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_built_in_core_shows_the_insights_model_tile
TEST_CASE("test_built_in_core_shows_the_insights_model_tile", "[widget]") {
    PageRig rig;
    CHECK(rig.agents().choice() == builtin());
    // Lead decision 1: the built-in tile is the product, not Pi.
    CHECK(rig.agents().tile(builtin())->nameLabel()->text() == q(fv::identity().product));
    CHECK(rig.models().isVisibleTo(&rig.tile()));
    CHECK(rig.rail() == "OpenRouter \xC2\xB7 deepseek/test-model");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_choosing_an_agent_saves_it_and_openwhisper_restores_pi_even_if_missing
TEST_CASE("test_choosing_an_agent_saves_it_and_openwhisper_restores_pi_even_if_missing",
          "[widget]") {
    PageRig rig;
    rig.choose(QStringLiteral("codex"));
    CHECK(rig.saved()["meeting_agent_core"] == "codex");
    CHECK(rig.agents().choice() == QStringLiteral("codex"));
    CHECK_FALSE(rig.models().isVisibleTo(&rig.tile()));
    CHECK(rig.rail() == "Codex \xC2\xB7 default model");

    rig.choose(builtin());
    CHECK(rig.saved()["meeting_agent_core"] == "pi");
    CHECK(rig.models().isVisibleTo(&rig.tile()));
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_openwhisper_restores_pi_when_it_was_the_last_built_in_core
TEST_CASE("test_openwhisper_restores_pi_when_it_was_the_last_built_in_core", "[widget]") {
    PageRig rig;
    rig.choose(QStringLiteral("claude_code"));
    CHECK(rig.saved()["meeting_agent_core"] == "claude_code");
    rig.choose(builtin());
    CHECK(rig.saved()["meeting_agent_core"] == "pi");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_model_choice_merges_into_the_saved_models
TEST_CASE("test_model_choice_merges_into_the_saved_models", "[widget]") {
    PageRig rig(Json{{"meeting_agent_core", "claude_code"},
                     {"meeting_agent_models", {{"codex", "gpt-x"}}}});
    QComboBox* combo = rig.agents().modelCombo();
    REQUIRE(combo->findData(QStringLiteral("sonnet")) >= 0);

    Q_EMIT combo->activated(combo->findData(QStringLiteral("sonnet")));
    CHECK(rig.saved()["meeting_agent_models"] ==
          Json{{"codex", "gpt-x"}, {"claude_code", "sonnet"}});
    CHECK(rig.rail() == "Claude Code \xC2\xB7 Sonnet");

    combo = rig.agents().modelCombo();
    Q_EMIT combo->activated(combo->findData(QString()));
    CHECK(rig.saved()["meeting_agent_models"]["claude_code"] == "");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_saved_agent_that_is_missing_stays_selected_with_a_notice
TEST_CASE("test_saved_agent_that_is_missing_stays_selected_with_a_notice", "[widget]") {
    PageRig rig(Json{{"meeting_agent_core", "opencode_cli"}});
    fv::AgentPicker& picker = rig.agents();
    fv::AgentTile* tile = picker.tile(QStringLiteral("opencode_cli"));
    REQUIRE(tile != nullptr);

    CHECK(tile->isSelected());
    CHECK(tile->state().tone == fv::agent_picker::kMissing);
    CHECK_FALSE(tile->selectable());
    CHECK_FALSE(picker.notice()->isHidden());
    CHECK(picker.noticeLabel()->text().contains(QStringLiteral("until it is installed")));
    CHECK(picker.modelCard()->isHidden());
    CHECK_FALSE(rig.models().isVisibleTo(&rig.tile()));
    CHECK(rig.saved()["meeting_agent_core"] == "opencode_cli");
    CHECK(rig.rail() == "OpenCode \xC2\xB7 not installed");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_opening_intelligence_uses_the_cached_scan
TEST_CASE("test_opening_intelligence_uses_the_cached_scan", "[widget]") {
    PageRig rig;
    CHECK(rig.scans == 0);
    CHECK_FALSE(rig.agents().isScanning());
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_overview_names_the_agent_and_its_sign_in
TEST_CASE("test_overview_names_the_agent_and_its_sign_in", "[unit]") {
    fv::AiCopyDeps deps;
    deps.cached_scan = [] {
        return std::optional<ScanResult>(found());
    };
    const Json settings = values(Json{{"meeting_agent_core", "claude_code"},
                                      {"meeting_agent_models", {{"claude_code", "haiku"}}}});
    CHECK(fv::meeting_intelligence_overview(settings, deps) ==
          std::pair<std::string, std::string>{"Haiku",
                                              "Claude Code \xC2\xB7 your Claude Team sign-in"});
    // An installed agent counts as leaving whatever the provider check says.
    std::vector<std::string> asked;
    const auto local = [&asked](const std::string& provider) {
        asked.push_back(provider);
        return false;
    };
    CHECK(fv::meeting_intelligence_is_remote(settings, local));
    CHECK(asked.empty());
    CHECK(fv::agent_core_label("claude_code") == "Claude Code");
    // The built-in engine asks about the meeting text provider.
    CHECK_FALSE(fv::meeting_intelligence_is_remote(values(), local));
    CHECK(asked == std::vector<std::string>{"openrouter"});
    CHECK(fv::meeting_intelligence_is_remote(values(), [](const std::string&) { return true; }));
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_switching_to_an_installed_agent_restores_packaged_sdk_choice
// (the Agent core combo and the SDK payload are dropped, M8 change 3: the saved built-in core
// is what the built-in tile restores)
TEST_CASE("test_switching_to_an_installed_agent_restores_packaged_sdk_choice", "[widget]") {
    PageRig rig(Json{{"meeting_agent_core", "opencode"}});
    CHECK(rig.agents().choice() == builtin());
    rig.choose(QStringLiteral("codex"));
    CHECK_FALSE(rig.models().isVisibleTo(&rig.tile()));
    rig.choose(builtin());
    CHECK(rig.saved()["meeting_agent_core"] == "opencode");
    CHECK(rig.models().isVisibleTo(&rig.tile()));
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_legacy_direct_selection_shows_pi_without_an_api_option
// (the Agent core combo and its notice are dropped, M8 change 3)
TEST_CASE("test_legacy_direct_selection_shows_pi_without_an_api_option", "[widget]") {
    PageRig rig(Json{{"meeting_agent_core", "direct"}});
    CHECK(rig.agents().choice() == builtin());
    CHECK(rig.models().isVisibleTo(&rig.tile()));
    // Nothing is written until the user chooses.
    CHECK(rig.saved()["meeting_agent_core"] == "direct");
    rig.choose(QStringLiteral("codex"));
    rig.choose(builtin());
    CHECK(rig.saved()["meeting_agent_core"] == "pi");
}

// from
// tests/test_settings_models.py::TestMeetingAgentChoice::test_agent_thinking_levels_load_and_save
TEST_CASE("test_agent_thinking_levels_load_and_save", "[widget]") {
    PageRig rig(Json{{"meeting_agent_core", "claude_code"}});
    QComboBox& live = rig.combo("meeting_live_thinking");
    QComboBox& final = rig.combo("meeting_final_thinking");
    // The picker's model row sits inside the Insights model card.
    CHECK(rig.tile().isAncestorOf(rig.agents().modelCard()));
    CHECK(rig.tile().descriptionLabel()->text().contains(
        QStringLiteral("Claude Code runs live cards")));
    CHECK(live.currentData().toString() == QStringLiteral("low"));
    CHECK(final.currentData().toString() == QStringLiteral("medium"));
    CHECK(live.isEnabled());
    CHECK(final.findData(QStringLiteral("max")) >= 0);

    final.setCurrentIndex(final.findData(QStringLiteral("max")));
    CHECK(rig.saved()["meeting_final_thinking"] == "max");
    auto* note = rig.page->findChild<fv::WrappedLabel*>(QStringLiteral("meetingBuiltinThinking"));
    REQUIRE(note != nullptr);
    CHECK(note->text().contains(QStringLiteral("Sent to Claude Code")));
}

// from tests/test_settings_models.py::TestMeetingAgentChoice::test_packaged_sdk_thinks_at_medium
// (the built-in agent is the product, M8 lead decision 1)
TEST_CASE("test_packaged_sdk_thinks_at_medium", "[widget]") {
    PageRig rig;
    for (const char* name : {"meeting_live_thinking", "meeting_final_thinking"}) {
        QComboBox& combo = rig.combo(name);
        CHECK_FALSE(combo.isEnabled());
        CHECK(combo.currentData().toString() == QStringLiteral("medium"));
    }
    auto* note = rig.page->findChild<fv::WrappedLabel*>(QStringLiteral("meetingBuiltinThinking"));
    REQUIRE(note != nullptr);
    CHECK(note->text().startsWith(q(fv::identity().product) + QStringLiteral(" thinks at Medium")));
}
