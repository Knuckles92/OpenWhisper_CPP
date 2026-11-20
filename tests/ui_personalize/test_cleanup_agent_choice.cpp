// tests/test_settings_models.py::TestCleanupAgents, through the AI cleanup page's agent picker
// ("Who runs AI cleanup") and the Cleanup model card. OpenWhisper_CPP stages a tile until "Apply
// runner" and saves an agent only after the consent question that names it (difference
// a04-agent-cleanup-scope); the rig answers Yes, so what is saved and shown is OpenWhisper's.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include <QComboBox>
#include <QLabel>
#include <QMessageBox>

#include "base/identity.h"
#include "cleanup_support.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/agent_tile.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

Json apiValues(Json extra = Json::object()) {
    Json values{{"selected_model", "parakeet"},
                {"transcript_cleanup_provider", "openai"},
                {"transcript_cleanup_model", "gpt-test"},
                {"transcript_cleanup_model_sort", "alphabetical"}};
    for (const auto& [key, value] : extra.items()) {
        values[key] = value;
    }
    return values;
}

void showProvider(fv::TextModelPicker& picker, const char* id) {
    picker.providerCombo()->setCurrentIndex(
        picker.providerCombo()->findData(QString::fromUtf8(id)));
}

QString builtin() {
    return QString::fromUtf8(fv::legacy_builtin_agent_id.data(),
                             static_cast<qsizetype>(fv::legacy_builtin_agent_id.size()));
}

// A model from each agent's catalog in the rig (the Python test passes "chosen-model" straight to
// the handler; here it is picked in the agent's model row).
QString catalogModel(const std::string& agent) {
    if (agent == "claude_code") {
        return QStringLiteral("sonnet");
    }
    if (agent == "codex") {
        return QStringLiteral("gpt-b");
    }
    return QStringLiteral("anthropic/claude-sonnet");
}

} // namespace

// from
// tests/test_settings_models.py::TestCleanupAgents::test_cleanup_agent_persists_default_and_model_and_restores_api
TEST_CASE("test_cleanup_agent_persists_default_and_model_and_restores_api",
          "[test_settings_models][widget]") {
    const std::string agent = GENERATE("claude_code", "codex", "opencode_cli");
    const QString id = QString::fromStdString(agent);
    const QString chosen = catalogModel(agent);
    CleanupRig rig(apiValues());
    auto& section = rig.model();
    // "Installed agent…" in the provider list shows who runs AI cleanup.
    CHECK(section.runnerControls()->isHidden());
    showProvider(rig.picker(), fv::TextModelPicker::kInstalledAgentOption);
    CHECK_FALSE(section.runnerControls()->isHidden());
    rig.answer = QMessageBox::Yes;

    // _on_cleanup_agent_choice(agent): the agent with its own default model.
    rig.clickTile(id);
    section.applyRunnerButton()->click();
    CHECK(rig.saved("transcript_cleanup_provider") == agent);
    CHECK(rig.saved("transcript_cleanup_model") == "");
    rig.checkAgentCard(agent);
    CHECK(rig.agents().choice() == id);

    // _on_cleanup_agent_model_chosen, then a reload keeps the choice.
    rig.chooseAgentModel(chosen);
    CHECK(rig.saved("transcript_cleanup_model") == chosen.toStdString());
    rig.window().refresh();
    CHECK(rig.agents().choice() == id);
    CHECK(section.textSummary().contains(chosen));

    // The API choice restores the endpoint and model used before the agent.
    rig.clickTile(builtin());
    section.applyRunnerButton()->click();
    CHECK(rig.saved("transcript_cleanup_provider") == "openai");
    CHECK(rig.saved("transcript_cleanup_model") == "gpt-test");
    CHECK(rig.picker().isVisibleTo(&section));
    CHECK(section.descriptionLabel()->text().contains(QStringLiteral("API keys")));
    CHECK(rig.agents().tile(builtin())->nameLabel()->text() == QStringLiteral("API / local model"));

    // And the agent again remembers its model.
    showProvider(rig.picker(), fv::TextModelPicker::kInstalledAgentOption);
    rig.clickTile(id);
    section.applyRunnerButton()->click();
    CHECK(rig.saved("transcript_cleanup_model") == chosen.toStdString());
}

// from
// tests/test_settings_models.py::TestCleanupAgents::test_cleanup_agent_loads_saved_selection_without_api_key_warning
TEST_CASE("test_cleanup_agent_loads_saved_selection_without_api_key_warning",
          "[test_settings_models][widget]") {
    CleanupRig rig(apiValues({{"transcript_cleanup_provider", "codex"},
                              {"transcript_cleanup_model", ""},
                              {"fv_cleanup_agent_consent", "codex"}}));
    CHECK(rig.agents().choice() == QStringLiteral("codex"));
    rig.checkAgentCard("codex");
    CHECK(rig.model().textSummary() == QStringLiteral(u"Codex · default model"));
    rig.window();
    fv::WrappedLabel* status =
        rig.home().findChild<fv::WrappedLabel*>(QStringLiteral("homeStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->text().startsWith(QStringLiteral(u"Codex · ")));
    CHECK_FALSE(status->text().contains(QStringLiteral("API key needed")));
}
