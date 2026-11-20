// The endpoint and model picker of the language-model Settings sections, and the thinking-level
// combo's filler. The picker's copy and wording are replayed against OpenWhisper's
// (tests/golden/ui_qt.widgets.text_model_picker.json); the rest are the rules the Python widget
// documents, which tests/test_settings_models.py exercises through the Settings page (M4.12).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTest>

#include "base/json.h"
#include "golden/golden.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm/thinking_levels.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/thinking_combo.h"

namespace golden = fv::golden;
namespace llm = fv::llm;
using fv::Json;
using fv::TextModelPicker;

namespace {

const QString kOpenRouter = QStringLiteral("openrouter");

// One local server found answering at url.
std::map<QString, QString> running(const char* url, const char* server) {
    return {{QString::fromUtf8(url), QString::fromUtf8(server)}};
}

llm::TextLLMProfile addCustom(Json& settings, const char* name, const char* url,
                              const char* env = "") {
    return llm::upsert_custom_profile(settings, name, url, env);
}

void chooseProvider(TextModelPicker& picker, const char* id) {
    QComboBox* combo = picker.providerCombo();
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(id)));
}

// What a person's typing and Enter do to the model editor, as the Python tests drive them.
void typeFragment(TextModelPicker& picker, const QString& text) {
    QLineEdit* editor = picker.modelCombo()->lineEdit();
    editor->setText(text);
    QMetaObject::invokeMethod(editor, "textEdited", Q_ARG(QString, text));
}

void leaveEditor(TextModelPicker& picker) {
    QMetaObject::invokeMethod(picker.modelCombo()->lineEdit(), "editingFinished");
}

void pressEnter(TextModelPicker& picker) {
    QMetaObject::invokeMethod(picker.modelCombo()->lineEdit(), "returnPressed");
    leaveEditor(picker);
}

void pickFromList(TextModelPicker& picker, const QString& model) {
    picker.modelCombo()->setCurrentText(model);
    QMetaObject::invokeMethod(picker.modelCombo(), "textActivated", Q_ARG(QString, model));
}

std::vector<QString> comboData(const QComboBox* combo) {
    std::vector<QString> data;
    for (int index = 0; index < combo->count(); ++index) {
        data.push_back(combo->itemData(index).toString());
    }
    return data;
}

QStringList comboTexts(const QComboBox* combo) {
    QStringList texts;
    for (int index = 0; index < combo->count(); ++index) {
        texts << combo->itemText(index);
    }
    return texts;
}

llm::TextLLMProfile profileFrom(const Json& fields) {
    llm::TextLLMProfile profile;
    profile.id = fields.at("id").get<std::string>();
    profile.name = fields.at("name").get<std::string>();
    profile.kind = fields.at("kind").get<std::string>();
    if (!fields.at("base_url").is_null()) {
        profile.base_url = fields.at("base_url").get<std::string>();
    }
    profile.api_key_env = fields.at("api_key_env").get<std::string>();
    profile.builtin = fields.at("builtin").get<bool>();
    return profile;
}

std::vector<llm::TextLLMProfile> profilesFrom(const Json& list) {
    std::vector<llm::TextLLMProfile> profiles;
    for (const Json& fields : list) {
        profiles.push_back(profileFrom(fields));
    }
    return profiles;
}

std::vector<std::pair<QString, QString>> runningFrom(const Json& list) {
    std::vector<std::pair<QString, QString>> running;
    for (const Json& pair : list) {
        running.emplace_back(QString::fromStdString(pair.at(0).get<std::string>()),
                             QString::fromStdString(pair.at(1).get<std::string>()));
    }
    return running;
}

QString str(const Json& value) {
    return QString::fromStdString(value.get<std::string>());
}

QImage pixels(const QPixmap& pixmap) {
    return pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
}

} // namespace

// The copy and decision tables, against OpenWhisper's own.

TEST_CASE("endpoint descriptions replay _DESCRIPTIONS", "[golden]") {
    golden::for_each("ui_qt.widgets.text_model_picker", "_DESCRIPTIONS", [](const golden::Case& c) {
        const Json table = c.result<Json>();
        CHECK(table.size() == llm::kBuiltinKinds.size());
        for (const auto& [id, line] : table.items()) {
            INFO(id);
            llm::TextLLMProfile profile;
            profile.id = id;
            profile.kind = id;
            profile.name = id;
            profile.builtin = true;
            CHECK(fv::textEndpointDescription(profile, {}) ==
                  QString::fromStdString(line.get<std::string>()));
        }
    });
}

TEST_CASE("sort options replay _SORT_OPTIONS", "[golden]") {
    golden::for_each("ui_qt.widgets.text_model_picker", "_SORT_OPTIONS", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        const auto& options = TextModelPicker::sortOptions();
        REQUIRE(expected.size() == options.size());
        TextModelPicker picker;
        for (std::size_t index = 0; index < options.size(); ++index) {
            INFO(index);
            CHECK(options[index].first == str(expected.at(index).at(0)));
            CHECK(options[index].second == str(expected.at(index).at(1)));
            // The combo carries the same pairs in the same order.
            CHECK(picker.sortCombo()->itemText(static_cast<int>(index)) == options[index].first);
            CHECK(picker.sortCombo()->itemData(static_cast<int>(index)).toString() ==
                  options[index].second);
        }
        CHECK(picker.sortCombo()->count() == static_cast<int>(options.size()));
    });
}

