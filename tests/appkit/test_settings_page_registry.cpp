// SettingsPageRegistry: Settings pages by destination key, listed in the rail order of
// appkit/destinations.h whatever order the features register them in, with the rail value,
// search copy and control names each page registers; and the SettingsPageModule hooks.

#include <stdexcept>
#include <string>
#include <vector>

#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include "appkit/destinations.h"
#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/json.h"

namespace {

namespace destinations = fv::destinations;

fv::SettingsPageSpec page(std::string_view destination, const char* title) {
    return fv::SettingsPageSpec{destination, QString::fromUtf8(title), {}, {}};
}

std::vector<std::string> keys(const fv::SettingsPageRegistry& registry) {
    std::vector<std::string> found;
    for (const auto* spec : registry.pages()) {
        found.emplace_back(spec->destination);
    }
    return found;
}

} // namespace

TEST_CASE("Settings pages are listed in rail order") {
    fv::SettingsPageRegistry registry;
    registry.addPage(page(destinations::advanced, "Developer options"));
    registry.addPage(page("fiorivoce_only", "Extra"));
    registry.addPage(page(destinations::recording, "Recording"));
    registry.addPage(page(destinations::overview, "Home"));
    registry.addPage(page(destinations::voice_model, "Voice model"));
    registry.addPage(page(destinations::mcp, "API & MCP"));
    registry.addPage(page(destinations::hooks, "Hooks"));
    CHECK(keys(registry) == std::vector<std::string>{"overview", "recording", "voice_model",
                                                     "hooks", "mcp", "advanced", "fiorivoce_only"});
}

TEST_CASE("Settings pages are found by key or legacy alias") {
    fv::SettingsPageRegistry registry;
    std::string transient = "downloads";
    auto spec = page(transient, "Downloads");
    spec.scroll = false;
    registry.addPage(std::move(spec));
    transient = "overwritten";

    const fv::SettingsPageSpec* downloads = registry.page(destinations::downloads);
    REQUIRE(downloads != nullptr);
    CHECK(downloads->destination == "downloads"); // the registry's own copy
    CHECK(downloads->title == QStringLiteral("Downloads"));
    CHECK_FALSE(downloads->scroll);
    CHECK(registry.page("library") == downloads);
    CHECK(registry.page("engine_downloads") == downloads);
    CHECK(registry.page(destinations::cleanup) == nullptr);

    CHECK_THROWS_MATCHES(
        registry.addPage(page("voice", "Again")), std::invalid_argument,
        Catch::Matchers::Message("A Settings page for 'downloads' is already registered."));
    CHECK_THROWS_AS(registry.addPage(page("", "Nothing")), std::invalid_argument);
}

TEST_CASE("Settings pages keep their rail value, search copy and controls") {
    fv::SettingsPageRegistry registry;
    auto spec = page(destinations::general, "General");
    spec.railValue = [](const fv::Json& settings) {
        return settings.value("auto_paste", true) ? QStringLiteral("Auto-paste on")
                                                  : QStringLiteral("Manual");
    };
    spec.searchFields = {{"auto_paste_tile", QStringLiteral("Paste into the active window"),
                          QStringLiteral("Insert the transcript where you were typing")}};
    spec.helpTexts = {QStringLiteral("Changes save automatically.")};
    spec.controls = {"auto_paste_tile", "auto_paste_check"};
    registry.addPage(std::move(spec));

    const fv::SettingsPageSpec* general = registry.page(destinations::general);
    REQUIRE(general != nullptr);
    REQUIRE(general->railValue);
    CHECK(general->railValue(fv::Json::object()) == QStringLiteral("Auto-paste on"));
    CHECK(general->railValue(fv::Json{{"auto_paste", false}}) == QStringLiteral("Manual"));
    REQUIRE(general->searchFields.size() == 1);
    CHECK(general->searchFields[0].target == "auto_paste_tile");
    CHECK(general->searchFields[0].label == QStringLiteral("Paste into the active window"));
    CHECK(general->helpTexts ==
          std::vector<QString>{QStringLiteral("Changes save automatically.")});
    CHECK(general->controls == std::vector<std::string>{"auto_paste_tile", "auto_paste_check"});

    // A spec written as M0.9's call sites write it has none of them.
    registry.addPage(page(destinations::appearance, "Appearance"));
    const fv::SettingsPageSpec* appearance = registry.page(destinations::appearance);
    REQUIRE(appearance != nullptr);
    CHECK_FALSE(appearance->railValue);
    CHECK(appearance->searchFields.empty());
    CHECK(appearance->helpTexts.empty());
    CHECK(appearance->controls.empty());
}

TEST_CASE("a control name finds its page's destination") {
    fv::SettingsPageRegistry registry;
    auto keys = page("api_keys", "API keys");
    keys.controls = {"api_key_combo", "api_key_edit"};
    registry.addPage(std::move(keys));
    auto recording = page(destinations::recording, "Recording");
    recording.controls = {"audio_device_combo", "api_key_edit"};
    registry.addPage(std::move(recording));

    CHECK(registry.controlDestination("audio_device_combo") == "recording");
    CHECK(registry.controlDestination("api_key_combo") == "api_keys");
    // The last page registered with the name wins (merge_page_module overwrites the entry).
    CHECK(registry.controlDestination("api_key_edit") == "recording");
    CHECK(registry.controlDestination("missing_control").empty());
    CHECK(registry.controlDestination("").empty());
}

namespace {

class RecordingPage final : public fv::SettingsPageModule {
public:
    void loadSettings(const fv::Json& settings) override { loaded = settings; }
    bool saveDrafts(bool ask) override {
        asked = ask;
        return false;
    }
    fv::Json loaded;
    bool asked = false;
};

class PlainPage final : public fv::SettingsPageModule {};

} // namespace

TEST_CASE("a page module answers only the hooks it implements") {
    PlainPage plain;
    fv::SettingsPageModule& hooks = plain;
    hooks.loadSettings(fv::Json::object());
    hooks.refreshPage();
    hooks.cancelCapture();
    hooks.onShown();
    CHECK(hooks.saveDrafts(true));
    CHECK(hooks.namedControl("anything") == nullptr);

    RecordingPage recording;
    fv::SettingsPageModule& module = recording;
    module.loadSettings(fv::Json{{"auto_paste", false}});
    CHECK(recording.loaded == fv::Json{{"auto_paste", false}});
    CHECK_FALSE(module.saveDrafts(true));
    CHECK(recording.asked);
}
