// The Settings search palette (settings_search.py SearchPalette and build_index,
// settings_dialog.py _search_index, _SEARCH_ALIASES and _on_search_activated).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QTest>

#include "home_support.h"
#include "settings/keys.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fv::appcore::SearchEntry;
using fv::appcore::SearchKind;
using fvtest::HomeRig;
using fvtest::qs;
namespace dest = fv::destinations;

namespace {

const SearchEntry* find(const std::vector<SearchEntry>& entries,
                        const std::function<bool(const SearchEntry&)>& match) {
    const auto found = std::ranges::find_if(entries, match);
    return found == entries.end() ? nullptr : &*found;
}

std::string crumbOf(fv::SettingsDialog& dialog, std::string_view key) {
    return fv::qt::toUtf8(QStringLiteral("%1 › %2").arg(dialog.rail()->chapter(qs(key)),
                                                        dialog.rail()->name(qs(key))));
}

// The pages this package's world registers for real, with their registered search copy.
const std::vector<std::string_view>& realPages() {
    static const std::vector<std::string_view> keys{dest::recording, dest::voice_model,
                                                    dest::general,   dest::appearance,
                                                    dest::api_keys,  dest::advanced};
    return keys;
}

} // namespace

// from
// tests/test_settings_unified.py::TestSearchPalette::test_index_covers_tiles_fields_help_models_and_components
TEST_CASE("test_index_covers_tiles_fields_help_models_and_components",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    std::vector<SearchEntry> index = fv::buildSearchIndex(dialog);
    std::set<std::string> titles;
    std::set<SearchKind> kinds;
    for (const SearchEntry& entry : index) {
        titles.insert(entry.title);
        kinds.insert(entry.kind);
    }
    CHECK(kinds == std::set<SearchKind>{SearchKind::Setting, SearchKind::Model, SearchKind::Help});
    CHECK(titles.contains("Paste into the active window"));
    CHECK(std::ranges::any_of(
        titles, [](const std::string& title) { return title.starts_with("Recording engine"); }));
    CHECK(titles.contains("Voice model"));
    CHECK(find(index, [](const SearchEntry& e) { return e.model_name == "parakeet-v3"; }));
    CHECK(find(index, [](const SearchEntry& e) {
        return e.component_id == "asr-nvidia-cuda" &&
               e.detail == "Downloads › Components · Runs Parakeet on an NVIDIA GPU";
    }));
    const SearchEntry* voice =
        find(index, [](const SearchEntry& e) { return e.title.starts_with("Recording engine"); });
    REQUIRE(voice != nullptr);
    CHECK(voice->detail == "Dictation › Voice model");
    const SearchEntry* model =
        find(index, [](const SearchEntry& e) { return e.model_name == "parakeet-v3"; });
    CHECK(model->title == "Parakeet TDT 0.6B v3");
    CHECK(model->detail == "Downloads · nvidia/parakeet-tdt-0.6b-v3");
    CHECK(model->keywords == "parakeet-v3 model");
    CHECK(model->icon == "download-blue.svg");

    // Without a component list, search finds no component.
    rig.components.clear();
    index = fv::buildSearchIndex(dialog);
    CHECK_FALSE(find(index, [](const SearchEntry& e) { return !e.component_id.empty(); }));
}

// from
// tests/test_settings_unified.py::TestSearchPalette::test_ctrl_k_opens_the_palette_and_escape_keeps_settings_open
TEST_CASE("test_ctrl_k_opens_the_palette_and_escape_keeps_settings_open",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    // Offscreen windows never become active, so fire the window shortcut the way Qt would once
    // the key reaches it.
    QShortcut* ctrlK = nullptr;
    for (QShortcut* shortcut : dialog.findChildren<QShortcut*>()) {
        if (shortcut->key() == QKeySequence(QStringLiteral("Ctrl+K"))) {
            ctrlK = shortcut;
        }
    }
    REQUIRE(ctrlK != nullptr);
    Q_EMIT ctrlK->activated();
    CHECK(rig.palette().isVisible());
    CHECK(rig.palette().geometry() == dialog.rect());

    QTest::keyClick(rig.palette().input(), Qt::Key_Escape);
    CHECK_FALSE(rig.palette().isVisible());
    CHECK(dialog.isVisible());
    dialog.close();
}

// from tests/test_settings_unified.py::TestSearchPalette::test_rail_search_button_opens_the_palette
TEST_CASE("test_rail_search_button_opens_the_palette", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.searchButton()->click();
    CHECK(rig.palette().isVisible());
    dialog.close();
}