TEST_CASE("endpoint description replays _description", "[golden]") {
    golden::for_each("ui_qt.widgets.text_model_picker", "_description", [](const golden::Case& c) {
        INFO(c.label());
        const auto profile = profileFrom(c.arg<Json>(0));
        CHECK(fv::textEndpointDescription(profile, runningFrom(c.arg<Json>(1))) ==
              str(c.result<Json>()));
    });
}

TEST_CASE("endpoint combo rows replay _reload_provider_combo", "[golden]") {
    golden::for_each("ui_qt.widgets.text_model_picker", "_reload_provider_combo",
                     [](const golden::Case& c) {
                         INFO(c.label());
                         TextModelPicker picker;
                         picker.setProfiles(profilesFrom(c.arg<Json>(0)));
                         picker.setRunningServers(runningFrom(c.arg<Json>(1)));
                         const Json rows = c.result<Json>();
                         const QComboBox* combo = picker.providerCombo();
                         REQUIRE(combo->count() == static_cast<int>(rows.size()));
                         for (int index = 0; index < combo->count(); ++index) {
                             INFO(index);
                             // Row [icon file, label, data, tooltip]: a QIcon does not say which
                             // file it came from, so the icon is checked by the tests that look at
                             // the pixels.
                             CHECK(combo->itemText(index) == str(rows.at(index).at(1)));
                             CHECK(combo->itemData(index).toString() == str(rows.at(index).at(2)));
                             CHECK(combo->itemData(index, Qt::ToolTipRole).toString() ==
                                   str(rows.at(index).at(3)));
                             CHECK_FALSE(combo->itemIcon(index).isNull());
                         }
                     });
}

TEST_CASE("credential line replays _update_credential_status", "[golden]") {
    golden::for_each(
        "ui_qt.widgets.text_model_picker", "_update_credential_status", [](const golden::Case& c) {
            INFO(c.label());
            const Json profiles = c.arg<Json>(0);
            const std::string provider = c.arg<std::string>(1);
            const Json status = c.arg<Json>(2);
            TextModelPicker picker;
            picker.setProfiles(profilesFrom(profiles));
            picker.setRunningServers(runningFrom(c.arg<Json>(3)));
            picker.setProvider(QString::fromStdString(provider));
            const auto profile = picker.currentProfile();
            REQUIRE(profile.has_value());
            picker.setCredentialStatus(str(status.at(1)), status.at(0).get<bool>(),
                                       QString::fromStdString(profile->base_url.value_or("")));
            const Json expected = c.result<Json>();
            CHECK(picker.requirementLabel()->text() == str(expected.at("text")));
            CHECK(picker.requirementLabel()->toolTip() == str(expected.at("tooltip")));
            CHECK(picker.requirementLabel()->property("available").toBool() ==
                  expected.at("available").get<bool>());
            const QString icon = str(expected.at("icon"));
            CHECK(pixels(picker.credentialIcon()->pixmap()) ==
                  pixels(fv::ui::designIcon(icon).pixmap(16, 16)));
        });
}

TEST_CASE("active badge replays _update_active_badge", "[golden]") {
    golden::for_each(
        "ui_qt.widgets.text_model_picker", "_update_active_badge", [](const golden::Case& c) {
            INFO(c.label());
            TextModelPicker picker;
            picker.setProfiles(profilesFrom(c.arg<Json>(0)));
            picker.setProvider(QString::fromStdString(c.arg<std::string>(1)));
            picker.modelCombo()->setCurrentText(QString::fromStdString(c.arg<std::string>(4)));
            picker.setActiveSelection(QString::fromStdString(c.arg<std::string>(2)),
                                      QString::fromStdString(c.arg<std::string>(3)));
            const Json expected = c.result<Json>();
            CHECK(picker.modelCombo()->badgeText() == str(expected.at("badge")));
            CHECK(picker.modelCombo()->toolTip() == str(expected.at("tooltip")));
        });
}

// The widget's structure.

TEST_CASE("the picker is one column of labeled fields", "[widget]") {
    TextModelPicker picker;
    CHECK(picker.objectName() == QStringLiteral("textModelPicker"));
    CHECK(picker.provider() == QStringLiteral("openai"));
    CHECK(picker.providerCombo()->objectName() == QStringLiteral("textModelProviderCombo"));
    CHECK(picker.modelCombo()->objectName() == QStringLiteral("textModelCombo"));
    CHECK(picker.sortCombo()->objectName() == QStringLiteral("textModelSort"));
    CHECK(picker.statusLabel()->objectName() == QStringLiteral("textModelStatus"));
    CHECK(picker.editButton()->objectName() == QStringLiteral("textEndpointEditButton"));
    CHECK(picker.deleteButton()->objectName() == QStringLiteral("textEndpointDeleteButton"));
    CHECK(picker.providerCombo()->minimumHeight() == 40);
    CHECK(picker.modelCombo()->minimumHeight() == 40);
    CHECK(picker.sortCombo()->minimumHeight() == 26);
    CHECK(picker.sortCombo()->maximumHeight() == 26);
    CHECK(picker.editButton()->maximumHeight() == 28);
    CHECK(picker.refreshButton()->maximumHeight() == 40);
    CHECK(picker.refreshButton()->toolTip() ==
          QStringLiteral("Reload the selected provider's catalog"));
    // The status line starts as "Loading models…" whatever idle text the host gave: showing an
    // endpoint begins a load.
    CHECK(picker.statusLabel()->text() == QStringLiteral(u"Loading models…"));
    CHECK(TextModelPicker(QStringLiteral("idle")).statusLabel()->text() ==
          QStringLiteral(u"Loading models…"));
}

