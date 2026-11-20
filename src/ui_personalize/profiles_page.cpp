#include "ui_personalize/profiles_page.h"

#include <utility>

#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "text/cleanup_profiles.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_personalize/commands_page.h"
#include "ui_personalize/personalize_host.h"

namespace fv {

ProfilesPage::ProfilesPage(PersonalizeHost& host, SettingsPage& container, SettingsStore& store)
    : PersonalizePage(host, container) {
    panel_ = new CleanupProfilesPanel(store);
    connect(panel_, &CleanupProfilesPanel::profilesChanged, this,
            [this] { this->host().notifyChanged(QStringLiteral("cleanup_profiles")); });
    connect(panel_, &CleanupProfilesPanel::captureChanged, this,
            [this](bool suspended) { this->host().setHotkeyCaptureSuspended(suspended); });
    connect(panel_, &CleanupProfilesPanel::modelRequested, this,
            [this] { focusCleanupModel(this->host()); });
    pageLayout()->addWidget(panel_);
    // OpenWhisper's page column ends in a stretch (ensure_page).
    pageLayout()->addStretch();
    exposeControl("cleanup_profiles_panel", panel_);
}

ProfilesPage::~ProfilesPage() = default;

QString ProfilesPage::railValue(const Json& settings) {
    const auto count =
        text::load_cleanup_profiles(settings.is_object() ? settings : Json::object()).size();
    return QStringLiteral("%1 profile%2")
        .arg(count)
        .arg(count == 1 ? QString() : QStringLiteral("s"));
}

void ProfilesPage::refreshPage() {
    panel_->refresh();
}

bool ProfilesPage::saveDrafts(bool ask) {
    return panel_->saveDraft(ask);
}

void ProfilesPage::cancelCapture() {
    panel_->cancelCapture();
}

void registerProfilesPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::cleanup_profiles;
    descriptor.title = QStringLiteral("Profiles");
    descriptor.subtitle =
        QStringLiteral("Turn a recording into a support ticket, email, or your own format.");
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new ProfilesPage(host, container, context.settings());
    };
    descriptor.railValue = &ProfilesPage::railValue;
    descriptor.controls = {"cleanup_profiles_panel"};
    registerPersonalizePage(context, std::move(descriptor));
}

} // namespace fv
