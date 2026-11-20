// The API & MCP page's paired-host frame follows a rename of the host
// (tests/test_remote_rename.py::test_the_mcp_page_names_the_host_by_its_new_name). FioriVoce
// names the host in the frame's title where OpenWhisper names it on a host tab
// (differences a17-paired-mcp).

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

#include <QLabel>

#include "mcp_support.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_automation/live_automation_section.h"

using fv::remote::testing::FakeRemoteEngine;
using namespace fvtest;

// from tests/test_remote_rename.py::test_the_mcp_page_names_the_host_by_its_new_name
TEST_CASE("the MCP page names the host by its new name", "[mcp_paired_host][widget]") {
    // Declared first, so it outlives the io pool's last task.
    FakeRemoteEngine engine;
    McpRig rig;
    fv::remote::ClientPairing pairing;
    pairing.host = "10.0.0.2";
    pairing.port = 47821;
    pairing.fingerprint = std::string(64, 'A');
    pairing.host_name = "jed";
    engine.pairing = pairing;
    Held<fv::McpSettingsView> view = rig.makeView();
    view->bindRemote(&engine, &rig.io());
    view->show();
    REQUIRE(waitFor([&] { return view->pairedHost()->isVisibleTo(view.get()); }));
    CHECK(view->pairedHostTitle()->text() == QStringLiteral("Paired computer: jed"));

    // What the service's rename_host does: the new name, the old one kept, and "host_renamed".
    REQUIRE(engine.rename_host("Studio PC").has_value());
    REQUIRE(engine.pairing->paired_name == "jed");
    CHECK(waitFor([&] {
        return view->pairedHostTitle()->text() == QStringLiteral("Paired computer: Studio PC");
    }));
}
