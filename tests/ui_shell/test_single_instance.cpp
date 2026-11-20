// One desktop UI per data folder. Ported from tests/test_ui_instance.py, which ran on Linux only;
// OpenWhisper_CPP locks on every platform (architecture 12.3), so the Linux cases run here, and the
// golden vectors of services.ui_instance._data_key pin the key.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <QApplication>
#include <QLocalSocket>
#include <QLockFile>

#include "appkit/qt_bridge.h"
#include "base/data_lease.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/log.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "shell_fixture.h"
#include "testkit/testkit.h"
#include "ui_shell/single_instance.h"

using fv::SingleInstance;
using fv::SingleInstanceOptions;
using namespace std::chrono_literals;

namespace {

// A lock folder of the test's own, so nothing touches the user's temporary folder.
struct Scratch {
    fv::testkit::TempDataRoot root;
    std::filesystem::path locks = root.path() / "run";
    std::filesystem::path data = root.path() / "data";

    SingleInstanceOptions options(std::chrono::milliseconds handoff = 2000ms) const {
        SingleInstanceOptions made;
        made.lockFolder = locks;
        made.handoff = handoff;
        return made;
    }
};

class CapturedLog final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        lines.push_back(std::string(record.message));
    }
    std::vector<std::string> lines;
};

} // namespace

// from tests/test_ui_instance.py::test_non_linux_stays_primary_without_locking (adapted: every
// platform now locks, so the first launch holds a lock and a second one in the same process is
// not primary)
TEST_CASE("non linux stays primary without locking", "[test_ui_instance][platform]") {
    Scratch scratch;
    auto first = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options(200ms));
    REQUIRE(first != nullptr);
    CHECK(std::filesystem::exists(first->lockPath()));
    CHECK(SingleInstance::acquire(scratch.data, fv::identity(), scratch.options(200ms)) == nullptr);
    auto other =
        SingleInstance::acquire(scratch.data / "other", fv::identity(), scratch.options(200ms));
    CHECK(other != nullptr);
}

// from
// tests/test_ui_instance.py::test_second_launch_asks_the_first_to_show_and_does_not_become_primary
TEST_CASE("second launch asks the first to show and does not become primary",
          "[test_ui_instance][platform]") {
    Scratch scratch;
    auto primary = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options());
    REQUIRE(primary != nullptr);
    primary->listen();
    int shown = 0;
    primary->attachActivate([&shown] { ++shown; });

    auto log = std::make_shared<CapturedLog>();
    fv::log::add_sink(log);
    auto second = fvtest::onOtherThread(
        [&] { return SingleInstance::acquire(scratch.data, fv::identity(), scratch.options()); });
    fv::log::remove_sink(log);
    CHECK(second == nullptr);
    CHECK(fvtest::waitUntil([&shown] { return shown == 1; }, 2000ms));
    CHECK(std::ranges::find(log->lines, "OpenWhisper_CPP is already running; bringing the existing "
                                        "window forward") != log->lines.end());
}

// from tests/test_ui_instance.py::test_a_show_request_calls_the_activate_callback
TEST_CASE("a show request calls the activate callback", "[test_ui_instance][platform]") {
    Scratch scratch;
    auto primary = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options());
    REQUIRE(primary != nullptr);
    primary->listen();
    int shown = 0;
    primary->attachActivate([&shown] { ++shown; });
    const QString server = primary->serverName();
    CHECK(fvtest::onOtherThread([&server] { return SingleInstance::requestShow(server); }));
    CHECK(fvtest::waitUntil([&shown] { return shown == 1; }, 2000ms));
}

TEST_CASE("a show request before the callback is attached is replayed", "[platform]") {
    Scratch scratch;
    auto primary = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options());
    REQUIRE(primary != nullptr);
    primary->listen();
    const QString server = primary->serverName();
    CHECK(fvtest::onOtherThread([&server] { return SingleInstance::requestShow(server); }));
    REQUIRE(fvtest::waitUntil([&primary] { return primary->showRequested(); }, 2000ms));
    int shown = 0;
    primary->attachActivate([&shown] { ++shown; });
    CHECK(shown == 1);
}

TEST_CASE("a request that is not show calls nothing", "[platform]") {
    Scratch scratch;
    auto primary = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options());
    REQUIRE(primary != nullptr);
    primary->listen();
    int shown = 0;
    primary->attachActivate([&shown] { ++shown; });
    const QString server = primary->serverName();
    // The write completes only once the server has read the line.
    CHECK(fvtest::onOtherThread([&server] {
        QLocalSocket socket;
        socket.connectToServer(server);
        if (!socket.waitForConnected(400) || socket.write("hide\n") != 5) {
            return false;
        }
        const bool written = socket.waitForBytesWritten(2000);
        socket.disconnectFromServer();
        return written;
    }));
    // A show request after it is the only one that counts.
    CHECK(fvtest::onOtherThread([&server] { return SingleInstance::requestShow(server); }));
    CHECK(fvtest::waitUntil([&shown] { return shown == 1; }, 2000ms));
    fvtest::waitUntil([] { return false; }, 200ms);
    CHECK(shown == 1);
}

