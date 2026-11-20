// Explicit note requests run without speech and share the scheduler's worker
// (tests/test_meeting_note_requests.py, the scheduler tests; the prompt tests are M8.2's and the
// dashboard route M8.11's).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

#include "agents/loopback_mcp.h"
#include "meeting_agent/scheduler.h"
#include "scheduler_support.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;
using namespace std::chrono_literals;
using P = SchedulerProbe;

// from
// tests/test_meeting_note_requests.py::test_request_runs_without_new_speech_and_preserves_watermarks
TEST_CASE("test_request_runs_without_new_speech_and_preserves_watermarks",
          "[test_meeting_note_requests]") {
    FakeNotesAgent agent;
    auto host = notes_host({segment("sg_old", 10)});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock());
    scheduler.start();
    auto future = scheduler.request_note_adjustment("Shorten the first section");
    REQUIRE(future.wait_for(3s) == std::future_status::ready);
    CHECK(future.get().ok);
    REQUIRE_FALSE(agent.calls().empty());
    const auto payload = agent.calls()[0];
    CHECK(payload.is_notes);
    CHECK(payload.state_snapshot["note_adjustment_request"] == "Shorten the first section");
    CHECK(payload.new_segments[0]["id"] == "sg_old");
    scheduler.stop();
    CHECK(P::sent_starts(scheduler).empty());
    CHECK(P::notes_sent_starts(scheduler).empty());
    CHECK_FALSE(host->store()->snapshot().contains("note_adjustment_request"));
}

// from
// tests/test_meeting_note_requests.py::test_requests_are_serial_and_queued_request_is_settled_on_stop
TEST_CASE("test_requests_are_serial_and_queued_request_is_settled_on_stop",
          "[test_meeting_note_requests]") {
    fv::agents::Event entered;
    auto release = std::make_shared<fv::agents::Event>();
    FakeNotesAgent agent;
    agent.core->set_hook([&entered, release](const CheckpointPayload&, AgentToolHost*) {
        entered.set();
        release->wait(3s);
        AgentResult ok;
        ok.ok = true;
        return ok;
    });
    auto host = notes_host({});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock());
    scheduler.start();
    auto first = scheduler.request_note_adjustment("First");
    REQUIRE(entered.wait(2s));
    auto second = scheduler.request_note_adjustment("Second");
    CHECK(second.wait_for(0s) == std::future_status::timeout);
    scheduler.prepare_for_end();
    REQUIRE(second.wait_for(1s) == std::future_status::ready);
    const auto settled = second.get();
    CHECK_FALSE(settled.ok);
    CHECK(settled.error == "Meeting agent stopped.");
    release->set();
    REQUIRE(first.wait_for(2s) == std::future_status::ready);
    CHECK(first.get().ok);
    CHECK_THROWS_AS(scheduler.request_note_adjustment("Too late"), std::runtime_error);
    scheduler.stop();
}

// from tests/test_meeting_note_requests.py::test_agent_failure_reaches_requester
TEST_CASE("test_agent_failure_reaches_requester", "[test_meeting_note_requests]") {
    FakeNotesAgent agent(1);
    auto host = notes_host({});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock());
    scheduler.start();
    auto future = scheduler.request_note_adjustment("Shorten");
    REQUIRE(future.wait_for(3s) == std::future_status::ready);
    CHECK_FALSE(future.get().ok);
    scheduler.stop();
}

TEST_CASE("note requests are bounded and need a running worker", "[scheduler]") {
    FakeNotesAgent agent;
    auto host = notes_host({});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock());
    const auto message = [&](const std::string& text) -> std::string {
        try {
            (void)scheduler.request_note_adjustment(text);
        } catch (const std::exception& error) {
            return error.what();
        }
        return "";
    };
    CHECK(message("") == "Enter a note request of 1 to 4000 characters.");
    CHECK(message("   \n") == "Enter a note request of 1 to 4000 characters.");
    CHECK(message(std::string(4001, 'x')) == "Enter a note request of 1 to 4000 characters.");
    CHECK_THROWS_AS(scheduler.request_note_adjustment(" "), std::invalid_argument);
    // Code points, not bytes: 4000 "é" are 8000 bytes.
    std::string accents;
    for (int i = 0; i < 4000; ++i) {
        accents += "\xc3\xa9";
    }
    CHECK(message(accents) == "The note agent is not available.");
    CHECK(message("Shorten") == "The note agent is not available.");

    // The worker blocks on the first request, so the next ones queue: at most four.
    auto release = std::make_shared<fv::agents::Event>();
    fv::agents::Event entered;
    agent.core->set_hook([&entered, release](const CheckpointPayload&, AgentToolHost*) {
        entered.set();
        release->wait(3s);
        AgentResult ok;
        ok.ok = true;
        return ok;
    });
    scheduler.start();
    auto running = scheduler.request_note_adjustment("  running  ");
    REQUIRE(entered.wait(2s));
    std::vector<std::future<AgentResult>> queued;
    for (int i = 0; i < 4; ++i) {
        queued.push_back(scheduler.request_note_adjustment("queued"));
    }
    CHECK(message("one too many") == "The note agent already has several requests queued.");
    release->set();
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    CHECK(running.get().ok);
    for (auto& future : queued) {
        REQUIRE(future.wait_for(3s) == std::future_status::ready);
        CHECK(future.get().ok);
    }
    // The stored text is stripped.
    CHECK(agent.calls()[0].state_snapshot["note_adjustment_request"] == "running");
    scheduler.stop();
}

TEST_CASE("a note request says why it could not run", "[scheduler]") {
    FakeNotesAgent agent;
    auto host = notes_host({});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock());
    SECTION("an offline agent") {
        agent.core->set_healthy(false);
        auto request = P::push_note_request(scheduler, "Shorten");
        P::fire_note_request(scheduler);
        CHECK(request.get().error == "The note agent is offline.");
    }
    SECTION("no store") {
        host->set_store(nullptr);
        auto request = P::push_note_request(scheduler, "Shorten");
        P::fire_note_request(scheduler);
        CHECK(request.get().error == "No active meeting notes.");
    }
    SECTION("a stopped scheduler") {
        auto request = P::push_note_request(scheduler, "Shorten");
        scheduler.prepare_for_end();
        // prepare_for_end settled it before the worker could serve it.
        CHECK(request.get().error == "Meeting agent stopped.");
    }
    CHECK(agent.calls().empty());
}