TEST_CASE("the endpoint combo lists every profile, then Custom", "[widget]") {
    Json settings = Json::object();
    addCustom(settings, "Gateway", "https://gateway.test/v1", "GATEWAY_KEY");
    TextModelPicker picker;
    picker.setProfiles(llm::list_profiles(settings));
    const std::vector<QString> data = comboData(picker.providerCombo());
    REQUIRE(data.size() == llm::kBuiltinKinds.size() + 2);
    for (std::size_t index = 0; index < llm::kBuiltinKinds.size(); ++index) {
        CHECK(data[index].toStdString() == llm::kBuiltinKinds[index]);
    }
    CHECK(data[llm::kBuiltinKinds.size()].startsWith(QStringLiteral("custom_")));
    CHECK(data.back() == QStringLiteral("__custom__"));
    CHECK(picker.providerCombo()->itemText(picker.providerCombo()->count() - 1) ==
          QStringLiteral(u"Custom…"));
    CHECK(picker.providerCombo()
              ->itemData(picker.providerCombo()->count() - 1, Qt::ToolTipRole)
              .toString() ==
          QStringLiteral(u"Custom — add another OpenAI-compatible server or gateway"));
    // A custom endpoint is any server that speaks the Chat Completions API.
    CHECK(picker.providerCombo()
              ->itemData(picker.providerCombo()->count() - 2, Qt::ToolTipRole)
              .toString() ==
          QStringLiteral(u"Gateway — Any server that speaks the OpenAI Chat Completions API."));
}

TEST_CASE("the shown endpoint's tooltip names it", "[widget]") {
    TextModelPicker picker;
    CHECK(picker.providerCombo()->toolTip() ==
          QStringLiteral(u"OpenAI — Direct access to OpenAI chat and reasoning models."));
    picker.setProvider(QStringLiteral("groq"));
    CHECK(picker.providerCombo()->toolTip() ==
          QStringLiteral(u"Groq — Cloud text models served by Groq."));
}

TEST_CASE("an unknown endpoint id is ignored", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("ollama"));
    picker.setProvider(QStringLiteral("custom_gone"));
    CHECK(picker.provider() == QStringLiteral("ollama"));
    CHECK(picker.providerCombo()->currentData().toString() == QStringLiteral("ollama"));
}

// Staged models, and typed fragments.

TEST_CASE("each endpoint starts at its own default model", "[widget]") {
    Json settings = Json::object();
    addCustom(settings, "Gateway", "https://gateway.test/v1");
    TextModelPicker picker;
    picker.setProfiles(llm::list_profiles(settings));
    const auto& staged = picker.stagedModels();
    CHECK(staged.at(QStringLiteral("openrouter")) == QStringLiteral("openrouter/free"));
    CHECK(staged.at(QStringLiteral("openai")) == QStringLiteral("gpt-6-luna"));
    CHECK(staged.at(QStringLiteral("ollama")).isEmpty());
    const QString custom = QString::fromStdString(llm::list_custom_profiles(settings).front().id);
    CHECK(staged.at(custom).isEmpty());
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-6-luna"));
    picker.setProvider(QStringLiteral("openrouter"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
}

TEST_CASE("setProvider with a model stages it for that endpoint", "[widget]") {
    TextModelPicker picker;
    QSignalSpy changed(&picker, &TextModelPicker::providerChanged);
    picker.setProvider(QStringLiteral("ollama"), QStringLiteral("llama3.2"));
    CHECK(picker.stagedModels().at(QStringLiteral("ollama")) == QStringLiteral("llama3.2"));
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("llama3.2"));
    // A blank model stages nothing, and the host's own call is never echoed back.
    picker.setProvider(QStringLiteral("ollama"), QStringLiteral(""));
    CHECK(picker.stagedModels().at(QStringLiteral("ollama")) == QStringLiteral("llama3.2"));
    CHECK(changed.count() == 0);
}

TEST_CASE("an uncommitted typed fragment is dropped when focus leaves", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    picker.setModels(
        {QStringLiteral("openrouter/free"), QStringLiteral("anthropic/claude-3-haiku")});
    picker.setActiveSelection(kOpenRouter, QStringLiteral("openrouter/free"));
    QSignalSpy activations(&picker, &TextModelPicker::activationRequested);
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));

    typeFragment(picker, QStringLiteral("clau"));
    leaveEditor(picker);

    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
    CHECK(picker.stagedModels().at(kOpenRouter) == QStringLiteral("openrouter/free"));
    CHECK(activations.count() == 0);
}