// from
// tests/test_settings_unified.py::TestSearchPalette::test_enter_opens_the_setting_and_marks_its_tile
TEST_CASE("test_enter_opens_the_setting_and_marks_its_tile", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.openSearch(QStringLiteral("paste into the active"));
    const std::vector<SearchEntry>& entries = rig.palette().currentEntries();
    REQUIRE_FALSE(entries.empty());
    CHECK(entries.front().title == "Paste into the active window");

    QTest::keyClick(rig.palette().input(), Qt::Key_Return);
    fvtest::processEvents();

    CHECK_FALSE(rig.palette().isVisible());
    CHECK(dialog.currentDestination() == qs(dest::general));
    QWidget* tile = dialog.control("auto_paste_tile");
    REQUIRE(tile != nullptr);
    CHECK(tile->property("searchHit").toBool());
    fvtest::wait(fv::SettingsDialog::kSearchFlashMs + 200);
    CHECK_FALSE(tile->property("searchHit").toBool());
    dialog.close();
}

// from tests/test_settings_unified.py::TestSearchPalette::test_arrow_keys_skip_section_headers
TEST_CASE("test_arrow_keys_skip_section_headers", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    // OpenWhisper's query was "tiny", a Whisper model; Parakeet finds a setting, a model and a
    // help note, each under its header.
    dialog.openSearch(QStringLiteral("parakeet"));
    fv::SearchPalette& palette = rig.palette();
    QListWidget* results = palette.results();
    QListWidgetItem* first = results->currentItem();
    REQUIRE(first != nullptr);
    CHECK(palette.entryOf(first) != nullptr);
    CHECK(palette.entryOf(results->item(0)) == nullptr); // the Settings header

    std::set<int> visited{results->currentRow()};
    for (int step = 0; step < results->count() + 2; ++step) {
        QTest::keyClick(palette.input(), Qt::Key_Down);
        REQUIRE(palette.entryOf(results->currentItem()) != nullptr);
        visited.insert(results->currentRow());
    }
    // Every result was reached, wrapping past the end, and no header.
    CHECK(visited.size() == palette.currentEntries().size());
    QTest::keyClick(palette.input(), Qt::Key_Up);
    CHECK(palette.entryOf(results->currentItem()) != nullptr);
    dialog.close();
}

// from
// tests/test_settings_unified.py::TestSearchPalette::test_a_model_result_opens_downloads_on_that_model
TEST_CASE("test_a_model_result_opens_downloads_on_that_model", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.openSearch(QStringLiteral("parakeet-v3"));
    const SearchEntry* entry = find(rig.palette().currentEntries(), [](const SearchEntry& e) {
        return e.model_name == "parakeet-v3";
    });
    REQUIRE(entry != nullptr);
    fv::activateSearchEntry(dialog, *entry);
    CHECK(dialog.currentDestination() == qs(dest::downloads));
    CHECK(rig.downloadCalls == QStringList{QStringLiteral("model parakeet-v3")});

    // A component opens Downloads focused on it.
    rig.downloadCalls.clear();
    dialog.showHome();
    dialog.openSearch(QStringLiteral("cuda"));
    const SearchEntry* component = find(rig.palette().currentEntries(), [](const SearchEntry& e) {
        return e.component_id == "asr-nvidia-cuda";
    });
    REQUIRE(component != nullptr);
    fv::activateSearchEntry(dialog, *component);
    CHECK(dialog.currentDestination() == qs(dest::downloads));
    CHECK(rig.downloadCalls == QStringList{QStringLiteral("component asr-nvidia-cuda")});
    dialog.close();
}

// from
// tests/test_settings_unified.py::TestSearchPalette::test_old_window_names_still_find_their_pages
TEST_CASE("test_old_window_names_still_find_their_pages", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.openSearch(QStringLiteral("model manager"));
    const std::vector<SearchEntry>& entries = rig.palette().currentEntries();
    REQUIRE_FALSE(entries.empty());
    CHECK(entries.front().destination == dest::voice_model);
    dialog.close();
}

