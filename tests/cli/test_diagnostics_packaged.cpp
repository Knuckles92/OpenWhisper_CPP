// The diagnostics report's "packaged" (services/diagnostics.py: getattr(sys, "frozen"), true for
// every installed build): a copy is packaged when it runs from the folder its install
// registration names. The probes here are made by hand, so no test reads the registry.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "base/json.h"
#include "cli/commands.h"
#include "cli_process.h"
#include "testkit/testkit.h"
#include "update/check.h"

using fv::Json;
using fv::clitest::read_text;
using fv::clitest::run_cli;

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fv::update::InstallProbe probe(bool registered, const fs::path& install, const fs::path& running) {
    fv::update::InstallProbe made;
    made.registered = registered;
    made.install_dir = install;
    made.running_dir = running;
    return made;
}

// The report run_diagnostics writes for `install`, read back.
Json report_for(const fv::update::InstallProbe& install, const fs::path& output) {
    const std::vector<std::string> args{utf8(output)};
    REQUIRE(fv::cli::run_diagnostics(args, install) == 0);
    return Json::parse(read_text(output));
}

} // namespace

TEST_CASE("the cli diagnostics report is packaged only in its registered install folder",
          "[test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path installed = root.path() / "Programs" / "OpenWhisper_CPP";
    const fs::path elsewhere = root.path() / "build" / "bin";

    CHECK(report_for(probe(true, installed, installed), root.path() / "a.json")["packaged"] ==
          true);
    // A build tree on a computer that also has the app installed.
    CHECK(report_for(probe(true, installed, elsewhere), root.path() / "b.json")["packaged"] ==
          false);
    // A copy with no registration (unpacked by hand, or never installed).
    CHECK(report_for(probe(false, {}, elsewhere), root.path() / "c.json")["packaged"] == false);
}

TEST_CASE("openwhisper-cpp-cli in the build tree reports itself not packaged",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path output = root.path() / "support.json";

    const auto result =
        run_cli({"--diagnostics", utf8(output)}, {{"OPENWHISPER_CPP_DATA_DIR", utf8(root.path())}});

    REQUIRE(result.exit_code == 0);
    // The test build runs from the build tree, never from a registered install folder.
    CHECK(Json::parse(read_text(output))["packaged"] == false);
}
