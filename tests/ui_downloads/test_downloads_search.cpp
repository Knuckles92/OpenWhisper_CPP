// What Settings search finds on the Downloads page (ui_qt/dialogs/settings_search.py
// _catalog_entries): once the page is built, each model row with its size, its usage chip and
// "Downloaded", and each component row with "Installed"; until then the catalogs, without either.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QLabel>
#include <QString>

#include "appcore/settings_search.h"
#include "appkit/destinations.h"
#include "appkit/registries.h"
#include "downloads_page_support.h"
#include "models/downloads_model.h"
#include "ui_downloads/component_row_widget.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_downloads/model_row_widget.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_settings/settings_window.h"
#include "ui_widgets/eliding_label.h"
#include "voice_model_support.h"

using fv::appcore::SearchEntry;
using fv::appcore::SearchKind;
using fvtest::kCpuRuntime;
using fvtest::kCudaRuntime;
using fvtest::kOrukeet;
using fvtest::kParakeet;
using fvtest::kParakeetBytes;
using fvtest::kParakeetRepo;

namespace {

QString q(const std::string& text) {
    return QString::fromStdString(text);
}

std::string u(const QString& text) {
    return text.toStdString();
}

const SearchEntry* find(const std::vector<SearchEntry>& entries,
                        const std::function<bool(const SearchEntry&)>& match) {
    const auto found = std::ranges::find_if(entries, match);
    return found == entries.end() ? nullptr : &*found;
}

const SearchEntry* model(const std::vector<SearchEntry>& entries, const std::string& name) {
    return find(entries, [&name](const SearchEntry& e) { return e.model_name == name; });
}

const SearchEntry* component(const std::vector<SearchEntry>& entries, const std::string& id) {
    return find(entries, [&id](const SearchEntry& e) { return e.component_id == id; });
}

using Badges = std::vector<std::string>;

} // namespace

TEST_CASE("a Downloads page lists no search entries of its own until it is built", "[widget]") {
    fvtest::DownloadsRig rig;
    auto page = rig.page(true);
    CHECK_FALSE(page->isUiBuilt());
    CHECK_FALSE(page->searchEntries().has_value());
    CHECK_FALSE(page->isUiBuilt());
}

TEST_CASE("a built Downloads page's search entries carry sizes and badges", "[widget]") {
    fvtest::DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.components = {fvtest::runtimeInfo(kCpuRuntime),
                      fvtest::runtimeInfo(kCudaRuntime, fv::models::ComponentState::Installed)};
    auto page = rig.page();
    REQUIRE(page->isUiBuilt());

    const std::optional<std::vector<SearchEntry>> entries = page->searchEntries();
    REQUIRE(entries.has_value());
    CHECK(entries->size() == page->rows().size() + page->componentRows().size());
    CHECK(std::ranges::all_of(*entries,
                              [](const SearchEntry& e) { return e.kind == SearchKind::Model; }));

    // The downloaded dictation model: its size on disk, its summary, its usage and Downloaded.
    fv::ModelRowWidget* parakeetRow = page->row(q(kParakeet));
    REQUIRE(parakeetRow != nullptr);
    const QString size = parakeetRow->sizeLabel()->text();
    REQUIRE_FALSE(size.isEmpty());
    REQUIRE(parakeetRow->usageLabel()->isVisibleTo(parakeetRow));
    const std::string usage = u(parakeetRow->usageLabel()->text());
    const SearchEntry* parakeet = model(*entries, kParakeet);
    REQUIRE(parakeet != nullptr);
    CHECK(parakeet->title == "Parakeet TDT 0.6B v3");
    CHECK(parakeet->detail ==
          "Downloads · " + u(size) + " · " + fv::models::model_row_summary(kParakeet));
    CHECK(parakeet->badges == Badges{usage, "Downloaded"});
    CHECK(parakeet->keywords == kParakeet + " Parakeet model");
    CHECK(parakeet->destination == fv::destinations::downloads);
    CHECK(parakeet->icon == "download-blue.svg");

    // A model not downloaded and not in use: its estimated size, no badge.
    const SearchEntry* orukeet = model(*entries, kOrukeet);
    REQUIRE(orukeet != nullptr);
    CHECK(orukeet->badges.empty());
    CHECK(orukeet->detail.starts_with("Downloads · " +
                                      u(page->row(q(kOrukeet))->sizeLabel()->text()) + " · "));

    // Components: Installed only when usable.
    const SearchEntry* cuda = component(*entries, kCudaRuntime);
    REQUIRE(cuda != nullptr);
    CHECK(cuda->title == fvtest::runtimeInfo(kCudaRuntime).display_name);
    CHECK(cuda->detail == "Downloads › Components · " + fvtest::runtimeInfo(kCudaRuntime).summary);
    CHECK(cuda->badges == Badges{"Installed"});
    CHECK(cuda->keywords == "component runtime add-on");
    CHECK(cuda->icon == "box-blue.svg");
    const SearchEntry* cpu = component(*entries, kCpuRuntime);
    REQUIRE(cpu != nullptr);
    CHECK(cpu->badges.empty());
}

