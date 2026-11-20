#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include <QDir>
#include <QLineEdit>
#include <QPushButton>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/utf8.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_meeting/intelligence_page.h"
#include "ui_meeting/meeting_consent_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/settings_switch.h"

namespace {

TEST_CASE("meeting intelligence discloses the selected destination and grants only that route",
          "[widget]") {
    const fv::meeting_agent::MeetingAgentRoute route{"pi",
                                                     "ollama",
                                                     "example",
                                                     {{"profile_id", "ollama"},
                                                      {"kind", "ollama"},
                                                      {"name", "Ollama"},
                                                      {"base_url", "http://127.0.0.1:11434/v1"}}};
    const auto local = fv::meetingConsentCopy(route);
    CHECK_FALSE(local.remote);
    CHECK(local.destination.find("127.0.0.1") != std::string::npos);

    auto changed = route;
    changed.endpoint["base_url"] = "https://example.invalid/v1";
    CHECK(fv::meetingConsentCopy(changed).remote);
    fv::MeetingConsentGrants grants;
    CHECK_FALSE(grants.allows(route));

    fv::MeetingConsentDialog dialog(local);
    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("meetingConsentNotNowButton"));
    auto* enable = dialog.findChild<QPushButton*>(QStringLiteral("meetingConsentEnableButton"));
    REQUIRE(cancel != nullptr);
    REQUIRE(enable != nullptr);
    cancel->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(grants.allows(route));
    enable->click();
    CHECK(dialog.result() == QDialog::Accepted);
    grants.grant(route);
    CHECK(grants.allows(route));
    CHECK_FALSE(grants.allows(changed));
}

TEST_CASE("meeting intelligence settings put text sharing choices behind explicit switches",
          "[widget]") {
    auto original = fv::agent_picker::scanSeam();
    struct Restore {
        fv::agent_picker::ScanSeam original;
        ~Restore() { fv::agent_picker::scanSeam() = std::move(original); }
    } reset{std::move(original)};
    fv::agent_picker::scanSeam().cached_agents = [] {
        return std::optional<fv::agents::ScanResult>(std::in_place);
    };
    fv::MemorySettingsStore settings;
    fv::AppServices services;
    services.settings = &settings;
    fv::AppContext context(services);
    fv::SettingsPage container;
    fv::IntelligencePage page(container, context);
    auto* recall =
        qobject_cast<fv::SettingsSwitch*>(page.namedControl("meeting_past_recall_enabled"));
    auto* folder =
        qobject_cast<fv::SettingsSwitch*>(page.namedControl("meeting_context_folder_enabled"));
    REQUIRE(recall != nullptr);
    REQUIRE(folder != nullptr);
    CHECK_FALSE(recall->isChecked());
    CHECK_FALSE(folder->isChecked());
    CHECK_FALSE(settings.get(fv::keys::meeting_cloud_consent_given));
    recall->click();
    CHECK(settings.get(fv::keys::meeting_past_recall_enabled));
    CHECK_FALSE(settings.get(fv::keys::meeting_context_folder_enabled));
    CHECK_FALSE(settings.get(fv::keys::meeting_cloud_consent_given));
}

TEST_CASE("meeting knowledge folder Browse and Clear save path and search together", "[widget]") {
    auto original = fv::agent_picker::scanSeam();
    struct Restore {
        fv::agent_picker::ScanSeam original;
        ~Restore() { fv::agent_picker::scanSeam() = std::move(original); }
    } reset{std::move(original)};
    fv::agent_picker::scanSeam().cached_agents = [] {
        return std::optional<fv::agents::ScanResult>(std::in_place);
    };
    fv::testkit::TempDataRoot root;
    const auto vault = root.path() / "vault";
    std::filesystem::create_directories(vault);
    const QString vaultText = fv::qt::toQString(fv::path_to_utf8(vault));
    fv::MemorySettingsStore settings;
    settings.update({{"meeting_context_folder_path", "/example/missing"},
                     {"meeting_context_folder_enabled", true},
                     {"meeting_language", "it"}});
    fv::AppServices services;
    services.settings = &settings;
    fv::AppContext context(services);
    fv::SettingsPage container;
    QString picked;
    QString chooserStartPath;
    fv::IntelligencePage page(container, context,
                              [&](QWidget*, const QString&, const QString& path) {
                                  chooserStartPath = path;
                                  return picked;
                              });
    auto* folder = qobject_cast<QLineEdit*>(page.namedControl("meeting_context_folder_path"));
    auto* enabled =
        qobject_cast<fv::SettingsSwitch*>(page.namedControl("meeting_context_folder_enabled"));
    auto* browse = page.findChild<QPushButton*>(QStringLiteral("browseMeetingKnowledgeFolder"));
    auto* clear = page.findChild<QPushButton*>(QStringLiteral("clearMeetingKnowledgeFolder"));
    REQUIRE(folder != nullptr);
    REQUIRE(enabled != nullptr);
    REQUIRE(browse != nullptr);
    REQUIRE(clear != nullptr);
    CHECK(folder->text() == QStringLiteral("/example/missing"));
    CHECK(enabled->isChecked());

    // A folder that does not exist: Browse starts at home, and a cancelled choice changes nothing.
    browse->click();
    CHECK(chooserStartPath == QDir::homePath());
    CHECK(folder->text() == QStringLiteral("/example/missing"));
    CHECK(enabled->isChecked());

    clear->click();
    CHECK(folder->text().isEmpty());
    CHECK_FALSE(enabled->isChecked());
    CHECK(settings.get(fv::keys::meeting_context_folder_path).empty());
    CHECK_FALSE(settings.get(fv::keys::meeting_context_folder_enabled));

    // The chosen path is saved as os.path.normpath writes it.
    picked = vaultText + QStringLiteral("/./notes/../");
    browse->click();
    CHECK(chooserStartPath == QDir::homePath());
    CHECK(folder->text() == QDir::toNativeSeparators(vaultText));
    CHECK(enabled->isChecked());
    CHECK(settings.get(fv::keys::meeting_context_folder_path) ==
          fv::qt::toUtf8(QDir::toNativeSeparators(vaultText)));
    CHECK(settings.get(fv::keys::meeting_context_folder_enabled));

    // An existing folder in the field is where Browse starts.
    picked.clear();
    browse->click();
    CHECK(chooserStartPath == QDir::toNativeSeparators(vaultText));

    clear->click();
    CHECK(folder->text().isEmpty());
    CHECK_FALSE(enabled->isChecked());
    CHECK(settings.get(fv::keys::meeting_context_folder_path).empty());
    CHECK_FALSE(settings.get(fv::keys::meeting_context_folder_enabled));
    CHECK(settings.get(fv::keys::meeting_language) == "it");
}

} // namespace
