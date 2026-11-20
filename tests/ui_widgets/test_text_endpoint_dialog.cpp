// The text-model endpoint editor: its three modes, what each shows, and what a save keeps.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

#include "base/json.h"
#include "llm/profiles.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/text_endpoint_dialog.h"

namespace {

using fv::Json;
using fv::TextEndpointDialog;
namespace llm = fv::llm;

void choose(QComboBox* combo, const char* data) {
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(data)));
}

llm::TextLLMProfile builtin(const char* id) {
    return *llm::get_profile(std::string_view(id), Json::object());
}

// The text of every label in the dialog's form, in order.
QStringList formLabels(const TextEndpointDialog& dialog) {
    QStringList labels;
    const auto* form = dialog.findChild<QFormLayout*>();
    for (int row = 0; row < form->rowCount(); ++row) {
        const QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole);
        labels << qobject_cast<QLabel*>(item->widget())->text();
    }
    return labels;
}

} // namespace

// from tests/test_text_endpoint_dialog.py::test_endpoint_editor_saves_and_restores_responses
TEST_CASE("endpoint editor saves and restores responses", "[test_text_endpoint_dialog][widget]") {
    TextEndpointDialog dialog;
    CHECK(dialog.protocolCombo()->currentData().toString() == QStringLiteral("chat"));
    dialog.nameEdit()->setText(QStringLiteral("Gateway"));
    dialog.urlEdit()->setText(QStringLiteral("https://gateway.test/v1/"));
    choose(dialog.protocolCombo(), "responses");
    dialog.save();
    REQUIRE(dialog.resultPayload().has_value());
    const TextEndpointDialog::Payload payload = *dialog.resultPayload();
    CHECK(payload.protocol == "responses");
    CHECK(payload.baseUrl == "https://gateway.test/v1");

    Json settings = Json::object();
    const llm::TextLLMProfile profile = llm::upsert_custom_profile(
        settings, payload.name, payload.baseUrl, payload.apiKeyEnv, payload.id, payload.protocol);
    TextEndpointDialog restored(profile);
    CHECK(restored.protocolCombo()->currentData().toString() == QStringLiteral("responses"));
    restored.save();
    REQUIRE(restored.resultPayload().has_value());
    CHECK(restored.resultPayload()->protocol == "responses");
}

// from
// tests/test_text_endpoint_dialog.py::test_local_preset_editor_changes_only_address_and_key_variable
TEST_CASE("local preset editor changes only address and key variable",
          "[test_text_endpoint_dialog][widget]") {
    TextEndpointDialog dialog(builtin("llama_cpp"));
    CHECK(dialog.windowTitle() == QStringLiteral("llama.cpp server"));
    CHECK(dialog.urlEdit()->text() == QStringLiteral("http://127.0.0.1:9931/v1"));
    CHECK(dialog.nameEdit()->parent() == nullptr);
    CHECK(dialog.protocolCombo()->parent() == nullptr);
    dialog.urlEdit()->setText(QStringLiteral("http://user:pw@127.0.0.1:8081"));
    dialog.save();
    CHECK_FALSE(dialog.resultPayload().has_value());
    CHECK(dialog.errorLabel()->text().contains(QStringLiteral("credentials")));
    dialog.urlEdit()->setText(QStringLiteral("http://127.0.0.1:8081"));
    dialog.envEdit()->setText(QStringLiteral("LLAMA_SERVER_KEY"));
    dialog.save();
    REQUIRE(dialog.resultPayload().has_value());
    const TextEndpointDialog::Payload& payload = *dialog.resultPayload();
    CHECK(payload.baseUrl == "http://127.0.0.1:8081/v1");
    CHECK(payload.apiKeyEnv == "LLAMA_SERVER_KEY");
    CHECK(payload.id == std::optional<std::string>("llama_cpp"));
    // The hidden name is the preset's own and still passes through validation.
    CHECK(payload.name == "llama.cpp");
    CHECK(payload.protocol == "chat");
}

