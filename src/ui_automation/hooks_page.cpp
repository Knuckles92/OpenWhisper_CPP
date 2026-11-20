#include "ui_automation/hooks_page.h"

#include <algorithm>
#include <stdexcept>

#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "automation/filters.h"
#include "automation/hooks_model.h"
#include "automation/programs.h"
#include "automation/webhooks.h"
#include "settings/store.h"
#include "ui_automation/hooks_panel.h"
#include "ui_automation/settings_page_host.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/setting_tile.h"

namespace fv {

namespace {

QString q(std::string_view text) {
    return qt::toQString(text);
}

// One tile of the page: what the search finds, its icon, and the kind of change it announces.
struct TileSpec {
    const char* control;
    const char* icon;
    const char* kind;
};

constexpr TileSpec kWebhooks{"hooks_webhooks_panel", "webhook-blue.svg", "webhooks"};
constexpr TileSpec kPrograms{"hooks_programs_panel", "terminal-green.svg", "programs"};
constexpr TileSpec kFilters{"hooks_filters_panel", "filter-purple.svg", "filters"};

const hooks_page::SearchField& field(std::string_view target) {
    for (const hooks_page::SearchField& entry : hooks_page::search_fields()) {
        if (entry.target == target) {
            return entry;
        }
    }
    throw std::out_of_range("no Hooks search field");
}

// The engine behind a Hooks control: the interface when the context provides it, else the engine
// itself.
template <class Interface, class Engine> Interface* engine(AppContext& context) {
    if (Interface* found = context.find<Interface>()) {
        return found;
    }
    return context.find<Engine>();
}

} // namespace

namespace hooks_page {

const std::vector<SearchField>& search_fields() {
    static const std::vector<SearchField> fields{
        {"hooks_webhooks_panel", "Webhooks",
         "Send events such as finished dictations to a web address, signed so it can check "
         "them."},
        {"hooks_programs_panel", "Programs",
         "Run a program on this computer when something happens, with the event as JSON on its "
         "input."},
        {"hooks_filters_panel", "Text filters",
         "Let a web address or a program change your words, before or after AI cleanup."},
    };
    return fields;
}

const std::vector<std::string>& control_names() {
    static const std::vector<std::string> names{
        "hooks_webhooks_panel", "hooks_programs_panel", "hooks_filters_panel",
        "hooks_webhooks",       "hooks_programs",       "hooks_filters",
    };
    return names;
}

std::string rail_value(const Json& settings) {
    namespace hooks = automation::hooks;
    std::size_t total = 0;
    std::size_t on = 0;
    const auto count = [&](const auto& items) {
        for (const auto& item : items) {
            ++total;
            on += item.enabled ? 1 : 0;
        }
    };
    count(hooks::load_webhooks(settings));
    count(hooks::load_programs(settings));
    count(hooks::load_filters(settings));
    if (total == 0) {
        return "None yet";
    }
    const std::string hooksText = std::to_string(total) + (total == 1 ? " hook" : " hooks");
    return on == total ? hooksText : hooksText + " · " + std::to_string(on) + " on";
}

} // namespace hooks_page

HooksPage::HooksPage(AppContext& context, SettingsPageHost& host, SettingsPage& container)
    : QWidget(&container), host_(host) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);

    HookServices services;
    services.store = &context.settings();
    try {
        services.io = &context.io();
    } catch (const ServiceUnavailable&) {
        // Without blocking work the secrets cannot be read; their buttons stay disabled.
    }
    try {
        services.clock = &context.clock();
    } catch (const ServiceUnavailable&) {
        // Record times and lease countdowns then follow the system clock.
    }
    services.webhooks = engine<automation::WebhookControl, automation::WebhookEngine>(context);
    services.programs = engine<automation::ProgramControl, automation::ProgramEngine>(context);
    services.leases = context.find<automation::FilterLeases>();

    const auto addTile = [&](const TileSpec& spec, auto* section, InfoTile*& tileOut) {
        const hooks_page::SearchField& entry = field(spec.control);
        auto* tile = new InfoTile(q(entry.title), q(entry.description),
                                  ui::designIcon(QString::fromLatin1(spec.icon)));
        tile->addTrailing(section->countLabel());
        tile->addBody(section);
        // The editor needs the room more than the indent, most of all when narrow.
        tile->setBodyIndented(false);
        const QString kind = QString::fromLatin1(spec.kind);
        connect(section, &HookSection::changed, this, [this, kind] { host_.notifyChanged(kind); });
        container.addTileGroup(layout_, QString(), {tile}, 1);
        tileOut = tile;
    };
    webhooks_ = new WebhookSection(services);
    addTile(kWebhooks, webhooks_, webhooksTile_);
    programs_ = new ProgramSection(services);
    addTile(kPrograms, programs_, programsTile_);
    filters_ = new FilterSection(services);
    addTile(kFilters, filters_, filtersTile_);

    connect(filters_->leases(), &LeaseList::openApiRequested, this,
            [this] { Q_EMIT apiPageRequested(); });
    connect(this, &HooksPage::apiPageRequested, this,
            [this] { host_.selectDestination(q(destinations::mcp)); });

    controls_ = {
        {"hooks_webhooks_panel", webhooksTile_}, {"hooks_programs_panel", programsTile_},
        {"hooks_filters_panel", filtersTile_},   {"hooks_webhooks", webhooks_},
        {"hooks_programs", programs_},           {"hooks_filters", filters_},
    };
}

HooksPage::~HooksPage() = default;

void HooksPage::loadSettings(const Json&) {
    refreshPage();
}

void HooksPage::refreshPage() {
    for (HookSection* section :
         {static_cast<HookSection*>(webhooks_), static_cast<HookSection*>(programs_),
          static_cast<HookSection*>(filters_)}) {
        section->refresh();
    }
}

bool HooksPage::saveDrafts(bool ask) {
    // Stops at the first section whose draft is kept.
    return webhooks_->saveDraft(ask) && programs_->saveDraft(ask) && filters_->saveDraft(ask);
}

QWidget* HooksPage::namedControl(std::string_view name) {
    const auto found = controls_.find(name);
    return found == controls_.end() ? nullptr : found->second.data();
}

void registerHooksPage(AppContext& context) {
    SettingsPageSpec spec;
    spec.destination = destinations::hooks;
    spec.title = q(hooks_page::TITLE);
    spec.subtitle = q(hooks_page::SUBTITLE);
    spec.railValue = [](const Json& settings) {
        return q(hooks_page::rail_value(settings));
    };
    for (const hooks_page::SearchField& entry : hooks_page::search_fields()) {
        spec.searchFields.push_back(
            {std::string(entry.target), q(entry.title), q(entry.description)});
    }
    spec.controls = hooks_page::control_names();
    spec.create = [&context](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        if (dialog == nullptr) {
            throw std::logic_error("The Hooks page is built inside the Settings window.");
        }
        auto host = std::make_unique<DialogPageHost>(*dialog);
        auto* page = new HooksPage(context, *host, *container);
        // Owned by the window, which deletes its pages first: the host outlives every control
        // torn down with the page.
        host.release()->setParent(dialog);
        return page;
    };
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