TEST_CASE("a typed fragment does not follow the user to another endpoint", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    typeFragment(picker, QStringLiteral("clau"));
    chooseProvider(picker, "openai");
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-6-luna"));
    // The next focus-out is not treated as the old fragment's.
    leaveEditor(picker);
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gpt-6-luna"));
    chooseProvider(picker, "openrouter");
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
}

TEST_CASE("typing real keys then leaving also reverts", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    picker.setModels(
        {QStringLiteral("openrouter/free"), QStringLiteral("anthropic/claude-3-haiku")});
    picker.show();
    picker.modelCombo()->lineEdit()->setFocus();
    QApplication::processEvents();
    QTest::keyClicks(picker.modelCombo()->lineEdit(), QStringLiteral("cla"));
    QApplication::processEvents();
    picker.modelCombo()->hidePopup();
    leaveEditor(picker);
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
}

TEST_CASE("Enter or a list pick requests activation only for another model", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    picker.setModels(
        {QStringLiteral("openrouter/free"), QStringLiteral("anthropic/claude-3-haiku")});
    picker.setActiveSelection(kOpenRouter, QStringLiteral("openrouter/free"));
    QSignalSpy activations(&picker, &TextModelPicker::activationRequested);

    // The model in use again: nothing to assign.
    pressEnter(picker);
    pickFromList(picker, QStringLiteral("openrouter/free"));
    CHECK(activations.count() == 0);

    pickFromList(picker, QStringLiteral("anthropic/claude-3-haiku"));
    REQUIRE(activations.count() == 1);
    CHECK(activations.takeFirst().at(0).toString() == kOpenRouter);
    // The pick is staged at once; the badge follows the host's saved assignment.
    CHECK(picker.stagedModels().at(kOpenRouter) == QStringLiteral("anthropic/claude-3-haiku"));
    CHECK(picker.modelCombo()->badgeText().isEmpty());
    picker.setActiveSelection(kOpenRouter, QStringLiteral("anthropic/claude-3-haiku"));
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));

    // A typed id confirmed with Enter is a pick too.
    typeFragment(picker, QStringLiteral("typed/model"));
    pressEnter(picker);
    REQUIRE(activations.count() == 1);
    CHECK(picker.stagedModels().at(kOpenRouter) == QStringLiteral("typed/model"));
    // Another endpoint than the one in use is a different assignment, whatever the model.
    chooseProvider(picker, "openai");
    picker.modelCombo()->setCurrentText(QStringLiteral("typed/model"));
    pressEnter(picker);
    REQUIRE(activations.count() == 2);
    CHECK(activations.last().at(0).toString() == QStringLiteral("openai"));
}

TEST_CASE("an empty editor reverts instead of activating", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    QSignalSpy activations(&picker, &TextModelPicker::activationRequested);
    typeFragment(picker, QStringLiteral("   "));
    pressEnter(picker);
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
    CHECK(activations.count() == 0);
}

TEST_CASE("an invalid model is listed disabled with the reason", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("opencode_go"));
    const std::vector<std::string> routed = llm::catalog_models("opencode_go");
    REQUIRE_FALSE(routed.empty());
    const QString good = QString::fromStdString(routed.front());
    picker.setModels({good, QStringLiteral("not-a-model")});
    const auto* model = qobject_cast<const QStandardItemModel*>(picker.modelCombo()->model());
    REQUIRE(model != nullptr);
    CHECK(model->item(0)->isEnabled());
    CHECK(model->item(0)->toolTip().isEmpty());
    CHECK_FALSE(model->item(1)->isEnabled());
    CHECK(model->item(1)->toolTip() ==
          QStringLiteral("not-a-model has no supported API route for OpenCode Go. Choose a "
                         "supported model."));
}

TEST_CASE("typing an unsupported model says why and assigns nothing", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("opencode_go"));
    QSignalSpy activations(&picker, &TextModelPicker::activationRequested);
    typeFragment(picker, QStringLiteral("not-a-model"));
    pressEnter(picker);
    CHECK(activations.count() == 0);
    CHECK(picker.statusLabel()->text() ==
          QStringLiteral("not-a-model has no supported API route for OpenCode Go. Choose a "
                         "supported model."));
    CHECK(picker.stagedModels().at(QStringLiteral("opencode_go")) != QStringLiteral("not-a-model"));
}

TEST_CASE("the Active badge marks only the saved assignment", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    picker.setModels(
        {QStringLiteral("openrouter/free"), QStringLiteral("anthropic/claude-3-haiku")});
    CHECK(picker.modelCombo()->badgeText().isEmpty());
    picker.setActiveSelection(kOpenRouter, QStringLiteral("openrouter/free"));
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
    CHECK(picker.modelCombo()->toolTip() ==
          QStringLiteral(u"OpenRouter · openrouter/free is the model in use."));
    // Browsing another model of the same endpoint, and another endpoint: no badge, and the
    // tooltip names the real assignment.
    picker.modelCombo()->setCurrentText(QStringLiteral("anthropic/claude-3-haiku"));
    CHECK(picker.modelCombo()->badgeText().isEmpty());
    chooseProvider(picker, "openai");
    CHECK(picker.modelCombo()->badgeText().isEmpty());
    CHECK(picker.modelCombo()->toolTip() ==
          QStringLiteral(u"Choose from the catalog or type a model id — the choice is saved "
                         u"immediately. In use now: OpenRouter · openrouter/free."));
    // The assignment stages its model, so coming back shows it.
    chooseProvider(picker, "openrouter");
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("openrouter/free"));
    CHECK(picker.modelCombo()->badgeText() == QStringLiteral("Active"));
}

