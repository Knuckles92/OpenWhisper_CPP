#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include "base/json.h"
#include "focus/snapshot.h"
#include "ui_personalize/personalize_page.h"

class QLabel;

namespace fv {

class AppContext;
class Button;
class CleanupGates;
class ElidingComboBox;
class FitRow;
class HomePage;
class InfoTile;
class RemovableRows;
class SearchableComboBox;
class SegmentedBar;
class SettingTile;
class SettingsSwitch;
class WrappedLabel;

// Settings > Personalize > Apps & styles (ui_qt/dialogs/settings_styles.py _Page): knowing the
// app you dictate into, reading text near the cursor, the apps never read from, and the tone AI
// cleanup writes in for each kind of app, with apps moved between kinds.
//
// Every switch, tone and list edit saves at once, says "styles" changed and shows the page again
// from the saved settings; showing it never saves (picking a tone saves all four kinds, loading
// tones saves none). The styles lock while AI cleanup is off (the shared cleanup gate shows) or
// app awareness is off (the page's own gate, behind the cleanup one, turns awareness back on);
// the "Never read from" list stays editable whenever the app can be known, because it also keeps
// Command Mode and transforms out of those apps. The page shows the settings again whenever it
// is shown and when AI cleanup changes elsewhere in its window, because cleanup and its provider
// change on other pages.
// Affinity: GUI thread.
class StylesPage : public PersonalizePage {
    Q_OBJECT
public:
    // settings_styles._MAX_EXCLUDED: the newest this many "Never read from" apps are kept.
    static constexpr std::size_t kMaxExcluded = 100;

    // What the page asks of this computer (focus_context): the apps this session has seen,
    // newest first, and whether text near the cursor can be read here at all. Each defaults to
    // fv_focus's answer; a test replaces them.
    struct Platform {
        std::function<std::vector<focus::AppIdentity>()> recentApps;
        std::function<bool()> textReadingSupported;
    };
    // Replaces the platform answers; an empty function goes back to fv_focus's.
    static void setPlatform(Platform platform);
    static bool textReadingSupported();
    // _choices: the app pickers' names, this session's apps first (the catalogue's name for
    // each, else its own), then every name the catalogue knows, without repeats ignoring case.
    static std::vector<std::string> choices();
    // privacy_note for a settings snapshot, with the AI cleanup provider's name and locality.
    static QString privacyNote(const Json& settings);
    // EXAMPLES: one message per kind of app (app_styles::CATEGORIES) as each tone writes it.
    static QString example(std::string_view category, std::string_view tone);

    StylesPage(PersonalizeHost& host, SettingsPage& container, CleanupGates& gates);
    ~StylesPage() override;

    // One kind of app's tone picker with a live example of that tone (_StyleCard).
    struct StyleCard {
        std::string category;
        InfoTile* tile = nullptr;
        SegmentedBar* bar = nullptr;
        WrappedLabel* example = nullptr;
    };

    SettingTile* appContextTile() const { return appContextTile_; }
    SettingsSwitch* appContextCheck() const;
    SettingTile* readTextTile() const { return readTextTile_; }
    SettingsSwitch* readTextCheck() const;
    WrappedLabel* privacy() const { return privacy_; }
    InfoTile* excludedAppsTile() const { return excludedAppsTile_; }
    SearchableComboBox* excludedPicker() const { return excludedPicker_; }
    RemovableRows* excludedRows() const { return excludedRows_; }
    Button* excludedAdd() const { return excludedAdd_; }
    InfoTile* stylesCleanupGateTile() const { return stylesCleanupGateTile_; }
    InfoTile* stylesGateTile() const { return stylesGateTile_; }
    Button* gateButton() const { return gateButton_; }
    SettingTile* appStylesTile() const { return appStylesTile_; }
    SettingsSwitch* appStylesCheck() const;
    // In app_styles::CATEGORIES order.
    const std::array<StyleCard, 4>& cards() const { return cards_; }
    const StyleCard& card(std::string_view category) const;
    InfoTile* styleOverridesTile() const { return styleOverridesTile_; }
    SearchableComboBox* overridePicker() const { return overridePicker_; }
    ElidingComboBox* overrideCategory() const { return overrideCategory_; }
    RemovableRows* overrideRows() const { return overrideRows_; }
    Button* overrideAdd() const { return overrideAdd_; }

    // The actions behind the page's controls.
    void addExcluded();
    void removeExcluded(const QString& name);
    void addOverride();
    void removeOverride(const QString& match);
    void saveTone(std::string_view category, std::string_view tone);

    // SettingsPageModule: refresh shows the saved settings again.
    void refreshPage() override;

protected:
    void load(const Json& settings) override;
    void sync(const Json& settings) override;

private:
    // _Page.sync: the settings on the page's tiles, with the loading flag held.
    void show(const Json& settings);
    // _saved: saves one key, says "styles" changed when it was saved, and shows the page again.
    void saved(std::string_view key, const Json& value);
    void saveSwitch(std::string_view key, bool on);
    void picked(std::size_t card, int index);

    CleanupGates& gates_;

    SettingTile* appContextTile_ = nullptr;
    SettingTile* readTextTile_ = nullptr;
    WrappedLabel* privacy_ = nullptr;
    InfoTile* excludedAppsTile_ = nullptr;
    SearchableComboBox* excludedPicker_ = nullptr;
    RemovableRows* excludedRows_ = nullptr;
    Button* excludedAdd_ = nullptr;
    InfoTile* stylesCleanupGateTile_ = nullptr;
    InfoTile* stylesGateTile_ = nullptr;
    Button* gateButton_ = nullptr;
    SettingTile* appStylesTile_ = nullptr;
    std::array<StyleCard, 4> cards_;
    InfoTile* styleOverridesTile_ = nullptr;
    SearchableComboBox* overridePicker_ = nullptr;
    ElidingComboBox* overrideCategory_ = nullptr;
    RemovableRows* overrideRows_ = nullptr;
    Button* overrideAdd_ = nullptr;
};

// Registers Apps & styles in the context's Settings pages: title, subtitle, search fields,
// controls and the rail's "Off", "Needs AI cleanup" or tone count. Its cleanup gate comes from
// the context's CleanupGates.
// Affinity: UI actor.
void registerStylesPage(AppContext& context);

// Home's Personalize row for styles (home_rows): "Match tone to each app" with the page's own
// switch projected, disabled with the reason as its tooltip while styles cannot apply, and the
// tone each kind of app gets (or that reason) under it.
// Affinity: GUI thread.
void addStylesHomeRows(HomePage& home);

} // namespace fv
