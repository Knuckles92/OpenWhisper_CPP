// Settings → App → API keys (tests/test_api_keys_settings_page.py), over an in-memory credential
// store, an environment of the test's own and a fake key check. The page lists the text-model
// endpoints' credentials (OpenAI, OpenRouter, Groq and the two OpenCode plans by default), then
// the speech servers', then TypeSafe's row.
#include <catch2/catch_test_macros.hpp>

#include <future>
#include <mutex>
#include <optional>
#include <string>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QTest>

#include "app_pages_support.h"
#include "llm/profiles.h"
#include "ui_settings/api_keys_section.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

const std::string kSecret = "sk-proj-test-secret-value-7890wxyz";

// The credential store blocks and refuses the GUI thread: the test asks it on the worker.
template <class Fn> auto onWorker(AppPagesRig& rig, Fn fn) {
    using Result = decltype(fn());
    auto task = std::make_shared<std::packaged_task<Result()>>(std::move(fn));
    std::future<Result> result = task->get_future();
    rig.pool().post([task] { (*task)(); });
    return result.get();
}

struct KeysScene {
    explicit KeysScene(const Json& values = Json::object()) : rig(values) {
        QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::apiKeysChanged,
                         [this] { changes.push_back(true); });
    }

    fv::ApiKeysSection& section() {
        auto& keys = rig.page<fv::ApiKeysSection>(dest::api_keys);
        // The check of a text-model endpoint's key (the default asks the context's HTTP client)
        // answers as the rig's key check does.
        keys.setTextVerify([this](const fv::llm::TextLLMProfile& profile, const std::string& key) {
            std::lock_guard lock(verifyMutex);
            textVerified.emplace_back(profile.id, key);
            return rig.verifyAnswer;
        });
        REQUIRE(pumpUntil([&keys] { return keys.stateCurrent(); }));
        return keys;
    }
    std::vector<std::pair<std::string, std::string>> verifiedNow() {
        std::lock_guard lock(verifyMutex);
        return textVerified;
    }
    void select(const char* env) {
        QComboBox* combo = section().combo();
        combo->setCurrentIndex(combo->findData(QString::fromLatin1(env)));
        REQUIRE(pumpUntil([this] { return section().stateCurrent(); }));
    }
    void refresh() {
        rig.dialog().refresh();
        REQUIRE(pumpUntil([this] { return section().stateCurrent(); }));
    }
    // A save, removal or test finished.
    void settle() {
        REQUIRE(pumpUntil([this] {
            return !section().storing() && !section().testing() && section().stateCurrent();
        }));
    }
    QString railNow() {
        rig.dialog().refreshRailValues();
        return rig.dialog().rail()->value(qs(dest::api_keys));
    }
    std::optional<std::string> stored(const std::string& name) {
        return onWorker(rig, [this, name] { return rig.store().get(name); });
    }
    QString message() { return rig.dialog().messageText(); }

    AppPagesRig rig;
    std::vector<bool> changes;
    std::mutex verifyMutex;
    std::vector<std::pair<std::string, std::string>> textVerified;
};

