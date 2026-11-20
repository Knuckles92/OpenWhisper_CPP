// The Intelligence page inside Settings (tests/test_settings_models.py
// TestCleanupSettingsOwnership): the page hosts the meeting picker, who runs AI insights above
// the Insights model card, and the recall switch; search finds the agent picker.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <QSet>

#include "agents/installed_agents.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "home_support.h"
#include "ui_meeting/intelligence_page.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/text_model_picker.h"

using fvtest::HomeRig;
using fvtest::qs;
namespace dest = fv::destinations;

namespace {

// No agent is looked up: the cached scan finds none.
struct NoScan {
    fv::agent_picker::ScanSeam original = fv::agent_picker::scanSeam();
    NoScan() {
        fv::agent_picker::scanSeam().cached_agents = [] {
            return std::optional<fv::agents::ScanResult>(std::in_place);
        };
    }
    ~NoScan() {
        fv::agent_picker::waitForWorkers(std::chrono::seconds(5));
        fv::agent_picker::scanSeam() = std::move(original);
    }
    NoScan(const NoScan&) = delete;
    NoScan& operator=(const NoScan&) = delete;
};

std::vector<fv::appcore::SearchEntry> agentEntries(fv::SettingsDialog& dialog) {
    std::vector<fv::appcore::SearchEntry> found;
    for (const auto& entry : fv::buildSearchIndex(dialog)) {
        if (entry.title == "Who runs AI insights") {
            found.push_back(entry);
        }
    }
    return found;
}

} // namespace

// from
// tests/test_settings_models.py::TestCleanupSettingsOwnership::test_intelligence_page_hosts_the_meeting_picker_and_agent_core
// (the Agent core combo is not built, M8 change 3; Pi, OpenWhisper's packaged agent, is the
// product's built-in agent here, M8 lead decision 1)
TEST_CASE("test_intelligence_page_hosts_the_meeting_picker_and_agent_core", "[widget]") {
    NoScan scan;
    HomeRig rig(fv::Json::object(), {dest::meeting_intelligence});
    fv::registerIntelligencePage(rig.context());
    fv::SettingsDialog& dialog = rig.dialog();

    // Before the page is built the entry names the picker; activating it builds the page.
    std::vector<fv::appcore::SearchEntry> entries = agentEntries(dialog);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].target == nullptr);
    CHECK(entries[0].target_name == "meeting_agent_picker");

    dialog.selectDestination(qs(dest::meeting_intelligence));
    dialog.show();
    QCoreApplication::processEvents();
    fv::SettingsPage* page = dialog.page(dest::meeting_intelligence);
    REQUIRE(page != nullptr);
    auto* content =
        dynamic_cast<fv::IntelligencePage*>(dialog.pageContent(dest::meeting_intelligence));
    REQUIRE(content != nullptr);
    auto* picker = content->findChild<fv::AgentPicker*>();
    auto* models = content->findChild<fv::TextModelPicker*>();
    auto* tile = content->findChild<fv::InfoTile*>(QStringLiteral("meeting_insights_model_tile"));
    auto* recall =
        content->findChild<fv::SettingsSwitch*>(QStringLiteral("meeting_past_recall_enabled"));
    REQUIRE(picker != nullptr);
    REQUIRE(models != nullptr);
    REQUIRE(tile != nullptr);
    REQUIRE(recall != nullptr);
    CHECK(page->isAncestorOf(models));
    CHECK(page->isAncestorOf(recall));
    CHECK(page->isAncestorOf(picker));
    CHECK(dialog.control("meeting_agent_picker") == picker);

    // Who runs AI insights comes first, above the insights model; the card holds only the
    // picker's model row.
    CHECK_FALSE(tile->isAncestorOf(picker));
    CHECK(tile->isAncestorOf(picker->modelCard()));
    CHECK(picker->mapTo(page, QPoint(0, 0)).y() < tile->mapTo(page, QPoint(0, 0)).y());

    entries = agentEntries(dialog);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].target == static_cast<const void*>(static_cast<const QWidget*>(picker)));
    CHECK(entries[0].destination == dest::meeting_intelligence);
    CHECK(entries[0].keywords.find("claude code") != std::string::npos);
    const QStringList words = fv::qt::toQString(entries[0].keywords).split(QLatin1Char(' '));
    CHECK(words.contains(QStringLiteral("pi")));

    const QString subtitle = dialog.pageSubtitle(dest::meeting_intelligence);
    CHECK(subtitle.contains(QStringLiteral("coding agent")));
    CHECK(subtitle.contains(fv::qt::toQString(fv::identity().product)));
    CHECK_FALSE(subtitle.contains(QStringLiteral("built-in engine")));
    dialog.close();
}
