// Search finding the control that shares this computer's history with the paired host. In
// OpenWhisper it is a tile on API & MCP; in OpenWhisper_CPP history is shared only in a session
// opened from Remote engine's "Share history for this session…" (docs/port/differences.md
// a17-direct-history), so search opens that page and marks the tile holding the button.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>

#include <QPushButton>
#include <QTest>

#include "appkit/app_context.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "remote/control.h"
#include "remote/testing/fake_remote_engine.h"
#include "settings/testing/memory_store.h"
#include "settings_support.h"
#include "ui_remote/history_publication.h"
#include "ui_remote/remote_engine_page.h"
#include "ui_remote/remote_feature.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_widgets/setting_tile.h"

namespace dest = fv::destinations;
using fv::appcore::SearchEntry;

// from tests/test_settings_unified.py::TestSearchPalette::
// test_history_sharing_search_opens_and_marks_its_mcp_control
TEST_CASE("searching share history opens Remote engine and marks its history session control",
          "[test_settings_unified][widget]") {
    fv::testkit::TempDataRoot root;
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    fv::MemorySettingsStore settings;
    fv::qt::QtUiExecutor ui(qApp);
    fv::ManualExecutor io;
    fv::FakeClock clock;
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    services.settings = &settings;
    services.ui = &ui;
    services.io = &io;
    services.clock = &clock;
    fv::AppContext context(services);
    auto engine = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    fv::remote::ClientPairing pairing;
    pairing.host = "192.168.1.20";
    pairing.fingerprint = "ab:cd";
    pairing.host_name = "jed";
    pairing.device_id = "laptop";
    engine->pairing = pairing;
    context.provide<fv::remote::RemoteEngineControl>(engine);
    // Never started here: the button only needs a publication to exist.
    context.provide<fv::remote_history::Publication>(
        std::make_shared<fv::remote_history::Publication>(
            io, std::filesystem::path(),
            []() -> std::optional<fv::remote::ClientPairing> { return std::nullopt; },
            fv::remote_history::Publication::Connect{}));
    // The window opens on another page, so Remote engine is built by the search.
    for (const fvtest::Heading& heading : fvtest::headings()) {
        if (heading.key != dest::remote_engine) {
            context.settingsPages().addPage(fvtest::standInSpec(heading));
        }
    }
    fv::registerRemoteEnginePage(context);

    auto window = std::make_unique<fv::SettingsDialog>(context);
    window->availableScreenRect = [] {
        return QRect(0, 0, 1920, 1080);
    };
    fv::SettingsDialog& dialog = *window;
    fv::SearchPalette* palette = fv::installSearchPalette(dialog);
    dialog.show();
    CHECK_FALSE(dialog.isBuilt(dest::remote_engine));
    dialog.openSearch(QStringLiteral("share history"));
    const std::vector<SearchEntry>& entries = palette->currentEntries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().destination == dest::remote_engine);
    CHECK(entries.front().title == "Share history for this session");

    QTest::keyClick(palette->input(), Qt::Key_Return);
    fvtest::processEvents(4);
    CHECK(dialog.currentDestination() ==
          QString::fromUtf8(dest::remote_engine.data(), dest::remote_engine.size()));
    auto* page = dynamic_cast<fv::RemoteEnginePage*>(dialog.pageContent(dest::remote_engine));
    REQUIRE(page != nullptr);
    QWidget* control = dialog.control("remote_publish_history");
    REQUIRE(control != nullptr);
    CHECK(control == page->namedControl("remote_publish_history"));
    CHECK(qobject_cast<QPushButton*>(control)->text() == "Share history for this session…");
    // The tile holding it is marked, as OpenWhisper marks its history tile.
    CHECK(page->clientTile()->isAncestorOf(control));
    CHECK(page->clientTile()->property("searchHit").toBool());
    // The page's pairing arrives from the io executor; then the control shows.
    for (int i = 0; i < 20 && !control->isVisible(); ++i) {
        io.run_all();
        fvtest::processEvents(4);
    }
    CHECK(control->isVisible());
    dialog.close();
}