TEST_CASE("each mode has its own title, heading, fields and button",
          "[test_text_endpoint_dialog][widget]") {
    SECTION("add") {
        TextEndpointDialog dialog;
        CHECK(dialog.mode() == TextEndpointDialog::Mode::Add);
        CHECK(dialog.windowTitle() == QStringLiteral("Add endpoint"));
        CHECK(dialog.findChild<QLabel*>(QStringLiteral("headerLabel"))->text() ==
              QStringLiteral("Add an OpenAI-compatible endpoint"));
        CHECK(formLabels(dialog) == QStringList{QStringLiteral("Name"), QStringLiteral("Base URL"),
                                                QStringLiteral("API"),
                                                QStringLiteral("API key variable")});
        CHECK(dialog.saveButton()->text() == QStringLiteral("Save endpoint"));
        CHECK(dialog.nameEdit()->placeholderText() == QStringLiteral("LM Studio"));
        CHECK(dialog.urlEdit()->placeholderText() == QStringLiteral("http://127.0.0.1:1234/v1"));
        CHECK(dialog.envEdit()->placeholderText() ==
              QStringLiteral("Optional, e.g. LMSTUDIO_API_KEY"));
        CHECK(dialog.protocolCombo()->itemText(0) == QStringLiteral("Chat Completions"));
        CHECK(dialog.protocolCombo()->itemText(1) == QStringLiteral("Responses"));
        CHECK(dialog.protocolCombo()->toolTip() ==
              QStringLiteral("Choose the API supported by your server."));
        CHECK(dialog.isModal());
        CHECK(dialog.minimumWidth() == 460);
    }
    SECTION("edit a custom endpoint") {
        Json settings = Json::object();
        const llm::TextLLMProfile profile = llm::upsert_custom_profile(
            settings, "Gateway", "https://gateway.test/v1", "GATEWAY_KEY");
        TextEndpointDialog dialog(profile);
        CHECK(dialog.mode() == TextEndpointDialog::Mode::EditCustom);
        CHECK(dialog.windowTitle() == QStringLiteral("Edit endpoint"));
        CHECK(dialog.findChild<QLabel*>(QStringLiteral("headerLabel"))->text() ==
              QStringLiteral("Edit OpenAI-compatible endpoint"));
        CHECK(dialog.nameEdit()->text() == QStringLiteral("Gateway"));
        CHECK(dialog.urlEdit()->text() == QStringLiteral("https://gateway.test/v1"));
        CHECK(dialog.envEdit()->text() == QStringLiteral("GATEWAY_KEY"));
        dialog.save();
        REQUIRE(dialog.resultPayload().has_value());
        // An edit carries the profile's id, so the host replaces it.
        CHECK(dialog.resultPayload()->id == std::optional<std::string>(profile.id));
    }
    SECTION("local preset") {
        TextEndpointDialog dialog(builtin("lm_studio"));
        CHECK(dialog.mode() == TextEndpointDialog::Mode::LocalPreset);
        CHECK(dialog.findChild<QLabel*>(QStringLiteral("headerLabel"))->text() ==
              QStringLiteral("LM Studio server"));
        CHECK(formLabels(dialog) ==
              QStringList{QStringLiteral("Base URL"), QStringLiteral("API key variable")});
        CHECK(dialog.saveButton()->text() == QStringLiteral("Save server"));
        CHECK(dialog.envEdit()->placeholderText() ==
              QStringLiteral("Optional, e.g. LM_STUDIO_API_KEY"));
        CHECK(dialog.findChild<QLabel*>(QStringLiteral("infoLabel"))
                  ->text()
                  .startsWith(QStringLiteral("Cleanup and meetings share this server.")));
    }
}

TEST_CASE("a refused field stays in the dialog as the error line",
          "[test_text_endpoint_dialog][widget]") {
    TextEndpointDialog dialog;
    // Each field is checked in order: name, address, key variable.
    dialog.save();
    CHECK(dialog.errorLabel()->text() == QStringLiteral("Endpoint name is required"));
    dialog.nameEdit()->setText(QStringLiteral("Gateway"));
    dialog.save();
    CHECK(dialog.errorLabel()->text() == QStringLiteral("Base URL is required"));
    dialog.urlEdit()->setText(QStringLiteral("ftp://gateway.test"));
    dialog.save();
    CHECK(dialog.errorLabel()->text() == QStringLiteral("Base URL must use http or https"));
    dialog.urlEdit()->setText(QStringLiteral("https://gateway.test/v1"));
    dialog.envEdit()->setText(QStringLiteral("not valid"));
    dialog.save();
    CHECK(dialog.errorLabel()->text().startsWith(QStringLiteral("API key variable must be")));
    CHECK_FALSE(dialog.resultPayload().has_value());
    CHECK(dialog.result() != QDialog::Accepted);

    dialog.envEdit()->setText(QStringLiteral("GATEWAY_KEY"));
    dialog.save();
    REQUIRE(dialog.resultPayload().has_value());
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.resultPayload()->name == "Gateway");
    CHECK(dialog.resultPayload()->apiKeyEnv == "GATEWAY_KEY");
    CHECK_FALSE(dialog.resultPayload()->id.has_value());
}

TEST_CASE("the preset still validates its hidden name", "[test_text_endpoint_dialog][widget]") {
    // A stored preset whose name no longer passes (blank here) cannot be saved, and the user
    // sees the reason although the field is not shown.
    llm::TextLLMProfile profile = builtin("ollama");
    profile.name = "";
    TextEndpointDialog dialog(profile);
    dialog.save();
    CHECK_FALSE(dialog.resultPayload().has_value());
    CHECK(dialog.errorLabel()->text() == QStringLiteral("Endpoint name is required"));
}

TEST_CASE("cancel rejects without a payload", "[test_text_endpoint_dialog][widget]") {
    TextEndpointDialog dialog;
    dialog.nameEdit()->setText(QStringLiteral("Gateway"));
    dialog.urlEdit()->setText(QStringLiteral("https://gateway.test/v1"));
    dialog.cancelButton()->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(dialog.resultPayload().has_value());
}