TEST_CASE("setModels keeps the staged model and signals nothing", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    picker.setStagedModel(kOpenRouter, QStringLiteral("anthropic/claude-3-haiku"));
    QSignalSpy activations(&picker, &TextModelPicker::activationRequested);
    QSignalSpy texts(picker.modelCombo(), &QComboBox::currentTextChanged);
    // A catalog the size OpenRouter's is: one rebuild, the staged model chosen at the end.
    QStringList models;
    for (int index = 0; index < 400; ++index) {
        models << QStringLiteral("vendor/model-%1").arg(index, 3, 10, QLatin1Char('0'));
    }
    models << QStringLiteral("anthropic/claude-3-haiku");
    picker.setModels(models);
    CHECK(picker.modelCombo()->count() == 401);
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("anthropic/claude-3-haiku"));
    CHECK(activations.count() == 0);
    CHECK(texts.count() == 0);
    // The list narrows by filtering the view, never by rebuilding items.
    const int rows = picker.modelCombo()->model()->rowCount();
    picker.show();
    picker.modelCombo()->showPopup();
    typeFragment(picker, QStringLiteral("model-39"));
    CHECK(picker.modelCombo()->model()->rowCount() == rows);
    picker.modelCombo()->hidePopup();
}

TEST_CASE("setLoading disables Refresh and says so", "[widget]") {
    TextModelPicker picker;
    picker.setStatus(QStringLiteral("2 models available"));
    picker.setLoading(true);
    CHECK_FALSE(picker.refreshButton()->isEnabled());
    CHECK(picker.statusLabel()->text() == QStringLiteral(u"Loading models…"));
    picker.setStatus(QStringLiteral("2 models available"));
    picker.setLoading(false);
    CHECK(picker.refreshButton()->isEnabled());
    // Finishing a load leaves the host's own status line alone.
    CHECK(picker.statusLabel()->text() == QStringLiteral("2 models available"));
}

TEST_CASE("Refresh asks for the shown endpoint's catalog", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("groq"));
    QSignalSpy refreshes(&picker, &TextModelPicker::refreshRequested);
    picker.refreshButton()->click();
    REQUIRE(refreshes.count() == 1);
    CHECK(refreshes.takeFirst().at(0).toString() == QStringLiteral("groq"));
}

// Sorting.

TEST_CASE("the sort combo shows only for OpenRouter", "[widget]") {
    TextModelPicker picker;
    CHECK_FALSE(picker.sortCombo()->isVisibleTo(&picker));
    CHECK_FALSE(picker.sortLabel()->isVisibleTo(&picker));
    picker.setProvider(kOpenRouter);
    CHECK(picker.sortCombo()->isVisibleTo(&picker));
    CHECK(picker.sortLabel()->isVisibleTo(&picker));
    CHECK(picker.sortLabel()->text() == QStringLiteral("Sort catalog"));
    picker.setProvider(QStringLiteral("groq"));
    CHECK_FALSE(picker.sortCombo()->isVisibleTo(&picker));
}

TEST_CASE("the catalog order is alphabetical away from OpenRouter", "[widget]") {
    TextModelPicker picker;
    QSignalSpy sorts(&picker, &TextModelPicker::sortChanged);
    picker.setProvider(kOpenRouter);
    CHECK(picker.currentSort() == QStringLiteral("alphabetical"));
    // Selecting a saved order does not ask for a refresh.
    picker.setSort(QStringLiteral("newest"));
    CHECK(picker.currentSort() == QStringLiteral("newest"));
    CHECK(sorts.count() == 0);
    // An unknown saved order falls back to the first.
    picker.setSort(QStringLiteral("nonsense"));
    CHECK(picker.currentSort() == QStringLiteral("alphabetical"));
    picker.setSort(QStringLiteral("newest"));

    // A person's change reports it for the shown endpoint.
    picker.sortCombo()->setCurrentIndex(
        picker.sortCombo()->findData(QStringLiteral("most-popular")));
    REQUIRE(sorts.count() == 1);
    CHECK(sorts.takeFirst().at(0).toString() == kOpenRouter);
    CHECK(picker.currentSort() == QStringLiteral("most-popular"));

    picker.setProvider(QStringLiteral("openai"));
    CHECK(picker.currentSort() == QStringLiteral("alphabetical"));
}

// The Custom entry, Edit and Delete.

TEST_CASE("Custom restores the previous selection and asks to add an endpoint", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("groq"));
    QSignalSpy adds(&picker, &TextModelPicker::addEndpointRequested);
    QSignalSpy changes(&picker, &TextModelPicker::providerChanged);
    chooseProvider(picker, "__custom__");
    CHECK(adds.count() == 1);
    CHECK(changes.count() == 0);
    CHECK(picker.provider() == QStringLiteral("groq"));
    CHECK(picker.providerCombo()->currentData().toString() == QStringLiteral("groq"));
}

