// The Cleanup model card of the AI cleanup page (settings_models.py's cleanup section and its
// handlers) inside a real Settings window over fakes: key status read off the GUI thread, a
// scripted catalog and local-server detection, the endpoint dialog answered by the test.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItemModel>

#include "base/testing/manual_executor.h"
#include "cleanup_support.h"
#include "llm/cleanup_settings.h"
#include "llm/model_catalog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

// test_settings_models.py's _settings_values, as far as AI cleanup and meetings read them.
Json baseValues(Json extra = Json::object()) {
    Json values{
        {"selected_model", "parakeet"},
        {"meeting_llm_provider", "openrouter"},
        {"meeting_llm_model", "deepseek/test-model"},
        {"transcript_cleanup_provider", "openai"},
        {"transcript_cleanup_model", "gpt-test"},
        {"transcript_cleanup_model_sort", "alphabetical"},
    };
    for (const auto& [key, value] : extra.items()) {
        values[key] = value;
    }
    return values;
}

// _make_text_dialog.
Json textValues(const std::string& provider = "openai", const std::string& model = "gpt-test") {
    return baseValues({{"transcript_cleanup_provider", provider},
                       {"transcript_cleanup_model", model},
                       {"transcript_cleanup_model_sort", "most-popular"}});
}

const Json kCustomProfile{{"id", "custom_abcd1234"},
                          {"name", "LM Studio"},
                          {"base_url", "http://127.0.0.1:1234/v1"},
                          {"api_key_env", ""}};

QStringList providerLabels(fv::TextModelPicker& picker) {
    QStringList labels;
    for (int i = 0; i < picker.providerCombo()->count(); ++i) {
        labels << picker.providerCombo()->itemText(i);
    }
    return labels;
}

void showProvider(fv::TextModelPicker& picker, const char* id) {
    picker.providerCombo()->setCurrentIndex(
        picker.providerCombo()->findData(QString::fromUtf8(id)));
}

QString builtin() {
    return QString::fromUtf8(fv::legacy_builtin_agent_id.data(),
                             static_cast<qsizetype>(fv::legacy_builtin_agent_id.size()));
}

QString message(CleanupRig& rig) {
    return rig.window().messageLabel()->text();
}

const QString kKeyNeeded = QStringLiteral(u"Key needed in Settings → App → API keys");

} // namespace

// --- TestTextModelPicker ---