TEST_CASE("aliases are listed only for registered pages", "[widget]") {
    HomeRig rig(fv::Json::object(), {dest::mcp, dest::walkthroughs});
    std::vector<SearchEntry> index = fv::buildSearchIndex(rig.dialog());
    CHECK_FALSE(find(index, [](const SearchEntry& e) {
        return e.title == "Connect an agent with MCP" ||
               e.title == "Walkthrough and screenshot settings";
    }));
    const SearchEntry* hotkeys = find(index, [](const SearchEntry& e) {
        return e.title == "Every shortcut: Command Mode, Scratchpad, walkthroughs, profiles, "
                          "actions";
    });
    REQUIRE(hotkeys != nullptr);
    CHECK(hotkeys->destination == dest::hotkeys);
    CHECK(hotkeys->detail == "App › Hotkeys");
    // The meeting agent picker's entry follows Meeting Mode's Intelligence page.
    const SearchEntry* agent =
        find(index, [](const SearchEntry& e) { return e.title == "Who runs AI insights"; });
    REQUIRE(agent != nullptr);
    CHECK(agent->target_name == "meeting_agent_picker");
    CHECK(agent->detail == "Meeting Mode › Intelligence");

    HomeRig without(fv::Json::object(), {dest::meeting_intelligence, dest::downloads});
    index = fv::buildSearchIndex(without.dialog());
    CHECK_FALSE(find(index, [](const SearchEntry& e) {
        return e.title == "Who runs AI insights" || e.title == "Cloud intelligence" ||
               e.kind == SearchKind::Model;
    }));
}

// The demo: Ctrl+K "groq" finds the Voice model page's server field.
TEST_CASE("groq finds the Voice model page's server field", "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.openSearch(QStringLiteral("groq"));
    const SearchEntry* server = find(rig.palette().currentEntries(), [](const SearchEntry& e) {
        return e.target_name == "api_server";
    });
    REQUIRE(server != nullptr);
    CHECK(server->destination == dest::voice_model);
    CHECK(server->title == "API server and model");
    CHECK_FALSE(dialog.isBuilt(dest::voice_model));
    QTest::keyClick(rig.palette().input(), Qt::Key_Return);
    fvtest::processEvents();
    CHECK(dialog.currentDestination() == qs(dest::voice_model));
    dialog.close();
}

TEST_CASE("the palette says what it found", "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    fv::SearchPalette& palette = rig.palette();
    dialog.openSearch();
    CHECK(palette.input()->placeholderText() ==
          QStringLiteral("Search settings, models, and help"));
    CHECK(palette.emptyLabel()->isVisible());
    CHECK(palette.emptyLabel()->text() ==
          QStringLiteral("Type to search every setting, model, and help note."));
    CHECK(palette.countLabel()->text().isEmpty());
    CHECK(palette.panel()->width() == fv::SearchPalette::kPanelWidth);
    CHECK(palette.footer()->isVisible());

    palette.input()->setText(QStringLiteral("  zzqx  "));
    CHECK(palette.emptyLabel()->text() == QStringLiteral("Nothing matches \"zzqx\"."));
    CHECK(palette.countLabel()->text() == QStringLiteral("0 results"));
    CHECK_FALSE(palette.results()->isVisible());

    palette.input()->setText(QStringLiteral("screenshot settings"));
    CHECK(palette.countLabel()->text() == QStringLiteral("1 result"));
    CHECK(palette.results()->isVisible());
    auto* row = qobject_cast<fv::SearchResultRow*>(
        palette.results()->itemWidget(palette.results()->currentItem()));
    REQUIRE(row != nullptr);
    CHECK(row->titleText() == QStringLiteral("Walkthrough and screenshot settings"));
    CHECK(row->titleLabel()->toolTip() == QStringLiteral("Walkthrough and screenshot settings"));

    // A click outside the panel closes the palette, not Settings.
    QTest::mouseClick(&palette, Qt::LeftButton, {}, QPoint(4, palette.height() - 4));
    CHECK_FALSE(palette.isVisible());
    CHECK(dialog.isVisible());

    // Narrow and short, the footer goes.
    dialog.resize(fv::SettingsDialog::kMinimumSize.width(), 520);
    dialog.openSearch(QStringLiteral("mic"));
    CHECK(palette.panel()->width() <= fv::SearchPalette::kPanelWidth);
    dialog.close();
}

// from
// tests/test_desktop_performance.py::test_settings_first_show_and_search_do_not_build_hidden_destinations
TEST_CASE("test_settings_first_show_and_search_do_not_build_hidden_destinations",
          "[test_desktop_performance][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    fvtest::processEvents();
    const std::vector<std::string> home{std::string(dest::overview)};
    CHECK(rig.builtPages() == home);
    std::vector<SearchEntry> entries = fv::buildSearchIndex(dialog);
    // OpenWhisper looked for an AI cleanup field ("Custom prompt (optional)", M4); General's is
    // the unbuilt page this build has.
    CHECK(find(entries, [](const SearchEntry& e) {
        return e.title == "Paste into the active window" && e.target_name == "auto_paste_tile";
    }));
    CHECK(find(entries, [](const SearchEntry& e) { return e.model_name == "parakeet-v3"; }));
    CHECK(rig.builtPages() == home);
    // Home must not ask an audio driver.
    fvtest::processEvents(3);
    CHECK(rig.listings == 0);
    dialog.close();
}

