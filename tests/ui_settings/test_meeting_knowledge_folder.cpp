// The knowledge folder saved through Settings (settings_dialog.py _browse_context_folder and
// _persist_many): a failed save says why in the dialog's message line.

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <QDir>
#include <QLineEdit>
#include <QPushButton>

#include "agents/installed_agents.h"
#include "appkit/qt_bridge.h"
#include "home_support.h"
#include "ui_meeting/intelligence_page.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/settings_switch.h"

using fvtest::HomeRig;
using fvtest::qs;
namespace dest = fv::destinations;

TEST_CASE("a knowledge folder that cannot be saved says why in the dialog", "[widget]") {
    auto original = fv::agent_picker::scanSeam();
    struct Restore {
        fv::agent_picker::ScanSeam original;
        ~Restore() {
            fv::agent_picker::scanSeam() = std::move(original);
            fv::IntelligencePage::folderChooserSeam() = {};
        }
    } reset{std::move(original)};
    fv::agent_picker::scanSeam().cached_agents = [] {
        return std::optional<fv::agents::ScanResult>(std::in_place);
    };
    QString start;
    fv::IntelligencePage::folderChooserSeam() = [&start](QWidget*, const QString&,
                                                         const QString& from) {
        start = from;
        return QStringLiteral("/example/notes/");
    };
    HomeRig rig(fv::Json{{"meeting_context_folder_path", "/example/missing"}},
                {dest::meeting_intelligence});
    fv::registerIntelligencePage(rig.context());
    rig.dialog().selectDestination(qs(dest::meeting_intelligence));
    auto* page =
        dynamic_cast<fv::IntelligencePage*>(rig.dialog().pageContent(dest::meeting_intelligence));
    REQUIRE(page != nullptr);
    auto* browse = page->findChild<QPushButton*>(QStringLiteral("browseMeetingKnowledgeFolder"));
    auto* folder = qobject_cast<QLineEdit*>(page->namedControl("meeting_context_folder_path"));
    REQUIRE(browse != nullptr);
    REQUIRE(folder != nullptr);

    rig.settings().file().fail_next_replace("Disk full");
    browse->click();
    CHECK(start == QDir::homePath());
    CHECK(rig.dialog().messageText() == QStringLiteral("Couldn't save settings: Disk full"));
    CHECK(rig.saved("meeting_context_folder_path") == "/example/missing");
    CHECK(folder->text() == QStringLiteral("/example/missing"));

    browse->click();
    CHECK(rig.dialog().messageText().isEmpty());
    CHECK(rig.saved("meeting_context_folder_path") ==
          fv::qt::toUtf8(QDir::toNativeSeparators(QStringLiteral("/example/notes"))));
    CHECK(rig.saved("meeting_context_folder_enabled") == true);
    CHECK(folder->text() == QDir::toNativeSeparators(QStringLiteral("/example/notes")));
}
