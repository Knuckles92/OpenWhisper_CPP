#include "ui_personalize/snippets_page.h"

#include <utility>
#include <vector>

#include <QLabel>
#include <QPointer>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "settings/keys.h"
#include "text/personalize_copy.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/snippets_panel.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/icons.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

// SEARCH_FIELDS: (control, title, description).
struct Field {
    const char* target;
    const char* title;
    const char* description;
};
constexpr Field kEnabledField{
    "snippets_enabled_tile", "Expand snippets as you dictate",
    "Say a trigger phrase, alone or mid-sentence, and its text is typed exactly as saved, with "
    "no AI. Live dictation only."};
constexpr Field kLibraryField{
    "snippets_library_tile", "Your snippets",
    "Pick one to edit, or add a new one. Triggers ignore capitals and punctuation."};

QString q(const char* text) {
    return QString::fromUtf8(text);
}

const Json& orEmpty(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
}

} // namespace

SnippetsPage::SnippetsPage(PersonalizeHost& host, SettingsPage& container, SettingsStore& store)
    : PersonalizePage(host, container) {
    enabledTile_ = new SettingTile(q(kEnabledField.title), q(kEnabledField.description),
                                   ui::designIcon(QStringLiteral("text-plus-green.svg")));
    connect(enabledTile_->checkbox(), &QAbstractButton::toggled, this,
            &SnippetsPage::enabledToggled);
    addTileGroup(QString(), {enabledTile_}, 1);

    libraryTile_ = new InfoTile(q(kLibraryField.title), q(kLibraryField.description),
                                ui::designIcon(QStringLiteral("notes-blue.svg")));
    panel_ = new SnippetsPanel(store);
    libraryTile_->addTrailing(panel_->countLabel());
    libraryTile_->addBody(panel_);
    // The two-column panel needs the indent's room at narrow widths.
    libraryTile_->setBodyIndented(false);
    connect(panel_, &SnippetsPanel::snippetsChanged, this,
            [this] { this->host().notifyChanged(QStringLiteral("snippets")); });
    addTileGroup(QString(), {libraryTile_}, 1);
    // OpenWhisper's page column ends in a stretch (ensure_page), which takes its share of any
    // height the page is given beyond what the panel needs.
    pageLayout()->addStretch();

    exposeControl(kEnabledField.target, enabledTile_);
    exposeControl("snippets_enabled_check", enabledTile_->checkbox());
    exposeControl(kLibraryField.target, libraryTile_);
    exposeControl("snippets_panel", panel_);
}

SnippetsPage::~SnippetsPage() = default;

SettingsSwitch* SnippetsPage::enabledCheck() const {
    return enabledTile_->checkbox();
}

void SnippetsPage::enabledToggled(bool checked) {
    if (loading()) {
        return;
    }
    if (save(keys::snippets_enabled.key(), checked)) {
        host().notifyChanged(QStringLiteral("snippets"));
        return;
    }
    // The save failed: show what is still saved. While loading, the toggle this causes is
    // ignored, and the tile still restyles.
    const LoadingScope scope(*this);
    enabledCheck()->setChecked(!checked);
}

void SnippetsPage::load(const Json& settings) {
    enabledCheck()->setChecked(keys::snippets_enabled.read(orEmpty(settings)));
    panel_->refresh();
}

void SnippetsPage::refreshPage() {
    panel_->refresh();
}

bool SnippetsPage::saveDrafts(bool ask) {
    return panel_->saveDraft(ask);
}

void registerSnippetsPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::snippets;
    descriptor.title = QStringLiteral("Snippets");
    descriptor.subtitle =
        QStringLiteral("Say a short phrase to insert text you use often, exactly as written.");
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new SnippetsPage(host, container, context.settings());
    };
    descriptor.railValue = [](const Json& settings) {
        return qt::toQString(text::snippets_page::rail_value(settings));
    };
    for (const Field& field : {kEnabledField, kLibraryField}) {
        descriptor.searchFields.push_back({field.target, q(field.title), q(field.description)});
    }
    descriptor.controls = {"snippets_enabled_tile", "snippets_enabled_check",
                           "snippets_library_tile", "snippets_panel"};
    registerPersonalizePage(context, std::move(descriptor));
}

void addSnippetsHomeRows(HomePage& home) {
    auto* add = neutralButton(new Button(QStringLiteral("Add")));
    add->setBaseMinimumSize(88, 34);
    SettingsDialog* dialog = &home.dialog();
    QObject::connect(add, &QPushButton::clicked, &home, [dialog] {
        dialog->selectDestination(qt::toQString(destinations::snippets));
        if (auto* panel = qobject_cast<SnippetsPanel*>(dialog->control("snippets_panel"))) {
            panel->newSnippet();
        }
    });
    const QPointer<WrappedLabel> detail =
        home.addRow(HomeCardId::Personalize, QStringLiteral("Snippets"), QString(), add);
    add->setAccessibleName(QStringLiteral("Add a snippet"));
    home.addRefreshHook([detail](const Json& settings) {
        if (detail == nullptr) {
            return;
        }
        const QString count = qt::toQString(text::snippets_page::count(settings));
        if (!keys::snippets_enabled.read(orEmpty(settings))) {
            detail->setText(count.isEmpty() ? QStringLiteral("Off")
                                            : QStringLiteral("Off · %1").arg(count));
        } else {
            detail->setText(count.isEmpty()
                                ? QStringLiteral("None yet. Say a phrase to insert text you use "
                                                 "often.")
                                : count);
        }
    });
}

} // namespace fv