TEST_CASE("choosing an endpoint reports it once", "[widget]") {
    TextModelPicker picker;
    QSignalSpy changes(&picker, &TextModelPicker::providerChanged);
    chooseProvider(picker, "ollama");
    REQUIRE(changes.count() == 1);
    CHECK(changes.takeFirst().at(0).toString() == QStringLiteral("ollama"));
    CHECK(picker.provider() == QStringLiteral("ollama"));
    CHECK(picker.statusLabel()->text() == QStringLiteral(u"Loading models…"));
}

TEST_CASE("Edit is enabled for custom endpoints and local servers, Delete for custom only",
          "[widget]") {
    Json settings = Json::object();
    const llm::TextLLMProfile gateway = addCustom(settings, "Gateway", "https://gateway.test/v1");
    TextModelPicker picker;
    picker.setProfiles(llm::list_profiles(settings));
    QSignalSpy edits(&picker, &TextModelPicker::editEndpointRequested);
    QSignalSpy deletes(&picker, &TextModelPicker::deleteEndpointRequested);

    for (const char* id : {"openai", "openrouter", "groq", "opencode_go", "opencode_zen"}) {
        picker.setProvider(QString::fromUtf8(id));
        INFO(id);
        CHECK_FALSE(picker.editButton()->isEnabled());
        CHECK_FALSE(picker.deleteButton()->isEnabled());
    }
    for (const auto& server : llm::kLocalServers) {
        picker.setProvider(QString::fromUtf8(server.id.data(), server.id.size()));
        INFO(server.id);
        CHECK(picker.editButton()->isEnabled());
        CHECK_FALSE(picker.deleteButton()->isEnabled());
    }
    picker.setProvider(QStringLiteral("ollama"));
    picker.editButton()->click();
    picker.deleteButton()->click();
    CHECK(edits.count() == 1);
    CHECK(deletes.count() == 0);

    picker.setProvider(QString::fromStdString(gateway.id));
    CHECK(picker.editButton()->isEnabled());
    CHECK(picker.deleteButton()->isEnabled());
    picker.editButton()->click();
    picker.deleteButton()->click();
    REQUIRE(edits.count() == 2);
    REQUIRE(deletes.count() == 1);
    CHECK(edits.last().at(0).toString().toStdString() == gateway.id);
    CHECK(deletes.first().at(0).toString().toStdString() == gateway.id);
}

TEST_CASE("replacing the profile list keeps the shown endpoint when it is still listed",
          "[widget]") {
    Json settings = Json::object();
    const llm::TextLLMProfile gateway = addCustom(settings, "Gateway", "https://gateway.test/v1");
    TextModelPicker picker;
    picker.setProfiles(llm::list_profiles(settings));
    picker.setProvider(QString::fromStdString(gateway.id));
    picker.setStagedModel(QString::fromStdString(gateway.id), QStringLiteral("gateway-model"));

    // Renamed: still shown, with its model.
    Json renamed = settings;
    llm::upsert_custom_profile(renamed, "Gateway 2", "https://gateway.test/v1", "", gateway.id);
    picker.setProfiles(llm::list_profiles(renamed));
    CHECK(picker.provider().toStdString() == gateway.id);
    CHECK(picker.modelCombo()->currentText() == QStringLiteral("gateway-model"));
    CHECK(picker.providerCombo()->currentText() == QStringLiteral("Gateway 2"));

    // Deleted: the first endpoint is shown.
    picker.setProfiles(llm::list_profiles(Json::object()));
    CHECK(picker.provider() == QStringLiteral("openai"));
    CHECK(picker.providerCombo()->findData(QString::fromStdString(gateway.id)) == -1);

    // An empty list means the built-in endpoints.
    picker.setProfiles({});
    CHECK(picker.providerCombo()->count() == static_cast<int>(llm::kBuiltinKinds.size()) + 1);
}

// The credential row.

TEST_CASE("an endpoint that needs no key says so at once", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("vllm"));
    CHECK(picker.requirementLabel()->text() == QStringLiteral("No API key required"));
    CHECK(picker.requirementLabel()->property("available").toBool());
    CHECK(picker.urlLabel()->toolTip() == QStringLiteral("http://127.0.0.1:8000/v1"));
    CHECK(picker.urlLabel()->text() == QStringLiteral("http://127.0.0.1:8000/v1"));
}

TEST_CASE("a keyed endpoint shows nothing until the host answers", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    CHECK(picker.requirementLabel()->text().isEmpty());
    CHECK_FALSE(picker.requirementLabel()->property("available").toBool());
    // A running local server does not turn the blank line into a warning.
    picker.setRunningServers(running("http://localhost:11434/v1", "ollama"));
    CHECK(picker.requirementLabel()->text().isEmpty());

    picker.setCredentialStatus(QStringLiteral("OpenRouter API key found"), true,
                               QStringLiteral("https://openrouter.ai/api/v1"));
    CHECK(picker.requirementLabel()->text() == QStringLiteral("OpenRouter API key found"));
    CHECK(picker.requirementLabel()->property("available").toBool());
    CHECK(pixels(picker.credentialIcon()->pixmap()) ==
          pixels(fv::ui::designIcon(QStringLiteral("check-green.svg")).pixmap(16, 16)));
}