class UnavailableBackend final : public fv::MemoryCredentialBackend {
public:
    fv::CredentialStatus probe() override {
        return {false, "in-memory store", "no keyring daemon"};
    }
};

} // namespace

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_lists_a_speech_server_key_and_what_uses_it
TEST_CASE("test_lists_a_speech_server_key_and_what_uses_it",
          "[test_api_keys_settings_page][widget]") {
    KeysScene scene(
        {{"api_transcription_servers",
          {{"speaches",
            {{"base_url", "http://gpu-box:8000/v1"}, {"api_key_env", "SPEACHES_API_KEY"}}}}}});
    scene.refresh();
    QComboBox* combo = scene.section().combo();
    CHECK(combo->itemText(combo->findData(QStringLiteral("SPEACHES_API_KEY"))) ==
          QStringLiteral(u"Speaches · SPEACHES_API_KEY"));
    CHECK(combo->itemText(combo->findData(QStringLiteral("GROQ_API_KEY"))) ==
          QStringLiteral(u"Groq · GROQ_API_KEY"));
    scene.select("GROQ_API_KEY");
    // Groq's key serves both its chat models and its speech models.
    CHECK(scene.section().usesCaption()->text() ==
          QStringLiteral("Sent as the API key to Groq for transcript cleanup and meeting "
                         "intelligence, and to Groq for API transcription."));
    scene.select("SPEACHES_API_KEY");
    CHECK(scene.section().usesCaption()->text() ==
          QStringLiteral("Sent as the API key to Speaches for API transcription."));
    const auto endpoint = fv::endpointFor(*scene.rig.settings().snapshot(), "SPEACHES_API_KEY");
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->base_url == std::optional<std::string>("http://gpu-box:8000/v1"));
    CHECK(scene.railNow() == QStringLiteral("0 of 7 set"));
    CHECK(scene.section().status()->text().contains(QStringLiteral("No key set")));
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_field_hides_input_until_show_is_toggled
TEST_CASE("test_field_hides_input_until_show_is_toggled", "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    QLineEdit* edit = scene.section().edit();
    CHECK(edit->echoMode() == QLineEdit::Password);
    scene.section().showButton()->setChecked(true);
    CHECK(edit->echoMode() == QLineEdit::Normal);
    CHECK(scene.section().showButton()->text() == QStringLiteral("Hide"));
    scene.section().showButton()->setChecked(false);
    CHECK(edit->echoMode() == QLineEdit::Password);
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_save_stores_key_outside_settings_and_masks_it
TEST_CASE("test_save_stores_key_outside_settings_and_masks_it",
          "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    scene.select("OPENAI_API_KEY");
    auto& section = scene.section();
    CHECK_FALSE(section.saveButton()->isEnabled());
    // Enter saves only while Save is enabled.
    Q_EMIT section.edit()->returnPressed();
    CHECK_FALSE(section.storing());
    section.edit()->setText(QString::fromStdString(" " + kSecret + " "));
    CHECK(section.saveButton()->isEnabled());
    section.showButton()->setChecked(true);

    section.saveKey();
    scene.settle();

    CHECK(scene.stored("OPENAI_API_KEY") == kSecret);
    CHECK(section.edit()->text().isEmpty());
    CHECK(section.edit()->echoMode() == QLineEdit::Password);
    const QString status = section.status()->text();
    CHECK(status.contains(QStringLiteral("Saved in in-memory store")));
    CHECK(status.contains(QStringLiteral(u"••••wxyz")));
    CHECK_FALSE(status.contains(QString::fromStdString(kSecret)));
    CHECK(section.status()->property("tone").toString() == QStringLiteral("success"));
    CHECK(scene.message() == QStringLiteral("OpenAI API key saved."));
    CHECK(scene.railNow() == QStringLiteral("1 of 6 set"));
    CHECK(section.removeButton()->isEnabled());
    CHECK(scene.changes == std::vector<bool>{true});
    const std::string file = scene.rig.settings().snapshot()->dump();
    CHECK(file.find(kSecret) == std::string::npos);
    CHECK(file.find("wxyz") == std::string::npos);
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_invalid_key_is_rejected_before_storage
TEST_CASE("test_invalid_key_is_rejected_before_storage", "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    scene.select("OPENAI_API_KEY");
    scene.section().edit()->setText(QStringLiteral("sk-has a space"));
    scene.section().saveKey();
    CHECK_FALSE(scene.section().storing());
    CHECK_FALSE(scene.stored("OPENAI_API_KEY").has_value());
    CHECK(scene.message().contains(QStringLiteral("space")));
    CHECK(scene.changes.empty());
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_environment_value_is_reported_and_shadowed_by_saved_key
TEST_CASE("test_environment_value_is_reported_and_shadowed_by_saved_key",
          "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    scene.rig.env().set("OPENROUTER_API_KEY", "sk-or-environment-value-0001");
    scene.select("OPENROUTER_API_KEY");
    scene.refresh();
    scene.select("OPENROUTER_API_KEY");
    auto& section = scene.section();
    QString status = section.status()->text();
    CHECK(status.contains(QStringLiteral("OPENROUTER_API_KEY environment variable")));
    CHECK(status.contains(QStringLiteral(u"••••0001")));
    CHECK_FALSE(section.removeButton()->isEnabled());
    CHECK(section.testButton()->isEnabled());

    section.edit()->setText(QString::fromStdString(kSecret));
    section.saveKey();
    scene.settle();
    status = section.status()->text();
    CHECK(status.contains(QStringLiteral(u"••••wxyz")));
    CHECK(status.contains(QStringLiteral("ignored while a key is saved here")));

    scene.rig.answer = true;
    section.removeKey();
    scene.settle();
    CHECK_FALSE(scene.stored("OPENROUTER_API_KEY").has_value());
    CHECK(section.status()->text().contains(
        QStringLiteral("OPENROUTER_API_KEY environment variable")));
    CHECK(scene.message() == QStringLiteral("OpenRouter API key removed."));
    CHECK(scene.changes == std::vector<bool>{true, true});
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_declined_removal_keeps_key
TEST_CASE("test_declined_removal_keeps_key", "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    onWorker(scene.rig, [&scene] {
        scene.rig.store().set("OPENAI_API_KEY", kSecret);
        return true;
    });
    scene.select("OPENAI_API_KEY");
    scene.refresh();
    scene.rig.answer = false;
    scene.section().removeKey();
    CHECK(scene.rig.asked.size() == 1);
    CHECK(scene.rig.asked[0].first == QStringLiteral("Remove saved key"));
    CHECK(scene.stored("OPENAI_API_KEY") == kSecret);
    CHECK(scene.changes.empty());
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_unavailable_store_disables_saving_and_explains
TEST_CASE("test_unavailable_store_disables_saving_and_explains",
          "[test_api_keys_settings_page][widget]") {
    // Outlives the scene, whose worker reads it.
    fv::CredentialStore unavailable(std::make_shared<UnavailableBackend>(), fv::identity());
    KeysScene scene;
    scene.rig.pages().hooks().credentials = &unavailable;
    scene.refresh();
    scene.select("OPENAI_API_KEY");
    auto& section = scene.section();
    section.edit()->setText(QString::fromStdString(kSecret));
    CHECK_FALSE(section.saveButton()->isEnabled());
    CHECK_FALSE(section.removeButton()->isEnabled());
    const QString caption = section.storeCaption()->text();
    // The raw error stays out of the sentence and in the tooltip.
    CHECK_FALSE(caption.contains(QStringLiteral("no keyring daemon")));
    CHECK(caption.contains(QStringLiteral("isn't available on this computer")));
    CHECK(caption.contains(QStringLiteral("never falls back to an unprotected file")));
    CHECK(section.storeCaption()->toolTip().contains(QStringLiteral("no keyring daemon")));
    scene.rig.dropDialog();
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_test_button_reports_verdict_without_echoing_key
TEST_CASE("test_test_button_reports_verdict_without_echoing_key",
          "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    scene.select("OPENAI_API_KEY");
    auto& section = scene.section();
    section.edit()->setText(QString::fromStdString(kSecret));
    scene.rig.verifyAnswer = {false, "api.openai.com rejected the key (HTTP 401)."};

    section.testKey();
    CHECK(section.testing());
    CHECK_FALSE(section.saveButton()->isEnabled());
    REQUIRE(pumpUntil([&section] { return !section.testing(); }));

    CHECK(scene.verifiedNow() ==
          std::vector<std::pair<std::string, std::string>>{{"openai", kSecret}});
    const QString message = scene.message();
    CHECK(message ==
          QStringLiteral("OpenAI API key failed: api.openai.com rejected the key (HTTP 401)."));
    CHECK_FALSE(message.contains(QString::fromStdString(kSecret)));
    // A test never persists anything.
    CHECK_FALSE(scene.stored("OPENAI_API_KEY").has_value());
    CHECK(section.edit()->text() == QString::fromStdString(kSecret));
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_focus_api_keys_selects_destination_and_credential
TEST_CASE("test_focus_api_keys_selects_destination_and_credential",
          "[test_api_keys_settings_page][widget]") {
    KeysScene scene;
    scene.rig.dialog().focusApiKeys(QStringLiteral("GROQ_API_KEY"));
    CHECK(scene.rig.dialog().rail()->currentKey() == qs(dest::api_keys));
    CHECK(scene.section().combo()->currentData().toString() == QStringLiteral("GROQ_API_KEY"));
}

TEST_CASE("each opening re-reads the keys the API keys rail counts", "[widget]") {
    // rail_value asks credential_source on every redraw; a key set in the environment while
    // Settings was closed shows the next time it opens, the API keys page never built.
    KeysScene scene;
    fv::SettingsDialog& dialog = scene.rig.dialog();
    REQUIRE(pumpUntil([&dialog] {
        return dialog.rail()->value(qs(dest::api_keys)) == QStringLiteral("0 of 6 set");
    }));
    scene.rig.env().set("GROQ_API_KEY", "gsk-environment-value-0001");
    dialog.refreshRailValues();
    CHECK(dialog.rail()->value(qs(dest::api_keys)) == QStringLiteral("0 of 6 set"));

    dialog.show();
    CHECK(pumpUntil([&dialog] {
        return dialog.rail()->value(qs(dest::api_keys)) == QStringLiteral("1 of 6 set");
    }));
    CHECK_FALSE(dialog.isBuilt(dest::api_keys));
    dialog.hide();
}

TEST_CASE("a key test with nothing typed sends the resolved key", "[widget]") {
    KeysScene scene;
    onWorker(scene.rig, [&scene] {
        scene.rig.store().set("GROQ_API_KEY", "gsk_saved-value-123456");
        return true;
    });
    scene.select("GROQ_API_KEY");
    scene.refresh();
    auto& section = scene.section();
    section.testKey();
    REQUIRE(pumpUntil([&section] { return !section.testing(); }));
    CHECK(scene.verifiedNow() ==
          std::vector<std::pair<std::string, std::string>>{{"groq", "gsk_saved-value-123456"}});
    CHECK(scene.message().startsWith(QStringLiteral("Groq API key works: ")));
}

// from
// tests/test_api_keys_settings_page.py::TestApiKeysSettingsPage::test_lists_builtin_and_custom_credentials
TEST_CASE("test_lists_builtin_and_custom_credentials", "[test_api_keys_settings_page][widget]") {
    Json settings = Json::object();
    fv::llm::upsert_custom_profile(settings, "LM Studio", "http://127.0.0.1:1234/v1",
                                   "LMSTUDIO_API_KEY");
    fv::llm::upsert_custom_profile(settings, "Ollama", "http://127.0.0.1:11434/v1");
    KeysScene scene(settings);
    scene.refresh();

    QComboBox* combo = scene.section().combo();
    std::vector<std::string> names;
    for (int i = 0; i < combo->count(); ++i) {
        names.push_back(combo->itemData(i).toString().toStdString());
    }
    CHECK(names == std::vector<std::string>{"OPENAI_API_KEY", "OPENROUTER_API_KEY", "GROQ_API_KEY",
                                            "OPENCODE_GO_API_KEY", "OPENCODE_ZEN_API_KEY",
                                            "LMSTUDIO_API_KEY", "TYPESAFE_API_KEY"});
    CHECK(combo->itemText(combo->findData(QStringLiteral("TYPESAFE_API_KEY"))) ==
          QStringLiteral(u"TypeSafe · TYPESAFE_API_KEY"));
    CHECK(combo->itemText(combo->findData(QStringLiteral("LMSTUDIO_API_KEY"))) ==
          QStringLiteral(u"LM Studio · LMSTUDIO_API_KEY"));
    scene.select("LMSTUDIO_API_KEY");
    CHECK(scene.section().usesCaption()->text() ==
          QStringLiteral("Sent as the API key to LM Studio for transcript cleanup and meeting "
                         "intelligence."));
}

TEST_CASE("a text-model key is tested as its endpoint and the message names it", "[widget]") {
    KeysScene scene;
    scene.select("OPENROUTER_API_KEY");
    auto& section = scene.section();
    section.edit()->setText(QString::fromStdString(kSecret));
    scene.rig.verifyAnswer = {true, "openrouter.ai accepted the key."};

    section.testKey();
    REQUIRE(pumpUntil([&section] { return !section.testing(); }));

    CHECK(scene.verifiedNow() ==
          std::vector<std::pair<std::string, std::string>>{{"openrouter", kSecret}});
    CHECK(scene.rig.verifiedNow().empty());
    const QString message = scene.message();
    CHECK(message == QStringLiteral("OpenRouter API key works: openrouter.ai accepted the key."));
    CHECK_FALSE(message.contains(QString::fromStdString(kSecret)));
    CHECK_FALSE(scene.stored("OPENROUTER_API_KEY").has_value());
    CHECK(scene.rig.settings().snapshot()->dump().find(kSecret) == std::string::npos);
}

TEST_CASE("a speech server's own key is tested as the speech server", "[widget]") {
    KeysScene scene({{"api_transcription_servers",
                      {{"custom",
                        {{"base_url", "https://speech.example.test/v1"},
                         {"api_key_env", "SPEECH_GATEWAY_KEY"}}}}}});
    scene.refresh();
    scene.select("SPEECH_GATEWAY_KEY");
    auto& section = scene.section();
    section.edit()->setText(QString::fromStdString(kSecret));
    scene.rig.verifyAnswer = {false, "speech.example.test rejected the key (HTTP 401)."};

    section.testKey();
    REQUIRE(pumpUntil([&section] { return !section.testing(); }));

    CHECK(scene.rig.verifiedNow() ==
          std::vector<std::pair<std::string, std::string>>{{"custom", kSecret}});
    CHECK(scene.verifiedNow().empty());
    CHECK(scene.message() ==
          QStringLiteral("SPEECH_GATEWAY_KEY failed: speech.example.test rejected the key "
                         "(HTTP 401)."));
}

// A11-72: the entry tile as settings_api_keys.py builds it: a password field of at most 512
// characters that keeps input methods from retaining the key, and an 80 x 40 Show toggle.
TEST_CASE("the key field limits, hides and keeps input methods away from the key", "[widget]") {
    KeysScene scene;
    auto& section = scene.section();
    CHECK(section.entryTile()->titleLabel()->text() == QStringLiteral("Add or replace a key"));
    QLineEdit* edit = section.edit();
    CHECK(edit->maxLength() == 512);
    CHECK(edit->placeholderText() == QStringLiteral("Paste a key to save or test"));
    const Qt::InputMethodHints hints = edit->inputMethodHints();
    CHECK(hints.testFlag(Qt::ImhHiddenText));
    CHECK(hints.testFlag(Qt::ImhSensitiveData));
    CHECK(hints.testFlag(Qt::ImhNoPredictiveText));
    CHECK(section.showButton()->isCheckable());
    CHECK(section.showButton()->text() == QStringLiteral("Show"));
    CHECK(section.showButton()->minimumWidth() >= 80);
    CHECK(section.showButton()->minimumHeight() == 40);
    CHECK(section.showButton()->maximumHeight() == 40);
    CHECK(section.saveButton()->text() == QStringLiteral("Save key"));
    CHECK(section.testButton()->text() == QStringLiteral("Test"));
    CHECK(section.removeButton()->text() == QStringLiteral("Remove saved key"));
    CHECK_FALSE(edit->property("hasText").toBool());
    edit->setText(QStringLiteral("sk-typed"));
    CHECK(edit->property("hasText").toBool());
}