TEST_CASE("a component that cannot be described is found by its row's name", "[widget]") {
    fvtest::DownloadsRig rig;
    auto page = rig.page();
    rig.describeFails = true;
    fv::ComponentRowWidget* row = page->componentRow(q(kCudaRuntime));
    REQUIRE(row != nullptr);

    const auto entries = page->searchEntries();
    REQUIRE(entries.has_value());
    const SearchEntry* cuda = component(*entries, kCudaRuntime);
    REQUIRE(cuda != nullptr);
    CHECK(cuda->title == u(row->nameLabel()->text()));
    // f"Downloads › Components · {''}".rstrip(" ·")
    CHECK(cuda->detail == "Downloads › Components");
    CHECK(cuda->badges.empty());
}

TEST_CASE("Settings search reads the catalogs until Downloads is built, then its rows",
          "[widget]") {
    fvtest::WindowRig window(fv::Json{{"selected_model", "parakeet"}});
    fvtest::DownloadsRig pages;
    pages.cache(kParakeetRepo, kParakeetBytes);
    fv::DownloadsPresenter presenter(window.settings(), window.ui(), window.io());
    presenter.setPageOptions([&pages] { return pages.options(); });
    // Home, where the window opens, so Downloads is built only when it is opened.
    fv::SettingsPageSpec home;
    home.destination = fv::destinations::overview;
    home.title = QStringLiteral("Home");
    home.create = [](QWidget* parent) {
        return parent;
    };
    window.context().settingsPages().addPage(std::move(home));
    fv::registerDownloadsPage(window.context(), presenter);
    fv::SettingsWindow settingsWindow(window.context());
    settingsWindow.raiseDialog = [](fv::SettingsDialog&) {
    };
    fv::DownloadsPresenterParts parts;
    parts.settingsWindow = &settingsWindow;
    presenter.attach(parts);
    fv::SettingsDialog& dialog = settingsWindow.ensureDialog();

    std::vector<SearchEntry> index = fv::buildSearchIndex(dialog);
    const SearchEntry* catalog = model(index, kParakeet);
    REQUIRE(catalog != nullptr);
    CHECK(catalog->detail == "Downloads · " + kParakeetRepo);
    CHECK(catalog->badges.empty());

    dialog.selectDestination(QString::fromUtf8(fv::destinations::downloads));
    REQUIRE(presenter.page() != nullptr);
    REQUIRE(presenter.page()->isUiBuilt());

    index = fv::buildSearchIndex(dialog);
    const SearchEntry* built = model(index, kParakeet);
    REQUIRE(built != nullptr);
    CHECK(std::ranges::count(built->badges, std::string("Downloaded")) == 1);
    CHECK(built->detail.starts_with("Downloads · " +
                                    u(presenter.page()->row(q(kParakeet))->sizeLabel()->text())));
    // The page's components are listed too, though the context has no component list.
    CHECK(component(index, kCudaRuntime) != nullptr);
}
