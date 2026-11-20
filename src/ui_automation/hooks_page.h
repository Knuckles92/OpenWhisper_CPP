#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QPointer>
#include <QWidget>

#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/json.h"

class QVBoxLayout;

namespace fv {

class AppContext;
class FilterSection;
class InfoTile;
class ProgramSection;
class SettingsPage;
class SettingsPageHost;
class WebhookSection;

// What Settings > Automate > Hooks says about itself (ui_qt/dialogs/settings_hooks.py): TITLE,
// SUBTITLE, SEARCH_FIELDS, CONTROL_ATTRS and rail_value.
namespace hooks_page {

inline constexpr std::string_view TITLE = "Hooks";
inline constexpr std::string_view SUBTITLE =
    "Tell your own tools when you dictate, and let them change the words before they're pasted.";

struct SearchField {
    std::string_view target;
    std::string_view title;
    std::string_view description;
};
// SEARCH_FIELDS, in page order.
const std::vector<SearchField>& search_fields();
// CONTROL_ATTRS: the three tiles, then the three sections.
const std::vector<std::string>& control_names();

// "None yet", or "<n> hook(s)" with " · <m> on" when some are off, over the saved webhooks,
// programs and text filters.
std::string rail_value(const Json& settings);

} // namespace hooks_page

// Settings > Automate > Hooks: a tile each for webhooks, programs and text filters, each holding
// its section. A page content widget inside the window's SettingsPage, with its own column
// (margins 0, spacing 8). The sections save through the context's settings store and ask the
// engines the context provides (automation::WebhookControl or WebhookEngine, ProgramControl or
// ProgramEngine, FilterLeases); without an engine the Test buttons are disabled and the recent
// lists read "No deliveries yet." and "No runs yet.".
// Affinity: GUI thread.
class HooksPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // `container` is the page the Settings window made for the destination (the parent); `host`
    // and `context` outlive the page.
    HooksPage(AppContext& context, SettingsPageHost& host, SettingsPage& container);
    ~HooksPage() override;

    WebhookSection* webhooks() const { return webhooks_; }
    ProgramSection* programs() const { return programs_; }
    FilterSection* filters() const { return filters_; }
    InfoTile* webhooksTile() const { return webhooksTile_; }
    InfoTile* programsTile() const { return programsTile_; }
    InfoTile* filtersTile() const { return filtersTile_; }
    QVBoxLayout* pageLayout() const { return layout_; }

    // SettingsPageModule: each section reads its saved items again.
    void loadSettings(const Json& settings) override;
    void refreshPage() override;
    bool saveDrafts(bool ask) override;
    QWidget* namedControl(std::string_view name) override;

Q_SIGNALS:
    // The lease list's link to API & MCP was pressed; the page opens it through its host.
    void apiPageRequested();

private:
    SettingsPageHost& host_;
    QVBoxLayout* layout_ = nullptr;
    WebhookSection* webhooks_ = nullptr;
    ProgramSection* programs_ = nullptr;
    FilterSection* filters_ = nullptr;
    InfoTile* webhooksTile_ = nullptr;
    InfoTile* programsTile_ = nullptr;
    InfoTile* filtersTile_ = nullptr;
    std::map<std::string, QPointer<QWidget>, std::less<>> controls_;
};

// Registers the page for destinations::hooks in the context's Settings pages, with its rail
// value, search fields and controls. It is built on first selection over a DialogPageHost of the
// Settings window it is built in; building it without one throws std::logic_error.
// Affinity: UI actor.
void registerHooksPage(AppContext& context);

} // namespace fv
