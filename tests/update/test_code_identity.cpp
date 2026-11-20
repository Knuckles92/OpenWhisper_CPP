// Telling that the program on disk changed under the running app: OpenWhisper's
// tests/test_code_identity.py (the installed-build half; a FioriVoce build has no git checkout).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "download_support.h"
#include "testkit/testkit.h"
#include "update/code_identity.h"

namespace update = fv::update;
using fv::update::CodeIdentity;
using namespace fv::update::test;

namespace {

// Each test starts with nothing remembered and leaves nothing behind.
struct ForgetLaunch {
    ForgetLaunch() { CodeIdentity::forget_launch(); }
    ~ForgetLaunch() { CodeIdentity::forget_launch(); }
};

} // namespace

// from tests/test_code_identity.py::test_an_installed_build_notices_its_program_file_replaced
TEST_CASE("an installed build notices its program file replaced", "[test_code_identity]") {
    ForgetLaunch forget;
    fv::testkit::TempDataRoot root;
    const auto program = root.path() / "fiorivoce.exe";
    write_bytes(program, "version one");
    CodeIdentity::remember_launch(program);
    CHECK_FALSE(CodeIdentity::changed_since_launch());

    // Package managers write the new file beside the old and rename it over.
    const auto staged = root.path() / "fiorivoce.exe.new";
    write_bytes(staged, "version two, longer");
    std::filesystem::rename(staged, program);

    CHECK(CodeIdentity::changed_since_launch());
}

// from
// tests/test_code_identity.py::test_nothing_is_reported_without_a_launch_version_or_a_readable_head
TEST_CASE("nothing is reported without a launch version or a readable head",
          "[test_code_identity]") {
    ForgetLaunch forget;
    fv::testkit::TempDataRoot root;
    const auto program = root.path() / "fiorivoce.exe";
    write_bytes(program, "version one");
    CHECK_FALSE(CodeIdentity::changed_since_launch()); // Nothing remembered.

    CodeIdentity::remember_launch(program);
    std::filesystem::remove(program); // Mid-replace, or the install went away.
    CHECK_FALSE(CodeIdentity::changed_since_launch());

    // A program that could not be read at launch is never reported as changed.
    CodeIdentity::remember_launch(root.path() / "missing.exe");
    write_bytes(root.path() / "missing.exe", "appeared later");
    CHECK_FALSE(CodeIdentity::changed_since_launch());
}

TEST_CASE("the identity is the file index, size and modification time", "[update]") {
    fv::testkit::TempDataRoot root;
    const auto program = root.path() / "fiorivoce.exe";
    write_bytes(program, "twelve bytes");
    const auto identity = update::code_identity(program);
    REQUIRE(identity.has_value());
    const auto first = identity->find(':');
    const auto second = identity->find(':', first + 1);
    REQUIRE(second != std::string::npos);
    CHECK(identity->substr(first + 1, second - first - 1) == "12");
    CHECK(update::code_identity(program) == identity);
    CHECK_FALSE(update::code_identity(root.path() / "absent.exe"));
}
