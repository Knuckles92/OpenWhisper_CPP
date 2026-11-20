// walkthrough.py:693 with its unsaved suffix: a screenshot outside a walkthrough that neither
// the clipboard nor the disk took says both.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "runtime_rig.h"
#include "support.h"
#include "walkthrough/storage.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;

TEST_CASE("a screenshot neither copied nor saved says both", "[runtime]") {
    RuntimeRig rig;
    rig.copy_works = false;
    const auto captures = captures_root(rig.paths);
    std::filesystem::create_directories(captures);
    write_file(captures / "screenshots", "not a directory");

    rig.runtime->snap();
    rig.keep();

    CHECK(rig.notice == "Couldn't copy the screenshot (its file couldn't be saved)");
}
