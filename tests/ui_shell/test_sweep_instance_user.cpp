// The user part of the single-instance names (services/ui_instance.runtime_paths puts
// os.getuid() in the lock folder and the socket name).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include <QLockFile>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "shell_fixture.h"
#include "testkit/testkit.h"
#include "ui_shell/single_instance.h"

using namespace std::chrono_literals;

namespace {

struct Folders {
    fv::testkit::TempDataRoot root;
    std::filesystem::path data = root.path() / "data";
    std::filesystem::path locks = root.path() / "locks";
    fv::SingleInstanceOptions options() const {
        fv::SingleInstanceOptions out;
        out.lockFolder = locks;
        out.handoff = 200ms;
        out.connectTimeout = 50ms;
        return out;
    }
};

} // namespace

TEST_CASE("the instance name carries the account between the prefix and the key", "[platform]") {
    Folders folders;
    const std::string user = fv::instanceUserPart();
    REQUIRE(!user.empty());
    CHECK(user.find_first_of("\\/:") == std::string::npos);
    CHECK(fv::instanceUserPart() == user);
    CHECK(fv::instanceName(folders.data, fv::identity()) ==
          "openwhisper-cpp-ui-" + user + "-" + fv::instanceKey(folders.data));
}

TEST_CASE("another account's lock on the same data folder does not keep this one out",
          "[platform]") {
    Folders folders;
    std::filesystem::create_directories(folders.locks);
    const std::string other =
        "openwhisper-cpp-ui-" + fv::instanceUserPart() + "x-" + fv::instanceKey(folders.data);
    QLockFile held(fv::qt::toQString(fv::path_to_utf8(folders.locks / (other + ".lock"))));
    held.setStaleLockTime(0);
    REQUIRE(held.tryLock(0));

    auto mine = fv::SingleInstance::acquire(folders.data, fv::identity(), folders.options());
    CHECK(mine != nullptr);
}