TEST_CASE("a missing key points at Settings, or at the server running here", "[widget]") {
    const QString missing =
        QStringLiteral(u"Requires Groq API key — add it in Settings → App → API keys");
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("groq"));
    picker.setCredentialStatus(missing, false, QStringLiteral("https://api.groq.com/openai/v1"));
    CHECK(picker.requirementLabel()->text() ==
          QStringLiteral(u"Key needed in Settings → App → API keys"));
    CHECK(picker.requirementLabel()->toolTip() == missing);
    CHECK_FALSE(picker.requirementLabel()->property("available").toBool());
    CHECK(pixels(picker.credentialIcon()->pixmap()) ==
          pixels(fv::ui::designIcon(QStringLiteral("info-warning.svg")).pixmap(16, 16)));

    picker.setRunningServers(running("http://127.0.0.1:1234/v1", "lm_studio"));
    CHECK(picker.requirementLabel()->text() ==
          QStringLiteral(u"Key needed in Settings → App → API keys, or choose LM Studio, running "
                         u"on this computer"));
    // The tooltip keeps credential_status's own sentence.
    CHECK(picker.requirementLabel()->toolTip() == missing);
}

TEST_CASE("OpenAI and OpenRouter keep the status sentence for a missing key", "[widget]") {
    const QString missing =
        QStringLiteral(u"Requires OpenAI API key — add it in Settings → App → API keys");
    TextModelPicker picker;
    picker.setCredentialStatus(missing, false, QString());
    CHECK(picker.requirementLabel()->text() == missing);
}

TEST_CASE("a credential answer for another endpoint is stale", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(kOpenRouter);
    // The store was read for OpenAI, which was shown a moment ago and has no address of its own.
    picker.setCredentialStatus(QStringLiteral("OpenAI API key found"), true, QString());
    CHECK(picker.requirementLabel()->text().isEmpty());
    // Nor does an answer for another address count.
    picker.setCredentialStatus(QStringLiteral("Groq API key found"), true,
                               QStringLiteral("https://api.groq.com/openai/v1"));
    CHECK(picker.requirementLabel()->text().isEmpty());

    picker.setCredentialStatus(QStringLiteral("OpenRouter API key found"), true,
                               QStringLiteral("https://openrouter.ai/api/v1"));
    CHECK(picker.requirementLabel()->text() == QStringLiteral("OpenRouter API key found"));
    // Showing another endpoint drops the answer held for this one.
    picker.setProvider(QStringLiteral("groq"));
    CHECK(picker.requirementLabel()->text().isEmpty());
    picker.setProvider(kOpenRouter);
    CHECK(picker.requirementLabel()->text().isEmpty());
    // OpenAI's own answer, for its empty address, is taken while OpenAI is shown.
    picker.setProvider(QStringLiteral("openai"));
    picker.setCredentialStatus(QStringLiteral("OpenAI API key found"), true, QString());
    CHECK(picker.requirementLabel()->text() == QStringLiteral("OpenAI API key found"));
}

TEST_CASE("the address elides in the middle at 220 px and keeps the full URL", "[widget]") {
    Json settings = Json::object();
    const llm::TextLLMProfile gateway =
        addCustom(settings, "Gateway",
                  "https://a-very-long-gateway-host.example.internal/openai/compat/v1/chat");
    TextModelPicker picker;
    picker.setProfiles(llm::list_profiles(settings));
    picker.setProvider(QString::fromStdString(gateway.id));
    const QString full = QString::fromStdString(*gateway.base_url);
    CHECK(picker.urlLabel()->toolTip() == full);
    CHECK(picker.urlLabel()->text() != full);
    CHECK(picker.urlLabel()->text().contains(QChar(0x2026)));
    CHECK(picker.urlLabel()->fontMetrics().horizontalAdvance(picker.urlLabel()->text()) <= 220);
    CHECK(picker.urlLabel()->text().startsWith(QStringLiteral("https://")));
    CHECK(picker.urlLabel()->text().endsWith(QStringLiteral("/chat")));

    // OpenAI has no address of its own to show.
    picker.setProvider(QStringLiteral("openai"));
    CHECK(picker.urlLabel()->text().isEmpty());
    CHECK(picker.urlLabel()->toolTip().isEmpty());
    CHECK_FALSE(picker.urlLabel()->isVisibleTo(&picker));
}

// Local servers found running.

