// Guided setup's Clean up step over the app's AI cleanup handlers (guided_setup.py
// provider_summary, CleanupChoices and the cleanup switch through the AI cleanup page), as
// tests/test_guided_setup.py checks them: a real Settings window with the Personalize pages over
// fakes (tests/ui_personalize/cleanup_support.h), offscreen. No key store, network or real
// message box is reached.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <QAbstractButton>
#include <QFrame>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>

#include "../ui_personalize/cleanup_support.h"
#include "golden/golden.h"
#include "ui_personalize/cleanup_page.h"
#include "ui_setup/cleanup_provider_row.h"
#include "ui_setup/guided_setup_dialog.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;

namespace {

namespace golden = fv::golden;

// make_setup over the AI cleanup page's handlers: the guided setup window whose Settings window
// is the rig's, with recorded destinations and a provider row when `summary` is given.
class CleanupSetupRig : public fvtest::CleanupRig {
public:
    explicit CleanupSetupRig(const Json& saved = Json::object(),
                             std::function<fv::ProviderSummary(const Json&)> summary = {})
        : fvtest::CleanupRig(saved) {
        fv::GuidedSetupPorts ports;
        ports.settings = [this]() -> fv::SettingsDialog& {
            return window();
        };
        ports.openDestination = [this](const QString& destination) {
            destinations.push_back(destination);
        };
        ports.openAllSettings = [] {
        };
        ports.speechModelName = [](const Json&) -> std::optional<std::string> {
            return std::string("parakeet-v3");
        };
        ports.speechModelDownloaded = [](const Json&) -> std::optional<bool> {
            return true;
        };
        ports.cleanup = fv::CleanupPort::settingsDefault();
        ports.providerSummary = std::move(summary);
        setup_ = std::make_unique<fv::GuidedSetupDialog>(settings(), std::move(ports));
        setup_->begin(false);
    }
    ~CleanupSetupRig() {
        setup_->hide();
        setup_.reset();
        fvtest::flushDeletes();
    }
    CleanupSetupRig(const CleanupSetupRig&) = delete;
    CleanupSetupRig& operator=(const CleanupSetupRig&) = delete;

    fv::GuidedSetupDialog& setup() { return *setup_; }
    Json stored() { return *settings().snapshot(); }
    std::vector<QString> destinations;

private:
    std::unique_ptr<fv::GuidedSetupDialog> setup_;
};

bool anyChecked(fv::CleanupChoices& levels) {
    for (auto* button : levels.buttons()) {
        if (button->isChecked()) {
            return true;
        }
    }
    return false;
}

std::optional<Json> value(const Json& settings, const char* key) {
    const auto found = settings.find(key);
    return found == settings.end() ? std::nullopt : std::optional<Json>(*found);
}

} // namespace

TEST_CASE("provider_summary over providers, keys, local and cloud endpoints", "[golden]") {
    golden::for_each("ui_qt.dialogs.guided_setup", "provider_summary", [](const golden::Case& c) {
        const bool saved = c.arg<bool>(1);
        const fv::ProviderSummary summary =
            fv::providerSummary(c.arg<Json>(0), [saved](const std::string&) { return saved; });
        const Json expected = c.result<Json>();
        CHECK(summary.name.toStdString() == expected.at(0).get<std::string>());
        CHECK(summary.status.toStdString() == expected.at(1).get<std::string>());
        CHECK(summary.privacy.toStdString() == expected.at(2).get<std::string>());
        CHECK(summary.remote == expected.at(3).get<bool>());
        CHECK(summary.needsKey == expected.at(4).get<bool>());
    });
}

TEST_CASE("test_cleanup_toggle_uses_existing_callback_and_hides_dependents", "[widget]") {
    CleanupSetupRig rig;
    QSignalSpy changed(&rig.cleanup(), &fv::CleanupSettings::cleanupChanged);
    rig.setup().showPage(fv::GuidedSetupDialog::CleanUp);

    rig.setup().cleanupSwitch()->click();
    CHECK(value(rig.stored(), "transcript_cleanup_enabled") == Json(true));
    CHECK_FALSE(rig.setup().cleanupOptions()->isHidden());
    CHECK(changed.count() == 1);

    rig.setup().cleanupSwitch()->click();
    CHECK(value(rig.stored(), "transcript_cleanup_enabled") == Json(false));
    CHECK(rig.setup().cleanupOptions()->isHidden());
    CHECK(changed.count() == 2);
}

TEST_CASE("test_custom_prompt_confirmation_is_reused_and_declining_keeps_it", "[widget]") {
    CleanupSetupRig rig(Json{{"transcript_cleanup_enabled", true},
                             {"transcript_cleanup_prompt", "Use my exact prompt"}});
    rig.setup().showPage(fv::GuidedSetupDialog::CleanUp);
    const Json before = rig.stored();
    fv::CleanupChoices& levels = *rig.setup().levels();
    CHECK_FALSE(anyChecked(levels));
    CHECK(rig.setup().preview()->isHidden());

    // The AI cleanup page's own question (_confirm_custom_prompt_removal), declined.
    rig.answer = QMessageBox::No;
    levels.buttons()[0]->click();
    fvtest::flushDeletes();
    REQUIRE(rig.asked.size() == 1);
    CHECK(rig.asked.back().contains(QStringLiteral("Light")));
    CHECK(rig.stored() == before);
    CHECK_FALSE(anyChecked(levels));

    rig.answer = QMessageBox::Yes;
    levels.buttons()[1]->click();
    fvtest::flushDeletes();
    REQUIRE(rig.asked.size() == 2);
    CHECK(rig.asked.back().contains(QStringLiteral("Medium")));
    CHECK_FALSE(value(rig.stored(), "transcript_cleanup_prompt").has_value());
    CHECK(value(rig.stored(), "transcript_cleanup_level") == Json("medium"));
    CHECK(levels.buttons()[1]->isChecked());
}

