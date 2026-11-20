// The meeting runtime's one step that leaves the process: opening a dashboard in the user's
// browser through Shell::open_url. Tagged [interactive] (it opens the real default browser), so
// only the owner runs it, through the windows-interactive preset.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "net/reactor.h"
#include "process/launcher.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;

TEST_CASE("a past meeting's archive dashboard opens in the real browser",
          "[meeting][runtime][interactive]") {
    fv::net::Reactor reactor;
    const auto shell = fv::make_shell();
    RuntimeRig rig([&reactor, &shell](MeetingRuntimeDeps& deps) {
        deps.reactor = &reactor;
        deps.archive_factory = {};
        deps.open_url = [&shell](std::string_view url) {
            return shell->open_url(url);
        };
    });
    rig.make_ended_meeting("m_browser", false, "completed");
    rig.rt->open_past_meeting("m_browser");
    rig.drain();
    CHECK(rig.got.errors.empty());
    CHECK(rig.got.statuses.back() == "Opening past meeting");
    // The owner sees the archived meeting's dashboard in the browser, served for half a minute.
    std::this_thread::sleep_for(std::chrono::seconds(30));
    rig.rt->cleanup();
}