// from
// tests/test_settings_models.py::TestTextModelPicker::test_provider_credential_status_shows_when_key_is_found
TEST_CASE("test_provider_credential_status_shows_when_key_is_found",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues("openrouter"));
    rig.key("OPENROUTER_API_KEY", "test-key");
    QLabel* status = rig.picker().requirementLabel();
    CHECK(pumpUntil(
        [status] { return status->text() == QStringLiteral("OpenRouter API key found"); }));
    CHECK(status->property("available").toBool());
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_provider_credential_status_keeps_requirement_when_key_is_missing
TEST_CASE("test_provider_credential_status_keeps_requirement_when_key_is_missing",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues("openrouter"));
    QLabel* status = rig.picker().requirementLabel();
    CHECK(pumpUntil([status] {
        return status->text() ==
               QStringLiteral(u"Requires OpenRouter API key — add it in Settings → App → API keys");
    }));
    CHECK_FALSE(status->property("available").toBool());
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_picker_is_one_column_of_labeled_fields
TEST_CASE("test_picker_is_one_column_of_labeled_fields", "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    CHECK_FALSE(picker.refreshButton()->icon().isNull());
    CHECK(picker.providerCombo()->minimumSizeHint().width() <= 170);
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_text_models_use_one_labeled_provider_selector
TEST_CASE("test_text_models_use_one_labeled_provider_selector", "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    CHECK(providerLabels(picker) ==
          QStringList{QStringLiteral("OpenAI"), QStringLiteral("OpenRouter"),
                      QStringLiteral("Ollama"), QStringLiteral("LM Studio"), QStringLiteral("vLLM"),
                      QStringLiteral("llama.cpp"), QStringLiteral("LocalAI"),
                      QStringLiteral("Groq"), QStringLiteral("OpenCode Go"),
                      QStringLiteral("OpenCode Zen"), QStringLiteral(u"Installed agent…"),
                      QStringLiteral(u"Custom…")});
    CHECK(picker.provider() == QStringLiteral("openai"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-test"));
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_selecting_custom_asks_to_add_and_restores_the_current_endpoint
TEST_CASE("test_selecting_custom_asks_to_add_and_restores_the_current_endpoint",
          "[test_settings_models][widget]") {
    // Custom… is an action, never a selectable provider.
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    QObject::disconnect(&picker, &fv::TextModelPicker::addEndpointRequested, nullptr, nullptr);
    int addRequests = 0;
    QStringList providerChanges;
    QObject::connect(&picker, &fv::TextModelPicker::addEndpointRequested,
                     [&addRequests] { ++addRequests; });
    QObject::connect(&picker, &fv::TextModelPicker::providerChanged,
                     [&providerChanges](const QString& provider) { providerChanges << provider; });

    picker.providerCombo()->setCurrentIndex(
        picker.providerCombo()->findText(QStringLiteral(u"Custom…")));

    CHECK(addRequests == 1);
    CHECK(providerChanges.isEmpty());
    CHECK(picker.provider() == QStringLiteral("openai"));
    CHECK(picker.providerCombo()->currentText() == QStringLiteral("OpenAI"));
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_adding_from_custom_selects_the_new_endpoint
TEST_CASE("test_adding_from_custom_selects_the_new_endpoint", "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    rig.endpointAnswer = fv::TextEndpointDialog::Payload{"LM Studio", "http://127.0.0.1:1234/v1",
                                                         "", "chat", std::nullopt};

    picker.providerCombo()->setCurrentIndex(
        picker.providerCombo()->findText(QStringLiteral(u"Custom…")));

    const Json profiles = rig.saved("text_llm_profiles");
    REQUIRE(profiles.size() == 1);
    CHECK(picker.provider() == qs(profiles[0]["id"].get<std::string>()));
    CHECK(picker.providerCombo()->currentText() == QStringLiteral("LM Studio"));
    const QStringList labels = providerLabels(picker);
    CHECK(labels.mid(labels.size() - 3) == QStringList{QStringLiteral("LM Studio"),
                                                       QStringLiteral(u"Installed agent…"),
                                                       QStringLiteral(u"Custom…")});
    CHECK(rig.endpointDialogs.size() == 1);
    CHECK_FALSE(rig.endpointDialogs.front().has_value());
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_switching_provider_updates_the_same_model_picker
TEST_CASE("test_switching_provider_updates_the_same_model_picker",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();

    showProvider(picker, "openrouter");

    CHECK(picker.provider() == QStringLiteral("openrouter"));
    CHECK(picker.providerCombo()->currentText() == QStringLiteral("OpenRouter"));
    CHECK(picker.modelCombo()->currentText() ==
          qs(fv::llm::default_transcript_cleanup_model("openrouter", Json::object())));
    CHECK_FALSE(picker.sortCombo()->isHidden());
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_badge_marks_only_the_endpoint_actually_in_use
TEST_CASE("test_badge_marks_only_the_endpoint_actually_in_use", "[test_settings_models][widget]") {
    // Browsing another endpoint shows no badge, and its tooltip says why.
    CleanupRig rig(textValues("openrouter", "openrouter/free"));
    fv::TextModelPicker& picker = rig.picker();
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));

    showProvider(picker, "openai");
    CHECK(picker.modelCombo()->badgeText().isEmpty());
    CHECK(picker.modelCombo()->toolTip().contains(QStringLiteral(u"OpenRouter · openrouter/free")));

    showProvider(picker, "openrouter");
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_picking_a_model_persists_it_without_a_confirm_step
TEST_CASE("test_picking_a_model_persists_it_without_a_confirm_step",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    showProvider(picker, "openrouter");
    picker.modelCombo()->setCurrentText(QStringLiteral("anthropic/claude-test"));
    Q_EMIT picker.modelCombo()->textActivated(QStringLiteral("anthropic/claude-test"));

    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    CHECK(rig.saved("transcript_cleanup_model") == "anthropic/claude-test");
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
    CHECK(rig.model().textSummary() == QStringLiteral(u"OpenRouter · anthropic/claude-test"));
}

// from tests/test_settings_models.py::TestTextModelPicker::test_typed_model_id_persists_on_enter
TEST_CASE("test_typed_model_id_persists_on_enter", "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    QLineEdit* edit = picker.modelCombo()->lineEdit();

    edit->setText(QStringLiteral("gpt-typed"));
    Q_EMIT edit->textEdited(QStringLiteral("gpt-typed"));
    Q_EMIT edit->returnPressed();
    Q_EMIT edit->editingFinished();

    CHECK(rig.saved("transcript_cleanup_model") == "gpt-typed");
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_search_fragment_is_not_saved_when_focus_leaves
TEST_CASE("test_search_fragment_is_not_saved_when_focus_leaves", "[test_settings_models][widget]") {
    // Typing filters the dropdown, so a fragment is not a model choice.
    CleanupRig rig(textValues("openai", "gpt-test"));
    fv::TextModelPicker& picker = rig.picker();
    QLineEdit* edit = picker.modelCombo()->lineEdit();

    edit->setText(QStringLiteral("clau"));
    Q_EMIT edit->textEdited(QStringLiteral("clau"));
    Q_EMIT edit->editingFinished();

    CHECK(rig.saved("transcript_cleanup_model") == "gpt-test");
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-test"));
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_set_active_persists_provider_and_model
TEST_CASE("test_set_active_persists_provider_and_model", "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    showProvider(picker, "openrouter");
    picker.modelCombo()->setCurrentText(QStringLiteral("anthropic/claude-test"));

    rig.model().activateTextModel(QStringLiteral("openrouter"));

    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    CHECK(rig.saved("transcript_cleanup_model") == "anthropic/claude-test");
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
    CHECK(rig.model().textSummary() == QStringLiteral(u"OpenRouter · anthropic/claude-test"));
}

TEST_CASE("AI cleanup asks before sending transcripts through a named agent",
          "[test_settings_models][widget]") {
    const std::string provider = GENERATE("claude_code", "codex", "opencode_cli");
    Json values = textValues(provider, "saved-agent-model");
    values["transcript_cleanup_enabled"] = true;
    CleanupRig rig(values);
    auto& section = rig.model();
    fv::Button* apply = section.applyRunnerButton();
    REQUIRE(apply != nullptr);
    const QString id = QString::fromStdString(provider);
    const QString name = rig.agents().tile(id)->nameLabel()->text();
    // The saved agent is shown, staged, with the endpoint still in view: nothing is approved.
    CHECK_FALSE(section.runnerControls()->isHidden());
    CHECK(rig.agents().choice() == id);
    CHECK(apply->isEnabled());
    CHECK_FALSE(fv::llm::cleanup_agent_authorized(*rig.settings().snapshot(), provider));
    CHECK_FALSE(rig.picker().isHidden());

    rig.answer = QMessageBox::No;
    apply->click();
    REQUIRE(rig.asked.size() == 1);
    CHECK(rig.asked.front().contains(name));
    CHECK(rig.asked.front().contains(QStringLiteral("dictation transcripts")));
    CHECK(rig.asked.front().contains(QStringLiteral("cleanup instructions")));
    CHECK(rig.asked.front().contains(QStringLiteral("AI provider signed in there")));
    CHECK_FALSE(fv::llm::cleanup_agent_authorized(*rig.settings().snapshot(), provider));

    rig.answer = QMessageBox::Yes;
    apply->click();
    REQUIRE(fv::llm::cleanup_agent_authorized(*rig.settings().snapshot(), provider));
    CHECK(rig.saved("fv_cleanup_agent_consent") == provider);
    CHECK(rig.saved("transcript_cleanup_model") == "saved-agent-model");
    CHECK(section.textSummary().contains(QStringLiteral("saved-agent-model")));
    CHECK(rig.picker().isHidden());
    CHECK_FALSE(apply->isEnabled());

    // Clicking another agent's tile only stages it: nothing is asked or saved until Apply.
    const QString other =
        id == QStringLiteral("codex") ? QStringLiteral("claude_code") : QStringLiteral("codex");
    rig.clickTile(other);
    CHECK(rig.agents().choice() == other);
    CHECK(rig.asked.size() == 2);
    CHECK(rig.saved("transcript_cleanup_provider") == provider);
    CHECK(apply->isEnabled());
    rig.answer = QMessageBox::No;
    apply->click();
    CHECK(rig.asked.size() == 3);
    CHECK(rig.saved("transcript_cleanup_provider") == provider);
    CHECK(rig.saved("fv_cleanup_agent_consent") == provider);
    CHECK(rig.agents().choice() == id);

    rig.clickTile(builtin());
    apply->click();
    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    CHECK(rig.saved("fv_cleanup_agent_consent") == "");
    CHECK_FALSE(rig.picker().isHidden());
    CHECK(section.runnerControls()->isHidden());
    showProvider(rig.picker(), fv::TextModelPicker::kInstalledAgentOption);
    CHECK_FALSE(section.runnerControls()->isHidden());

    rig.answer = QMessageBox::Yes;
    rig.clickTile(id);
    apply->click();
    CHECK(rig.saved("transcript_cleanup_model") == "saved-agent-model");
    CHECK(rig.saved("fv_cleanup_agent_consent") == provider);
}

TEST_CASE("AI cleanup displays the approved agent model and effort while disabled",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues("claude_code", "haiku"));
    rig.settings().update({{"fv_cleanup_agent_consent", "claude_code"},
                           {"transcript_cleanup_enabled", false},
                           {"transcript_cleanup_reasoning", "high"},
                           {"cleanup_model_memory", {{"claude_code", "opus"}}}});
    auto& section = rig.model();

    CHECK_FALSE(fv::llm::cleanup_agent_authorized(*rig.settings().snapshot(), "claude_code"));
    CHECK(section.textSummary() == QStringLiteral(u"Claude Code · haiku"));
    CHECK(rig.picker().isHidden());
    // _assert_agent_card, and the saved model rather than the memory's.
    rig.checkAgentCard("claude_code");
    CHECK(rig.shownAgentModel() == QStringLiteral("haiku"));
    QComboBox* effort = section.reasoningCombo();
    REQUIRE(effort != nullptr);
    CHECK(effort->currentData().toString() == QStringLiteral("high"));
    QStringList levels;
    for (int i = 0; i < effort->count(); ++i) {
        levels << effort->itemData(i).toString();
    }
    CHECK(levels == QStringList{"low", "medium", "high", "xhigh", "max"});
    effort->setCurrentIndex(effort->findData(QStringLiteral("max")));
    CHECK(rig.saved("transcript_cleanup_reasoning") == "max");
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_local_preset_shows_its_default_url_and_offers_edit_not_delete
TEST_CASE("test_local_preset_shows_its_default_url_and_offers_edit_not_delete",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();

    showProvider(picker, "vllm");

    CHECK(picker.provider() == QStringLiteral("vllm"));
    CHECK(picker.urlLabel()->toolTip() == QStringLiteral("http://127.0.0.1:8000/v1"));
    CHECK(picker.requirementLabel()->text() == QStringLiteral("No API key required"));
    CHECK(picker.editButton()->isEnabled());
    CHECK_FALSE(picker.deleteButton()->isEnabled());
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_editing_a_local_preset_updates_its_server_and_key_variable
TEST_CASE("test_editing_a_local_preset_updates_its_server_and_key_variable",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    fv::TextModelPicker& picker = rig.picker();
    picker.setProvider(QStringLiteral("vllm"));
    rig.endpointAnswer = fv::TextEndpointDialog::Payload{"vLLM", "http://192.168.1.60:8000/v1",
                                                         "VLLM_API_KEY", "chat", std::nullopt};

    rig.model().editTextEndpoint(QStringLiteral("vllm"));

    REQUIRE(rig.endpointDialogs.size() == 1);
    REQUIRE(rig.endpointDialogs.front().has_value());
    CHECK(rig.endpointDialogs.front()->id == "vllm");
    CHECK(rig.saved("vllm_base_url") == "http://192.168.1.60:8000/v1");
    CHECK(rig.saved("vllm_api_key_env") == "VLLM_API_KEY");
    const auto profile = picker.currentProfile();
    REQUIRE(profile.has_value());
    CHECK(profile->base_url == "http://192.168.1.60:8000/v1");
    CHECK(profile->api_key_env == "VLLM_API_KEY");
    CHECK(pumpUntil([&rig] {
        std::lock_guard lock(rig.catalog->mutex);
        return !rig.catalog->requests.empty();
    }));
    {
        std::lock_guard lock(rig.catalog->mutex);
        CHECK(rig.catalog->requests ==
              std::vector<std::pair<std::string, std::string>>{{"vllm", "alphabetical"}});
    }
    CHECK(pumpUntil([&picker] { return picker.requirementLabel()->text() == kKeyNeeded; }));
    CHECK(message(rig).contains(QStringLiteral("Updated \"vLLM\"")));
    CHECK(rig.saved("text_llm_profiles").is_null());
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_custom_endpoint_appears_and_activation_persists
TEST_CASE("test_custom_endpoint_appears_and_activation_persists",
          "[test_settings_models][widget]") {
    CleanupRig rig(baseValues({{"text_llm_profiles", Json::array({kCustomProfile})},
                               {"transcript_cleanup_provider", "custom_abcd1234"},
                               {"transcript_cleanup_model", "local-qwen"}}));
    fv::TextModelPicker& picker = rig.picker();
    CHECK(providerLabels(picker).contains(QStringLiteral("LM Studio")));
    CHECK(picker.provider() == QStringLiteral("custom_abcd1234"));
    CHECK(picker.urlLabel()->toolTip() == QStringLiteral("http://127.0.0.1:1234/v1"));
    CHECK(picker.requirementLabel()->text() == QStringLiteral("No API key required"));

    picker.modelCombo()->setCurrentText(QStringLiteral("other-local"));
    rig.model().activateTextModel(QStringLiteral("custom_abcd1234"));

    CHECK(rig.saved("transcript_cleanup_provider") == "custom_abcd1234");
    CHECK(rig.saved("transcript_cleanup_model") == "other-local");
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_assigned_custom_endpoint_cannot_be_deleted
TEST_CASE("test_assigned_custom_endpoint_cannot_be_deleted", "[test_settings_models][widget]") {
    CleanupRig rig(baseValues({{"text_llm_profiles", Json::array({kCustomProfile})},
                               {"transcript_cleanup_provider", "custom_abcd1234"},
                               {"transcript_cleanup_model", "local-qwen"}}));

    rig.model().deleteTextEndpoint(QStringLiteral("custom_abcd1234"));

    CHECK(message(rig).contains(QStringLiteral("in use")));
    CHECK(rig.saved("text_llm_profiles").size() == 1);
    CHECK(rig.asked.isEmpty());
}

// A custom endpoint nobody uses is deleted after asking (settings_models.py
// _delete_text_endpoint).
TEST_CASE("an unused custom endpoint is deleted after the confirmation", "[widget]") {
    CleanupRig rig(baseValues({{"text_llm_profiles", Json::array({kCustomProfile})}}));
    fv::CleanupModelSection& section = rig.model();

    rig.answer = QMessageBox::No;
    section.deleteTextEndpoint(QStringLiteral("custom_abcd1234"));
    CHECK(rig.saved("text_llm_profiles").size() == 1);
    REQUIRE(rig.asked.size() == 1);
    CHECK(rig.asked.front() ==
          QStringLiteral("Delete endpoint: Delete \"LM Studio\"?\n\nMeetings that already "
                         "recorded this endpoint can still retry using the stored connection "
                         "snapshot."));

    rig.answer = QMessageBox::Yes;
    section.deleteTextEndpoint(QStringLiteral("custom_abcd1234"));
    CHECK(rig.saved("text_llm_profiles") == Json::array());
    CHECK(message(rig) == QStringLiteral("Deleted endpoint \"LM Studio\""));
    CHECK(section.picker()->providerCombo()->findData(QStringLiteral("custom_abcd1234")) == -1);
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_catalog_result_populates_matching_provider
TEST_CASE("test_catalog_result_populates_matching_provider", "[test_settings_models][widget]") {
    const auto [savedModel, expectedModel] =
        GENERATE(std::pair<const char*, const char*>{"gpt-4o-mini", "gpt-6-luna"},
                 std::pair<const char*, const char*>{"gpt-4.1", "gpt-4.1"});
    CleanupRig rig(textValues("openai", savedModel));
    const QStringList models{QStringLiteral("gpt-4.1"), QStringLiteral("gpt-4o-mini"),
                             QStringLiteral("o4-mini")};

    rig.model().onTextModelsLoaded(QStringLiteral("openai"), QStringLiteral("alphabetical"), models,
                                   QString());

    fv::TextModelPicker& picker = rig.picker();
    CHECK(picker.modelCombo()->count() == models.size());
    CHECK(picker.modelCombo()->currentText() == QString::fromUtf8(expectedModel));
    CHECK(rig.saved("transcript_cleanup_model") == savedModel);
    CHECK(picker.statusLabel()->text() == QStringLiteral("3 models available"));
}

// from
// tests/test_settings_models.py::TestTextModelPicker::test_opening_a_text_destination_loads_that_provider_catalog
TEST_CASE("test_opening_a_text_destination_loads_that_provider_catalog",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues());
    {
        std::lock_guard lock(rig.catalog->mutex);
        rig.catalog->models = {"gpt-test", "gpt-4.1"};
    }

    rig.window().selectDestination(qs(dest::cleanup));

    CHECK(pumpUntil([&rig] {
        return rig.picker().statusLabel()->text() == QStringLiteral("2 models available");
    }));
    std::lock_guard lock(rig.catalog->mutex);
    REQUIRE_FALSE(rig.catalog->requests.empty());
    CHECK(rig.catalog->requests.front() ==
          std::pair<std::string, std::string>{"openai", "alphabetical"});
}

// --- TestCleanupSettingsOwnership ---

// from
// tests/test_settings_models.py::TestCleanupSettingsOwnership::test_thinking_level_loads_saves_and_refreshes
TEST_CASE("test_thinking_level_loads_saves_and_refreshes", "[test_settings_models][widget]") {
    const std::string reasoning = GENERATE("off", "low", "medium", "high");
    CleanupRig rig(baseValues(
        {{"transcript_cleanup_model", "gpt-5-mini"}, {"transcript_cleanup_reasoning", reasoning}}));
    QComboBox* combo = rig.model().reasoningCombo();
    CHECK(combo->currentData().toString() == qs(reasoning));
    CHECK(rig.saved("transcript_cleanup_reasoning") == reasoning);

    combo->setCurrentIndex((combo->currentIndex() + 1) % combo->count());
    CHECK(rig.saved("transcript_cleanup_reasoning") ==
          fv::qt::toUtf8(combo->currentData().toString()));

    rig.settings().update(Json{{"transcript_cleanup_reasoning", reasoning}});
    rig.window().refresh();
    CHECK(combo->currentData().toString() == qs(reasoning));
}

// from
// tests/test_settings_models.py::TestCleanupSettingsOwnership::test_saving_cleanup_settings_preserves_text_model_keys
TEST_CASE("test_saving_cleanup_settings_preserves_text_model_keys",
          "[test_settings_models][widget]") {
    CleanupRig rig({{"transcript_cleanup_provider", "openrouter"},
                    {"transcript_cleanup_model", "provider/model-test"},
                    {"transcript_cleanup_reasoning", "high"},
                    {"transcript_cleanup_model_sort", "newest"}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    page.cleanupCheck()->toggle();

    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    CHECK(rig.saved("transcript_cleanup_model") == "provider/model-test");
    CHECK(rig.saved("transcript_cleanup_reasoning") == "high");
    CHECK(rig.saved("transcript_cleanup_model_sort") == "newest");
}

// from
// tests/test_settings_models.py::TestCleanupSettingsOwnership::test_cleanup_page_hosts_the_cleanup_model_picker
TEST_CASE("test_cleanup_page_hosts_the_cleanup_model_picker", "[test_settings_models][widget]") {
    CleanupRig rig;
    fv::CleanupPage& content = rig.builtCleanupPage();
    QWidget* page = rig.window().page(dest::cleanup);
    fv::TextModelPicker* picker = content.modelSection()->picker();
    CHECK(page->isAncestorOf(picker));
    CHECK(page->isAncestorOf(content.modelSection()->reasoningCombo()));
    CHECK(rig.window().control("cleanup_model_tile")->isAncestorOf(picker));
}

// --- TestNewTextProviders ---

// from
// tests/test_settings_models.py::TestNewTextProviders::test_independent_model_memory_survives_dialog_recreation
TEST_CASE("test_independent_model_memory_survives_dialog_recreation",
          "[test_settings_models][widget]") {
    Json saved;
    {
        CleanupRig rig(baseValues());
        rig.picker().setProvider(QStringLiteral("ollama"), QStringLiteral("llama3.2"));
        rig.model().activateTextModel(QStringLiteral("ollama"));
        CHECK(rig.saved("cleanup_model_memory") == Json{{"ollama", "llama3.2"}});
        saved = *rig.settings().snapshot();
    }
    CleanupRig reopened(saved);
    CHECK(reopened.picker().stagedModels().at(QStringLiteral("ollama")) ==
          QStringLiteral("llama3.2"));
}

// from
// tests/test_settings_models.py::TestNewTextProviders::test_unknown_go_model_cannot_be_assigned
TEST_CASE("test_unknown_go_model_cannot_be_assigned", "[test_settings_models][widget]") {
    CleanupRig rig(baseValues());
    fv::TextModelPicker& picker = rig.picker();
    picker.setProvider(QStringLiteral("opencode_go"));
    picker.setModels({QStringLiteral("glm-5.2"), QStringLiteral("future-model")});
    auto* items = qobject_cast<QStandardItemModel*>(picker.modelCombo()->model());
    REQUIRE(items != nullptr);
    CHECK_FALSE(items->item(1)->isEnabled());
    picker.modelCombo()->setCurrentText(QStringLiteral("future-model"));

    rig.model().activateTextModel(QStringLiteral("opencode_go"));

    CHECK(rig.saved("transcript_cleanup_provider") == "openai");
    CHECK(message(rig).contains(QStringLiteral("no supported API route")));
}

// from
// tests/test_settings_models.py::TestNewTextProviders::test_stale_ollama_catalog_result_is_discarded
TEST_CASE("test_stale_ollama_catalog_result_is_discarded", "[test_settings_models][widget]") {
    CleanupRig rig(baseValues());
    fv::CleanupModelSection& section = rig.model();
    // The request waits on an executor the test runs by hand.
    fv::ManualExecutor held;
    fv::Executor* io = std::exchange(rig.cleanup().services().io, &held);
    {
        std::lock_guard lock(rig.catalog->mutex);
        rig.catalog->models = {"old-model"};
    }
    section.fetchCatalogModels(QStringLiteral("ollama"));
    REQUIRE(section.catalogToken(QStringLiteral("ollama"), QStringLiteral("alphabetical")));

    section.invalidateTextCatalog(QStringLiteral("ollama"));
    held.run_all();
    processEvents(3);

    CHECK_FALSE(section.hasCachedCatalog(QStringLiteral("ollama"), QStringLiteral("alphabetical")));
    rig.cleanup().services().io = io;
}

// from
// tests/test_settings_models.py::TestNewTextProviders::test_failed_refresh_keeps_last_catalog_after_provider_switch
TEST_CASE("test_failed_refresh_keeps_last_catalog_after_provider_switch",
          "[test_settings_models][widget]") {
    CleanupRig rig(baseValues());
    fv::CleanupModelSection& section = rig.model();
    fv::TextModelPicker& picker = rig.picker();
    picker.setProvider(QStringLiteral("groq"));
    section.onTextModelsLoaded(QStringLiteral("groq"), QStringLiteral("alphabetical"),
                               {QStringLiteral("llama-3.3-70b-versatile")}, QString());
    picker.setProvider(QStringLiteral("ollama"));
    picker.setProvider(QStringLiteral("groq"));

    section.onTextModelsLoaded(QStringLiteral("groq"), QStringLiteral("alphabetical"), {},
                               QStringLiteral("offline"));

    CHECK(picker.modelCombo()->count() == 1);
    CHECK(picker.statusLabel()->text().contains(QStringLiteral("offline")));
}

// from
// tests/test_settings_models.py::TestNewTextProviders::test_running_local_servers_are_marked_and_offered_instead_of_a_missing_key
TEST_CASE("test_running_local_servers_are_marked_and_offered_instead_of_a_missing_key",
          "[test_settings_models][widget]") {
    CleanupRig rig(baseValues());
    fv::CleanupModelSection& section = rig.model();
    fv::TextModelPicker& picker = rig.picker();
    picker.setProvider(QStringLiteral("openrouter"));
    {
        std::lock_guard lock(rig.servers->mutex);
        rig.servers->found = {{"http://127.0.0.1:1234/v1", "lm_studio"},
                              {"http://127.0.0.1:8080/v1", "llama_cpp"}};
    }
    const auto itemText = [&picker](const char* id) {
        return picker.providerCombo()->itemText(
            picker.providerCombo()->findData(QString::fromUtf8(id)));
    };

    section.onDestinationShown();
    CHECK(
        pumpUntil([&] { return itemText("lm_studio").contains(QStringLiteral("Running")); }, 5000));
    section.onDestinationShown();

    {
        std::lock_guard lock(rig.servers->mutex);
        CHECK(rig.servers->probes.size() == 1); // once per Settings, no polling
        const std::vector<std::string>& probed = rig.servers->probes.front();
        CHECK(std::ranges::find(probed, "http://127.0.0.1:9931/v1") != probed.end());
        CHECK(std::ranges::find(probed, "http://127.0.0.1:8080/v1") != probed.end());
    }
    CHECK(itemText("lm_studio") == QStringLiteral(u"LM Studio · Running"));
    CHECK(itemText("llama_cpp") == QStringLiteral("llama.cpp"));
    CHECK(picker.provider() == QStringLiteral("openrouter"));
    CHECK(pumpUntil([&picker] {
        return picker.requirementLabel()->text() ==
               QStringLiteral(u"Key needed in Settings → App → API keys, or choose LM Studio, "
                              u"running on this computer");
    }));
    CHECK(picker.requirementLabel()->toolTip() ==
          QStringLiteral(u"Requires OpenRouter API key — add it in Settings → App → API keys"));
    picker.setProvider(QStringLiteral("llama_cpp"));
    CHECK(picker.providerCombo()->toolTip().contains(
        QStringLiteral("Answering at http://127.0.0.1:8080/v1")));
    picker.setProvider(QStringLiteral("lm_studio"));
    CHECK(picker.requirementLabel()->text() == QStringLiteral("No API key required"));
    CHECK(picker.providerCombo()->toolTip().contains(QStringLiteral("Running on this computer.")));
}

// from
// tests/test_settings_models.py::TestNewTextProviders::test_local_thinking_model_enables_the_thinking_level
TEST_CASE("test_local_thinking_model_enables_the_thinking_level",
          "[test_settings_models][widget]") {
    CleanupRig rig(baseValues());
    fv::CleanupModelSection& section = rig.model();
    rig.picker().setProvider(QStringLiteral("ollama"), QStringLiteral("qwen3"));
    section.onTextProviderChanged(QStringLiteral("ollama"));
    CHECK_FALSE(section.reasoningCombo()->isEnabled());

    fv::llm::TextModelSpec spec;
    spec.reasoning = true;
    spec.reasoning_format = "openai";
    spec.thinking_levels = Json{{"off", "none"}};
    fv::llm::remember_server_specs("http://localhost:11434/v1", {{"qwen3", spec}});
    section.onTextModelsLoaded(QStringLiteral("ollama"), QStringLiteral("alphabetical"),
                               {QStringLiteral("qwen3")}, QString());
    const bool enabled = section.reasoningCombo()->isEnabled();
    fv::llm::forget_server_specs();
    CHECK(enabled);
}

// _update_cleanup_reasoning_controls (settings_models.py:1500-1530): a saved level the model does
// not offer shows the nearest one it does, the note says what really runs, and filling the combo
// saves nothing.
TEST_CASE("a thinking level the model lacks shows the nearest and says what runs", "[widget]") {
    CleanupRig rig(baseValues({{"transcript_cleanup_reasoning", "off"}}));
    fv::CleanupModelSection& section = rig.model();
    rig.picker().setProvider(QStringLiteral("ollama"), QStringLiteral("qwen3"));

    fv::llm::TextModelSpec spec;
    spec.reasoning = true;
    spec.reasoning_format = "openai";
    spec.thinking_levels = Json{{"off", nullptr}};
    fv::llm::remember_server_specs("http://localhost:11434/v1", {{"qwen3", spec}});
    section.onTextModelsLoaded(QStringLiteral("ollama"), QStringLiteral("alphabetical"),
                               {QStringLiteral("qwen3")}, QString());
    const QString shown = section.reasoningCombo()->currentData().toString();
    const bool enabled = section.reasoningCombo()->isEnabled();
    const QString note = section.reasoningNote()->text();
    const QString tip = section.reasoningCombo()->toolTip();
    fv::llm::forget_server_specs();

    CHECK(enabled);
    CHECK(shown == QStringLiteral("low"));
    CHECK(note == QStringLiteral("How much the model reasons before it rewrites. Higher is slower. "
                                 "Your Off setting runs at Low here."));
    CHECK(tip == note);
    CHECK(rig.saved("transcript_cleanup_reasoning") == "off");
}

// from tests/test_settings_models.py::TestNewTextProviders::test_ollama_exposes_edit_but_not_delete
TEST_CASE("test_ollama_exposes_edit_but_not_delete", "[test_settings_models][widget]") {
    CleanupRig rig(baseValues());
    fv::TextModelPicker& picker = rig.picker();
    picker.setProvider(QStringLiteral("ollama"));
    CHECK(picker.editButton()->isEnabled());
    CHECK_FALSE(picker.deleteButton()->isEnabled());
    CHECK(picker.requirementLabel()->text() == QStringLiteral("No API key required"));
}

// --- test_luna_cleanup_picker.py ---

// from tests/test_luna_cleanup_picker.py::test_openai_picker_uses_luna_and_retains_explicit_model
TEST_CASE("test_openai_picker_uses_luna_and_retains_explicit_model",
          "[test_luna_cleanup_picker][widget]") {
    // One case: OpenWhisper's appearance switch (classic, Omarchy) is Linux's.
    fv::TextModelPicker picker;
    picker.setProvider(QStringLiteral("openai"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-6-luna"));
    picker.setProvider(QStringLiteral("openrouter"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
    picker.setProvider(QStringLiteral("openai"), QStringLiteral("gpt-4.1-mini"));
    picker.setProvider(QStringLiteral("openrouter"));
    picker.setProvider(QStringLiteral("openai"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-4.1-mini"));
}

// from
// tests/test_luna_cleanup_picker.py::test_saved_legacy_cleanup_selection_shows_luna_without_changing_provider
TEST_CASE("test_saved_legacy_cleanup_selection_shows_luna_without_changing_provider",
          "[test_luna_cleanup_picker][widget]") {
    const Json settings{{"transcript_cleanup_provider", "openai"},
                        {"transcript_cleanup_model", "gpt-4o-mini"}};
    fv::TextModelPicker picker;
    const QString provider = qs(fv::llm::resolve_transcript_cleanup_provider(settings));
    const QString model = qs(fv::llm::resolve_transcript_cleanup_model(settings));
    picker.setProvider(provider, model);
    picker.setActiveSelection(provider, model);
    CHECK(picker.provider() == QStringLiteral("openai"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-6-luna"));
    CHECK(settings["transcript_cleanup_model"] == "gpt-4o-mini");
}

// --- text_summary ---

TEST_CASE("text_summary names the endpoint and the model, or the default model", "[unit]") {
    const Json custom{{"text_llm_profiles", Json::array({kCustomProfile})}};
    CHECK(fv::CleanupModelSection::textSummary("openrouter", "openrouter/free", Json::object()) ==
          QStringLiteral(u"OpenRouter · openrouter/free"));
    CHECK(fv::CleanupModelSection::textSummary("custom_abcd1234", "", custom) ==
          QStringLiteral(u"LM Studio · default model"));
}

TEST_CASE("approved cleanup agents remember model choices and allow the default",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues("codex", "saved-codex"));
    rig.agentScan.state->models["codex"].push_back({"invalid model id", "Not an id"});
    rig.settings().update({{"fv_cleanup_agent_consent", "codex"},
                           {"cleanup_model_memory", {{"claude_code", "saved-claude"}}}});
    auto& section = rig.model();
    fv::Button* applyRunner = section.applyRunnerButton();
    REQUIRE(applyRunner != nullptr);

    // The saved model, missing from Codex's list, is added to it and shown.
    CHECK(rig.agents().modelCard()->isVisibleTo(&section));
    CHECK(rig.shownAgentModel() == QStringLiteral("saved-codex"));
    rig.chooseAgentModel(QStringLiteral("invalid model id"));
    CHECK(rig.saved("transcript_cleanup_model") == "saved-codex");
    CHECK(rig.shownAgentModel() == QStringLiteral("saved-codex"));
    rig.chooseAgentModel(QStringLiteral("gpt-b"));
    CHECK(rig.saved("transcript_cleanup_model") == "gpt-b");
    CHECK(rig.saved("cleanup_model_memory").at("codex") == "gpt-b");

    rig.answer = QMessageBox::Yes;
    const auto selectRunner = [&](const QString& id) {
        rig.clickTile(id);
        CHECK(applyRunner->isEnabled());
        applyRunner->click();
        CHECK(rig.agents().choice() == id);
        CHECK_FALSE(applyRunner->isEnabled());
    };
    selectRunner(QStringLiteral("claude_code"));
    CHECK(rig.shownAgentModel() == QStringLiteral("saved-claude"));
    CHECK(rig.saved("transcript_cleanup_model") == "saved-claude");
    // The agent's default.
    rig.chooseAgentModel(QString());
    CHECK(rig.shownAgentModel().isEmpty());
    CHECK(rig.saved("transcript_cleanup_model") == "");
    CHECK(rig.saved("cleanup_model_memory").at("claude_code") == "");

    selectRunner(QStringLiteral("codex"));
    CHECK(rig.shownAgentModel() == QStringLiteral("gpt-b"));
    CHECK(rig.saved("fv_cleanup_agent_consent") == "codex");
    selectRunner(QStringLiteral("claude_code"));
    CHECK(rig.shownAgentModel().isEmpty());

    // A model picked for a staged agent waits for Apply and its question.
    rig.clickTile(QStringLiteral("codex"));
    rig.chooseAgentModel(QStringLiteral("gpt-a"));
    CHECK(rig.saved("transcript_cleanup_provider") == "claude_code");
    CHECK(rig.saved("transcript_cleanup_model") == "");
    CHECK(rig.saved("cleanup_model_memory").at("codex") == "gpt-b");
    const auto asked = rig.asked.size();
    applyRunner->click();
    CHECK(rig.asked.size() == asked + 1);
    CHECK(rig.saved("transcript_cleanup_provider") == "codex");
    CHECK(rig.saved("transcript_cleanup_model") == "gpt-a");
    selectRunner(QStringLiteral("claude_code"));

    // An approval withdrawn behind the window: a model picked then is not saved.
    rig.settings().update({{"fv_cleanup_agent_consent", ""}});
    rig.chooseAgentModel(QStringLiteral("haiku"));
    CHECK(rig.saved("transcript_cleanup_model") == "");
    rig.window().refresh();
    CHECK(rig.saved("transcript_cleanup_model") == "");
    CHECK_FALSE(rig.picker().isHidden());
    CHECK(applyRunner->isEnabled());
}

TEST_CASE("the cleanup provider dropdown opens installed runner controls on demand",
          "[test_settings_models][widget]") {
    CleanupRig rig(textValues("openrouter", "chosen-endpoint-model"));
    auto& section = rig.model();
    CHECK(section.runnerControls()->isHidden());

    auto& picker = rig.picker();
    const auto endpoint = picker.provider();
    auto* combo = picker.providerCombo();
    const int action =
        combo->findData(QString::fromLatin1(fv::TextModelPicker::kInstalledAgentOption));
    REQUIRE(action >= 0);
    CHECK(combo->itemText(action) == QStringLiteral(u"Installed agent…"));
    combo->setCurrentIndex(action);
    CHECK(picker.provider() == endpoint);
    CHECK(combo->currentData().toString() == endpoint);
    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    CHECK(rig.asked.empty());

    fv::Button* apply = section.applyRunnerButton();
    REQUIRE(apply != nullptr);
    CHECK_FALSE(section.runnerControls()->isHidden());
    CHECK(rig.agents().choice() == builtin());
    CHECK_FALSE(apply->isEnabled());
    rig.clickTile(QStringLiteral("codex"));
    CHECK(rig.asked.empty());
    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    rig.answer = QMessageBox::No;
    apply->click();
    CHECK(rig.saved("transcript_cleanup_provider") == "openrouter");
    CHECK(rig.asked.size() == 1);
    CHECK_FALSE(fv::llm::cleanup_agent_authorized(*rig.settings().snapshot(), "codex"));
}

TEST_CASE("Codex cleanup shows cached model effort without saving the nearest level",
          "[test_settings_models][widget]") {
    Json values = textValues("codex", "gpt-a");
    values["fv_cleanup_agent_consent"] = "codex";
    values["transcript_cleanup_reasoning"] = "off";
    CleanupRig rig(values);
    rig.cleanup().services().codexAgentLevels = [](const std::string& model) {
        if (model == "gpt-a" || model.empty()) {
            return std::vector<std::string>{"low", "medium", "high", "xhigh", "max", "ultra"};
        }
        return std::vector<std::string>{"minimal", "medium"};
    };
    auto& section = rig.model();
    auto* combo = section.reasoningCombo();
    REQUIRE(combo != nullptr);
    REQUIRE(pumpUntil([&] { return combo->findData(QStringLiteral("ultra")) >= 0; }));
    CHECK(combo->itemText(combo->count() - 1) == QStringLiteral("Ultra"));
    CHECK(combo->currentData().toString() == QStringLiteral("low"));
    CHECK(section.reasoningNote()->text().contains(QStringLiteral("Your Off setting runs at Low")));
    CHECK(rig.saved("transcript_cleanup_reasoning") == "off");

    combo->setCurrentIndex(combo->findData(QStringLiteral("ultra")));
    CHECK(rig.saved("transcript_cleanup_reasoning") == "ultra");
    CHECK_FALSE(section.reasoningNote()->text().contains(QStringLiteral("runs at")));

    rig.chooseAgentModel(QStringLiteral("gpt-b"));
    REQUIRE(pumpUntil([&] { return combo->findData(QStringLiteral("minimal")) >= 0; }));
    CHECK(combo->currentData().toString() == QStringLiteral("medium"));
    CHECK(section.reasoningNote()->text().contains(
        QStringLiteral("Your Ultra setting runs at Medium")));
    CHECK(rig.saved("transcript_cleanup_reasoning") == "ultra");

    rig.chooseAgentModel(QString());
    REQUIRE(pumpUntil([&] { return combo->currentData().toString() == QStringLiteral("ultra"); }));
    CHECK(rig.saved("transcript_cleanup_model") == "");
    CHECK(rig.saved("transcript_cleanup_reasoning") == "ultra");
}