TEST_CASE("running local servers are marked and described", "[widget]") {
    TextModelPicker picker;
    picker.setProvider(QStringLiteral("ollama"));
    picker.setRunningServers(std::map<QString, QString>{
        {QStringLiteral("http://localhost:11434/v1"), QStringLiteral("ollama")},
        {QStringLiteral("http://127.0.0.1:8001/v1"), QStringLiteral("vllm")}});
    const QComboBox* combo = picker.providerCombo();
    CHECK(combo->itemText(combo->findData(QStringLiteral("ollama"))) ==
          QStringLiteral(u"Ollama · Running"));
    CHECK(combo->itemText(combo->findData(QStringLiteral("lm_studio"))) ==
          QStringLiteral("LM Studio"));
    CHECK(combo->itemData(combo->findData(QStringLiteral("ollama")), Qt::ToolTipRole).toString() ==
          QStringLiteral(u"Ollama — Connect to your Ollama server. Install and manage models in "
                         u"Ollama. Running on this computer."));
    // A server answering at another address is offered through Edit, not marked running.
    CHECK(combo->itemText(combo->findData(QStringLiteral("vllm"))) == QStringLiteral("vLLM"));
    CHECK(combo->itemData(combo->findData(QStringLiteral("vllm")), Qt::ToolTipRole).toString() ==
          QStringLiteral(u"vLLM — Connect to a vLLM server started with vllm serve. Answering at "
                         u"http://127.0.0.1:8001/v1 — choose Edit to use it."));
    // The shown endpoint keeps its selection and its refreshed tooltip.
    CHECK(combo->currentData().toString() == QStringLiteral("ollama"));
    CHECK(combo->toolTip() == combo->itemData(combo->currentIndex(), Qt::ToolTipRole).toString());
    // Nothing was announced: the host did this.
    picker.setRunningServers(std::map<QString, QString>{});
    CHECK(combo->itemText(combo->findData(QStringLiteral("ollama"))) == QStringLiteral("Ollama"));
}

TEST_CASE("the first address of a kind found names where it answers", "[widget]") {
    TextModelPicker picker;
    // Found in this order: the map overload would sort them, the vector keeps them.
    picker.setRunningServers(std::vector<std::pair<QString, QString>>{
        {QStringLiteral("http://127.0.0.1:9000/v1"), QStringLiteral("vllm")},
        {QStringLiteral("http://127.0.0.1:8001/v1"), QStringLiteral("vllm")}});
    const QComboBox* combo = picker.providerCombo();
    CHECK(combo->itemData(combo->findData(QStringLiteral("vllm")), Qt::ToolTipRole)
              .toString()
              .endsWith(QStringLiteral(
                  "Answering at http://127.0.0.1:9000/v1 — choose Edit to use it.")));
}

// fillThinkingCombo.

TEST_CASE("the thinking combo lists the offered levels and selects the saved one", "[widget]") {
    fv::NoWheelComboBox combo;
    QSignalSpy changes(&combo, &QComboBox::currentIndexChanged);
    const std::vector<std::string> offered{"off", "low", "medium", "high"};
    const auto shown = fv::fillThinkingCombo(&combo, offered, "medium");
    CHECK(shown == std::optional<std::string>("medium"));
    CHECK(comboTexts(&combo) == QStringList{QStringLiteral("Off"), QStringLiteral("Low"),
                                            QStringLiteral("Medium"), QStringLiteral("High")});
    CHECK(comboData(&combo) == std::vector<QString>{QStringLiteral("off"), QStringLiteral("low"),
                                                    QStringLiteral("medium"),
                                                    QStringLiteral("high")});
    CHECK(combo.currentData().toString() == QStringLiteral("medium"));
    CHECK(combo.isEnabled());
    // Filling never emits.
    CHECK(changes.count() == 0);
}

TEST_CASE("the thinking combo uses the nearest offered level for a saved one it lacks",
          "[widget]") {
    fv::NoWheelComboBox combo;
    const std::vector<std::string> offered{"low", "medium", "high"};
    CHECK(fv::fillThinkingCombo(&combo, offered, "max") == std::optional<std::string>("high"));
    CHECK(combo.currentData().toString() == QStringLiteral("high"));
    // An unknown level counts as off, and the lower level wins a tie.
    CHECK(fv::fillThinkingCombo(&combo, offered, "bogus") == std::optional<std::string>("low"));
    CHECK(fv::fillThinkingCombo(&combo, {"low", "high"}, "medium") ==
          std::optional<std::string>("low"));
    CHECK(combo.currentData().toString() == QStringLiteral("low"));
    // Extra high keeps its label.
    fv::fillThinkingCombo(&combo, {"off", "xhigh"}, "xhigh");
    CHECK(combo.currentText() == QStringLiteral("Extra high"));
}

TEST_CASE("the thinking combo shows Provider default, disabled, when nothing is offered",
          "[widget]") {
    fv::NoWheelComboBox combo;
    fv::fillThinkingCombo(&combo, {"off", "low"}, "low");
    QSignalSpy changes(&combo, &QComboBox::currentIndexChanged);
    const auto shown = fv::fillThinkingCombo(&combo, {}, "low");
    CHECK_FALSE(shown.has_value());
    CHECK(combo.count() == 1);
    CHECK(combo.itemText(0) == QStringLiteral("Provider default"));
    CHECK_FALSE(combo.itemData(0).isValid());
    CHECK_FALSE(combo.currentData().isValid());
    CHECK_FALSE(combo.isEnabled());
    CHECK(changes.count() == 0);
    // Offering levels again enables it.
    fv::fillThinkingCombo(&combo, {"off", "low"}, "off");
    CHECK(combo.isEnabled());
    CHECK(combo.count() == 2);
}

TEST_CASE("the thinking combo keeps a blocked signal state", "[widget]") {
    fv::NoWheelComboBox combo;
    combo.blockSignals(true);
    fv::fillThinkingCombo(&combo, {"off"}, "off");
    CHECK(combo.signalsBlocked());
}
