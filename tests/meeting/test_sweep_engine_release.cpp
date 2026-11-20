// An engine whose last owner lets go on one of the engine's own threads: MeetingRuntime's
// listener runs inline on End and holds the RuntimeEngine while it queues the event, so the
// release can land there, where ~MeetingEngine would join the thread it runs on.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "engine_support.h"

using fv::meeting::MeetingEngine;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::InlineExecutor;

TEST_CASE("an engine released by its listener on End is destroyed off that thread",
          "[engine_release]") {
    EngineRig rig;
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    MeetingEngine& made = rig.make(setup);
    auto owner = std::make_shared<std::unique_ptr<MeetingEngine>>(rig.take());
    InlineExecutor inline_executor;
    std::promise<std::thread::id> destroyed_on;
    std::atomic<bool> released_on_engine_thread{false};
    std::thread::id end_thread;
    std::once_flag once;
    fv::Subscription subscription =
        made.add_listener(inline_executor, [&](const fv::meeting::MeetingEvent& event) {
            if (event.kind != "ended") {
                return;
            }
            std::call_once(once, [&] {
                end_thread = std::this_thread::get_id();
                released_on_engine_thread = (*owner)->on_engine_thread();
                std::shared_ptr<std::unique_ptr<MeetingEngine>> last = std::move(owner);
                fv::meeting::release_off_engine_threads(**last, [last, &destroyed_on] {
                    last->reset();
                    destroyed_on.set_value(std::this_thread::get_id());
                });
            });
        });

    made.start();
    REQUIRE(made.wait_for_processing(std::chrono::seconds(5)));
    made.end();
    auto destroyed = destroyed_on.get_future();
    REQUIRE(destroyed.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    CHECK(released_on_engine_thread.load());
    CHECK(destroyed.get() != end_thread);
}
