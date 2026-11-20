// fiorivoce --diagnostics: the report's "packaged" (services/diagnostics.py: getattr(sys,
// "frozen"), true for every installed build) says whether this copy runs from the folder its
// install registration names. The probes are made by hand, so no case reads the registry but the
// last, which probes the test executable itself (a build tree).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "base/json.h"
#include "base/utf8.h"
#include "testkit/testkit.h"
#include "ui_shell/early_cli.h"
#include "update/check.h"

namespace {

namespace fs = std::filesystem;

fv::update::InstallProbe probe(bool registered, const fs::path& install, const fs::path& running) {
    fv::update::InstallProbe made;
    made.registered = registered;
    made.install_dir = install;
    made.running_dir = running;
    return made;
}

fv::Json readReport(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return fv::Json::parse(
        std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

fv::Json reportFor(const fv::update::InstallProbe& install, const fs::path& output) {
    const std::vector<std::string> args{fv::path_to_utf8(output)};
    REQUIRE(fv::runDiagnostics(args, "fiorivoce.exe", install) == 0);
    return readReport(output);
}

} // namespace

TEST_CASE("the diagnostics report is packaged only in its registered install folder", "[unit]") {
    fv::testkit::TempDataRoot root;
    const fs::path installed = root.path() / "Programs" / "FioriVoce";
    const fs::path elsewhere = root.path() / "build" / "bin";

    CHECK(reportFor(probe(true, installed, installed), root.path() / "a.json")["packaged"] == true);
    // A build tree on a computer that also has the app installed.
    CHECK(reportFor(probe(true, installed, elsewhere), root.path() / "b.json")["packaged"] ==
          false);
    // A copy with no registration (unpacked by hand, or never installed).
    CHECK(reportFor(probe(false, {}, elsewhere), root.path() / "c.json")["packaged"] == false);
}

TEST_CASE("the diagnostics report of a build tree copy is not packaged", "[unit]") {
    fv::testkit::TempDataRoot root;
    const fs::path output = root.path() / "support.json";
    REQUIRE(fv::runDiagnostics(std::vector<std::string>{fv::path_to_utf8(output)},
                               "fiorivoce.exe") == 0);
    CHECK(readReport(output)["packaged"] == false);
}