TEST_CASE("test_privacy_follows_actual_endpoint_and_never_claims_connected", "[golden]") {
    // (provider, url, local)
    const std::vector<std::tuple<std::string, std::string, bool>> cases{
        {"openai", "", false},
        {"openrouter", "", false},
        {"ollama", "http://localhost:11434/v1", true},
        {"ollama", "http://192.168.1.10:11434/v1", false},
        {"custom_company", "https://example.com/v1", false},
    };
    for (const auto& [provider, url, local] : cases) {
        INFO(provider << " " << url);
        Json values{{"transcript_cleanup_provider", provider},
                    {"transcript_cleanup_model", "my-model"}};
        if (provider == "ollama") {
            values["ollama_base_url"] = url;
        }
        if (provider.starts_with("custom")) {
            values["text_llm_profiles"] = Json::array({Json{{"id", provider},
                                                            {"name", "Company endpoint"},
                                                            {"kind", "custom"},
                                                            {"base_url", url},
                                                            {"api_key_env", "COMPANY_KEY"}}});
        }
        const fv::ProviderSummary summary =
            fv::providerSummary(values, [](const std::string&) { return true; });
        CHECK(summary.remote != local);
        CHECK_FALSE(summary.needsKey);
        CHECK(summary.privacy.contains(summary.name));
        CHECK(summary.privacy.contains(QStringLiteral("on this computer")) == local);
        CHECK(summary.privacy.contains(QStringLiteral("No audio")));
        CHECK_FALSE(summary.status.contains(QStringLiteral("Connected")));
        CHECK(summary.status.contains(QStringLiteral("Connection not tested")));
    }
}

TEST_CASE("test_missing_key_is_shown_without_network_probe", "[golden]") {
    int asked = 0;
    const fv::ProviderSummary summary =
        fv::providerSummary(Json::object(), [&asked](const std::string& env) {
            ++asked;
            CHECK(env == "OPENROUTER_API_KEY");
            return false;
        });
    CHECK(summary.status == QStringLiteral("API key needed"));
    CHECK(summary.needsKey);
    CHECK(asked == 1);
}

TEST_CASE("test_missing_key_links_to_api_keys_and_a_saved_key_to_cleanup", "[widget]") {
    auto saved = std::make_shared<bool>(false);
    CleanupSetupRig rig(Json{{"transcript_cleanup_enabled", true}}, [saved](const Json& settings) {
        return fv::providerSummary(settings, [saved](const std::string&) { return *saved; });
    });
    rig.setup().showPage(fv::GuidedSetupDialog::CleanUp);
    rig.setup().refresh();
    CHECK(rig.setup().providerButton()->text() == QStringLiteral("Add key"));
    rig.setup().providerButton()->click();
    REQUIRE_FALSE(rig.destinations.empty());
    CHECK(rig.destinations.back() == QStringLiteral("api_keys"));

    *saved = true;
    rig.setup().refresh();
    CHECK(rig.setup().providerButton()->text() == QStringLiteral("Change"));
    rig.setup().providerButton()->click();
    CHECK(rig.destinations.back() == QStringLiteral("cleanup"));
}

TEST_CASE("test_failed_cleanup_save_restores_source_gates", "[widget]") {
    CleanupSetupRig rig(Json{{"transcript_cleanup_enabled", false}});
    rig.setup().showPage(fv::GuidedSetupDialog::CleanUp);
    fv::CleanupPage& page = rig.builtCleanupPage();

    rig.settings().file().fail_next_replace("Disk full");
    rig.setup().cleanupSwitch()->click();
    CHECK_FALSE(rig.setup().cleanupSwitch()->isChecked());
    CHECK(rig.setup().cleanupOptions()->isHidden());
    CHECK_FALSE(page.cleanupCheck()->isChecked());
    CHECK_FALSE(page.levelTile()->isEnabled());

    rig.setup().cleanupSwitch()->click();
    CHECK(page.levelTile()->isEnabled());
}

TEST_CASE("test_enabling_cleanup_reveals_privacy", "[widget]") {
    for (const QSize size : {QSize(720, 690), QSize(540, 460)}) {
        INFO(size.width() << "x" << size.height());
        CleanupSetupRig rig(Json{{"transcript_cleanup_enabled", false}}, [](const Json& settings) {
            return fv::providerSummary(settings, [](const std::string&) { return true; });
        });
        rig.setup().show();
        rig.setup().resize(size);
        rig.setup().showPage(fv::GuidedSetupDialog::CleanUp);
        rig.setup().cleanupSwitch()->click();
        QTest::qWait(40);
        QWidget* viewport = rig.setup().scrollArea()->viewport();
        fv::WrappedLabel* privacy = rig.setup().privacyLabel();
        const int top = privacy->mapTo(viewport, privacy->rect().topLeft()).y();
        CHECK(top >= 0);
        CHECK(top + privacy->height() <= viewport->height());
        CHECK(privacy->text().contains(QStringLiteral("Transcript text is sent to")));
    }
}