// from tests/test_personalize_s7_settings.py::test_search_finds_both_microphone_tiles
TEST_CASE("test_search_finds_both_microphone_tiles", "[test_personalize_s7_settings][widget]") {
    HomeRig rig(fv::Json{{"audio_input_priority", fv::Json::array({fvtest::kUsb})}});
    fv::SettingsDialog& dialog = rig.dialog();
    std::map<std::string, std::string> found;
    for (const SearchEntry& entry : fv::buildSearchIndex(dialog)) {
        found.emplace(entry.title, entry.destination);
    }
    CHECK(found["Preferred microphone"] == dest::recording);
    CHECK(found["If it's unplugged, use"] == dest::recording);
    // Before the page is built, search reads its registered copy, which names the tiles.
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::recording);
    REQUIRE(spec != nullptr);
    std::map<std::string, QString> unbuilt;
    for (const fv::SettingsSearchField& field : spec->searchFields) {
        unbuilt.emplace(field.target, field.label);
    }
    for (const char* name : {"audio_device_tile", "microphone_backup_tile"}) {
        auto* tile = qobject_cast<fv::TileBase*>(dialog.control(name));
        REQUIRE(tile != nullptr);
        CHECK(unbuilt[name] == tile->titleLabel()->text());
    }
    // Built, the tiles themselves are found.
    std::vector<SearchEntry> index = fv::buildSearchIndex(dialog);
    const SearchEntry* preferred = find(index, [](const SearchEntry& e) {
        return e.title == "Preferred microphone" && e.target != nullptr;
    });
    REQUIRE(preferred != nullptr);
    CHECK(preferred->target == dialog.control("audio_device_tile"));
}

// from tests/test_recording_look.py::test_settings_search_finds_the_overlay_look
TEST_CASE("test_settings_search_finds_the_overlay_look", "[test_recording_look][widget]") {
    HomeRig rig;
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::appearance);
    REQUIRE(spec != nullptr);
    CHECK(std::ranges::any_of(spec->searchFields, [](const fv::SettingsSearchField& field) {
        return field.label == QStringLiteral("Recording overlay");
    }));
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.openSearch(QStringLiteral("recording overlay"));
    const SearchEntry* look = find(rig.palette().currentEntries(), [](const SearchEntry& e) {
        return e.title == "Recording overlay";
    });
    REQUIRE(look != nullptr);
    fv::activateSearchEntry(dialog, *look);
    fvtest::processEvents();
    QWidget* tile = dialog.control("recording_look_tile");
    REQUIRE(tile != nullptr);
    CHECK(tile->isVisible());
    dialog.close();
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPersonalizeRail::test_each_page_builds_the_tiles_its_search_copy_names
TEST_CASE("test_each_page_builds_the_tiles_its_search_copy_names",
          "[test_personalize_foundation_ui_settings][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    for (const std::string_view key : realPages()) {
        INFO(key);
        const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(key);
        REQUIRE(spec != nullptr);
        dialog.selectDestination(qs(key));
        CHECK(dialog.isBuilt(key));
        CHECK(dialog.pageTitleLabel()->text() == spec->title);
        for (const fv::SettingsSearchField& field : spec->searchFields) {
            INFO(field.target);
            QWidget* widget = dialog.control(field.target);
            REQUIRE(widget != nullptr);
            CHECK(dialog.page(key)->isAncestorOf(widget));
            if (auto* tile = qobject_cast<fv::TileBase*>(widget)) {
                CHECK(tile->titleLabel()->text() == field.label);
            }
        }
        for (const std::string& name : spec->controls) {
            INFO(name);
            CHECK(dialog.control(name) != nullptr);
        }
    }
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPersonalizeRail::test_pages_are_searchable_before_they_are_built
TEST_CASE("test_pages_are_searchable_before_they_are_built",
          "[test_personalize_foundation_ui_settings][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    for (const std::string_view key : realPages()) {
        INFO(key);
        const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(key);
        REQUIRE(spec != nullptr);
        REQUIRE_FALSE(spec->searchFields.empty());
        const fv::SettingsSearchField& field = spec->searchFields.front();
        std::vector<SearchEntry> entries;
        for (const SearchEntry& entry : fv::buildSearchIndex(dialog)) {
            if (entry.destination == key && entry.target_name == field.target) {
                entries.push_back(entry);
            }
        }
        REQUIRE(entries.size() == 1);
        CHECK(qs(entries.front().title) == field.label);
        CHECK(entries.front().detail == crumbOf(dialog, key));
        CHECK_FALSE(dialog.isBuilt(key));
        fv::activateSearchEntry(dialog, entries.front());
        fvtest::processEvents();
        CHECK(dialog.currentDestination() == qs(key));
        // The reveal tints the tile that holds the control.
        QWidget* card = dialog.control(field.target);
        while (card != nullptr && card->objectName() != QStringLiteral("settingsTile")) {
            card = card->parentWidget();
        }
        if (card != nullptr) {
            CHECK(card->property("searchHit").toBool());
        }
    }
    dialog.close();
}