// from tests/test_ui_instance.py::test_different_data_directories_can_each_have_a_ui
TEST_CASE("different data directories can each have a ui", "[test_ui_instance][platform]") {
    Scratch scratch;
    auto first =
        SingleInstance::acquire(scratch.root.path() / "one", fv::identity(), scratch.options());
    auto other =
        SingleInstance::acquire(scratch.root.path() / "two", fv::identity(), scratch.options());
    CHECK(first != nullptr);
    CHECK(other != nullptr);
    CHECK(first->serverName() != other->serverName());
}

// from tests/test_ui_instance.py::test_a_new_process_can_become_primary_after_release
TEST_CASE("a new process can become primary after release", "[test_ui_instance][platform]") {
    Scratch scratch;
    auto first = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options());
    REQUIRE(first != nullptr);
    first->listen();
    first.reset();
    auto next = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options(200ms));
    CHECK(next != nullptr);
}

// from tests/test_ui_instance.py::test_lock_holder_without_a_socket_blocks_a_second_copy
TEST_CASE("lock holder without a socket blocks a second copy", "[test_ui_instance][platform]") {
    Scratch scratch;
    std::filesystem::create_directories(scratch.locks);
    const std::string name = fv::instanceName(scratch.data, fv::identity());
    QLockFile held(fv::qt::toQString(fv::path_to_utf8(scratch.locks / (name + ".lock"))));
    held.setStaleLockTime(0);
    REQUIRE(held.tryLock(0));

    auto log = std::make_shared<CapturedLog>();
    fv::log::add_sink(log);
    const auto started = std::chrono::steady_clock::now();
    auto second = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options(200ms));
    fv::log::remove_sink(log);
    CHECK(second == nullptr);
    CHECK(std::chrono::steady_clock::now() - started >= 200ms);
    CHECK(
        std::ranges::find(
            log->lines, "OpenWhisper_CPP is already running but did not accept the show request") !=
        log->lines.end());
}

TEST_CASE("the server and lock are named from the identity and the key", "[platform]") {
    Scratch scratch;
    auto primary = SingleInstance::acquire(scratch.data, fv::identity(), scratch.options());
    REQUIRE(primary != nullptr);
    const std::string key = fv::instanceKey(scratch.data);
    CHECK(key.size() == 16);
    const std::string user = fv::instanceUserPart();
    CHECK(!user.empty());
    CHECK(primary->serverName() ==
          QString::fromStdString("openwhisper-cpp-ui-" + user + "-" + key));
    CHECK(primary->lockPath() ==
          scratch.locks / ("openwhisper-cpp-ui-" + user + "-" + key + ".lock"));
}

TEST_CASE("a lock that is a symbolic link is refused", "[platform]") {
    Scratch scratch;
    std::filesystem::create_directories(scratch.locks);
    const std::string name = fv::instanceName(scratch.data, fv::identity());
    const auto target = scratch.root.path() / "elsewhere.lock";
    { std::ofstream(target) << "x"; }
    std::error_code error;
    std::filesystem::create_symlink(target, scratch.locks / (name + ".lock"), error);
    if (error) {
        SKIP("This account may not create symbolic links: " + error.message());
    }
    CHECK_THROWS_AS(SingleInstance::acquire(scratch.data, fv::identity(), scratch.options()),
                    std::runtime_error);
}

TEST_CASE("spellings of one data folder share a key and a relative one resolves here", "[unit]") {
    const std::filesystem::path here = std::filesystem::current_path();
    CHECK(fv::instanceKey("fv-data") == fv::instanceKey(here / "fv-data"));
#if defined(_WIN32)
    // normcase folds case and slashes on Windows only.
    CHECK(fv::instanceKey(R"(C:\Data)") == fv::instanceKey("c:/data/"));
    CHECK(fv::instanceKey(R"(C:\Data)") != fv::instanceKey(R"(C:\Data2)"));
#else
    CHECK(fv::instanceKey("/data") != fv::instanceKey("/Data"));
#endif
}

// The vectors were made by Windows Python: drive letters, backslashes and normcase's folding
// mean nothing to POSIX path rules.
#if defined(_WIN32)
TEST_CASE("golden services.ui_instance _data_key", "[golden]") {
    fv::golden::for_each("services.ui_instance", "_data_key", [](const fv::golden::Case& c) {
        CHECK(fv::instanceKey(fv::path_from_utf8(c.arg<std::string>(0))) ==
              c.result<std::string>());
    });
    // A relative folder resolves against the working directory: the vector holds what it
    // normalized to, relative to that directory, and whether its key is the key of the folder
    // spelled from there.
    fv::golden::for_each(
        "services.ui_instance", "_data_key_relative", [](const fv::golden::Case& c) {
            const std::string relative = c.arg<std::string>(0);
            const fv::Json result = c.result<fv::Json>();
            REQUIRE(result.size() == 2);
            const std::filesystem::path here = std::filesystem::current_path();
            const std::filesystem::path resolved =
                fv::normalize_data_root(fv::path_from_utf8(relative));
            CHECK(fv::path_to_utf8(resolved.lexically_relative(fv::normalize_data_root(here))) ==
                  result[0].get<std::string>());
            CHECK((fv::instanceKey(fv::path_from_utf8(relative)) ==
                   fv::instanceKey(here / fv::path_from_utf8(relative))) == result[1].get<bool>());
        });
}
#endif