TEST_CASE("a tile marked as a choice is not a setting search finds", "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.ensurePage(dest::general);
    auto* gallery = new QWidget(dialog.page(dest::general));
    gallery->setProperty("settingsSearch", false);
    new fv::InfoTile(QStringLiteral("A starting point zebra"), QString(), {}, gallery);
    new fv::InfoTile(QStringLiteral("Second tile"), QStringLiteral("again"), {},
                     dialog.page(dest::general));
    const std::vector<SearchEntry> index = fv::buildSearchIndex(dialog);
    CHECK_FALSE(
        find(index, [](const SearchEntry& e) { return e.title == "A starting point zebra"; }));
    // Results are de-duplicated by (title, page).
    new fv::InfoTile(QStringLiteral("Second tile"), QStringLiteral("again"), {},
                     dialog.page(dest::general));
    const auto count =
        std::ranges::count_if(fv::buildSearchIndex(dialog), [](const SearchEntry& e) {
            return e.title == "Second tile" && e.destination == dest::general;
        });
    CHECK(count == 1);
}

// A11-35: the result row (settings_search.py _ResultRow): a 28 px icon chip, the title and
// detail with tooltips, a help text cut to its snippet, "Downloaded" and "Installed" pills read
// as ok and any other as use, and the row leaves the clicks to its list item.
TEST_CASE("a result row shows its chip, snippet, tooltips and badge tones", "[widget]") {
    SearchEntry model;
    model.kind = SearchKind::Model;
    model.title = "Parakeet TDT 0.6B v3";
    model.detail = "Downloads · nvidia/parakeet";
    model.badges = {"In use", "Downloaded"};
    model.icon = "download-blue.svg";
    const std::vector<std::string> tokens{"parakeet"};
    fv::SearchResultRow row(model, tokens);
    CHECK(row.objectName() == QStringLiteral("settingsSearchRow"));
    CHECK(row.testAttribute(Qt::WA_TransparentForMouseEvents));
    auto* chip = row.findChild<QLabel*>(QStringLiteral("settingsSearchIcon"));
    REQUIRE(chip != nullptr);
    CHECK(chip->size() == QSize(28, 28));
    CHECK(chip->minimumSize() == QSize(28, 28));
    CHECK(row.titleLabel()->toolTip() == QStringLiteral("Parakeet TDT 0.6B v3"));
    CHECK(row.detailLabel()->toolTip() == QStringLiteral("Downloads · nvidia/parakeet"));
    CHECK(row.titleLabel()->textFormat() == Qt::RichText);
    QStringList badges;
    QStringList tones;
    for (QLabel* pill : row.findChildren<QLabel*>(QStringLiteral("settingsSearchBadge"))) {
        badges.append(pill->text());
        tones.append(pill->property("tone").toString());
    }
    CHECK(badges == QStringList{QStringLiteral("In use"), QStringLiteral("Downloaded")});
    CHECK(tones == QStringList{QStringLiteral("use"), QStringLiteral("ok")});

    SearchEntry help;
    help.kind = SearchKind::Help;
    help.title = "Voice model";
    help.detail = std::string(300, 'a') + " parakeet " + std::string(300, 'b');
    help.icon = "info-blue.svg";
    fv::SearchResultRow helpRow(help, tokens);
    CHECK(helpRow.detailText() == fvtest::qs(fv::appcore::snippet(help.detail, tokens)));
    CHECK(helpRow.detailText().size() < 140);
    CHECK(helpRow.detailText().contains(QStringLiteral("parakeet")));
    CHECK(helpRow.findChildren<QLabel*>(QStringLiteral("settingsSearchBadge")).isEmpty());
}
