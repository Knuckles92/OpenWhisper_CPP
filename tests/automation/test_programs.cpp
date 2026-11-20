#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "automation/event_bus.h"
#include "automation/programs.h"
#include "automation/testing/fake_hook_engines.h"
#include "base/identity.h"
#include "settings/testing/memory_store.h"
#include "support/hook_support.h"

// tests/test_automation_hooks_programs.py, the program hook tests (its run_once tests are
// tests/process/test_run_once.cpp): real child processes, which here are
// fv_automation_hook_child in place of `python -c ...`.

using namespace fv::automation;
using fv::Json;
using test::wait_until;
using namespace std::chrono_literals;

namespace {

std::string env_name(std::string_view prefix, std::string_view name) {
    return std::string(prefix) + std::string(name);
}

// RECORD_RUN: appends one JSON line per run with its stdin and the event variables, the new
// names and the legacy ones.
std::vector<std::string> record_run(const std::string& file) {
    const std::string_view current = fv::identity().env_prefix;
    const std::string_view legacy = fv::legacy_identity().env_prefix;
    return {"read",
            "record",
            file,
            env_name(current, "EVENT_TYPE"),
            env_name(current, "EVENT_ID"),
            env_name(current, "EVENT_SEQ"),
            env_name(legacy, "EVENT_TYPE"),
            env_name(legacy, "EVENT_ID"),
            env_name(legacy, "EVENT_SEQ")};
}

Json program(const std::string& id, const std::vector<std::string>& commands,
             const Json& extra = Json::object()) {
    Json row = {
        {"id", id}, {"name", "Program " + id}, {"program", test::hook_child()}, {"args", commands}};
    for (const auto& [key, value] : extra.items()) {
        row[key] = value;
    }
    return row;
}

std::vector<Json> lines(const std::string& file) {
    std::vector<Json> parsed;
    for (const auto& line : test::read_lines(file)) {
        parsed.push_back(Json::parse(line));
    }
    return parsed;
}

struct Programs {
    explicit Programs(ProgramOptions options = {})
        : engine(std::make_unique<ProgramEngine>(settings, bus, std::move(options))) {}
    ~Programs() {
        engine.reset();
        bus.close();
    }
    Programs(const Programs&) = delete;
    Programs& operator=(const Programs&) = delete;

    // save_programs: each row enabled, following recording.started, 30 s, unless it says
    // otherwise.
    void save(const std::vector<Json>& programs) {
        Json rows = Json::array();
        for (const Json& item : programs) {
            Json row = {{"enabled", true}, {"events", {"recording.started"}}, {"timeout_s", 30}};
            for (const auto& [key, value] : item.items()) {
                row[key] = value;
            }
            rows.push_back(row);
        }
        settings.update(Json{{"automation_programs", rows}});
    }
    Event publish(const std::string& type, Json data) {
        return bus.publish_event(type, std::move(data));
    }
    RunRecord run_test(const std::string& id) {
        auto answer = std::make_shared<std::promise<RunRecord>>();
        auto future = answer->get_future();
        engine->run_test(id, [answer](RunRecord record) { answer->set_value(record); });
        REQUIRE(future.wait_for(20s) == std::future_status::ready);
        return future.get();
    }

    test::TempFolder folder;
    fv::MemorySettingsStore settings;
    EventBus bus;
    std::unique_ptr<ProgramEngine> engine;
};

} // namespace

TEST_CASE("test_a_program_gets_the_event_on_stdin_and_in_its_environment",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    const std::string out = f.folder.file("runs.jsonl");
    f.save({program("p", record_run(out))});
    f.engine->start();
    CHECK(f.bus.subscribers("programs") == 1);
    const Event event = f.publish("recording.started", {{"job_id", "abc"}, {"mode", "dictation"}});

    REQUIRE(wait_until([&] { return lines(out).size() == 1; }, 15s));
    const Json run = lines(out)[0];
    CHECK(run["stdin"] == event.to_json() + "\n");
    for (const std::string_view prefix :
         {fv::identity().env_prefix, fv::legacy_identity().env_prefix}) {
        CHECK(run["env"][env_name(prefix, "EVENT_TYPE")] == "recording.started");
        CHECK(run["env"][env_name(prefix, "EVENT_ID")] == event.id);
        CHECK(run["env"][env_name(prefix, "EVENT_SEQ")] == std::to_string(event.seq));
    }
    REQUIRE(wait_until([&] { return !f.engine->runs("p").empty(); }, 15s));
    const RunRecord record = f.engine->runs("p")[0];
    CHECK(record.event_id == event.id);
    CHECK(record.ok);
    CHECK(record.exit_code == 0);
    CHECK(record.error.empty());
}

TEST_CASE("test_only_enabled_programs_run_and_only_on_their_events",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    const std::string started = f.folder.file("started");
    const std::string stopped = f.folder.file("stopped");
    const std::string off = f.folder.file("off");
    f.save({program("a", record_run(started)),
            program("b", record_run(stopped), {{"events", {"recording.stopped"}}}),
            program("c", record_run(off), {{"enabled", false}})});
    f.engine->start();
    f.engine->start();
    CHECK(f.bus.subscribers("programs") == 1);
    f.publish("recording.started", {{"job_id", "1"}});
    f.publish("recording.stopped", {{"job_id", "1"}, {"duration_s", 1}});

    REQUIRE(
        wait_until([&] { return lines(started).size() == 1 && lines(stopped).size() == 1; }, 15s));
    test::sleep_ms(500);
    CHECK(Json::parse(lines(started)[0]["stdin"].get<std::string>())["type"] ==
          "recording.started");
    CHECK(lines(started).size() == 1);
    CHECK(Json::parse(lines(stopped)[0]["stdin"].get<std::string>())["type"] ==
          "recording.stopped");
    CHECK(lines(stopped).size() == 1);
    CHECK_FALSE(std::filesystem::exists(fv::path_from_utf8(off)));
}

TEST_CASE("test_a_program_runs_one_event_at_a_time_in_order",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    const std::string log = f.folder.file("log.txt");
    f.save({program(
        "p", {"read", "append", log, "start {n}", "sleep", "0.3", "append", log, "end {n}"})});
    f.engine->start();
    for (int n = 0; n < 3; ++n) {
        f.publish("recording.started", {{"n", n}});
    }

    REQUIRE(wait_until(
        [&] {
            const auto text = test::read_text(log);
            return std::ranges::count(test::read_lines(log), std::string("end 2")) == 1 &&
                   text.find("end 1") != std::string::npos;
        },
        20s));
    CHECK(test::read_lines(log) ==
          std::vector<std::string>{"start 0", "end 0", "start 1", "end 1", "start 2", "end 2"});
}

TEST_CASE("test_a_full_queue_drops_the_oldest_event",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    ProgramOptions options;
    options.max_queued = 2;
    Programs f(options);
    const std::string log = f.folder.file("log.txt");
    const std::string started = f.folder.file("started.txt");
    f.save(
        {program("p", {"read", "append", started, "{n}", "sleep", "0.4", "append", log, "{n}"})});
    f.engine->start();
    std::vector<Event> published{f.publish("recording.started", {{"n", 0}})};
    // The first event is running (its program has started) before the rest arrive. An empty
    // queue alone isn't enough: it is also empty before the first event reaches it, and then
    // the full queue would drop that one.
    REQUIRE(wait_until([&] { return test::read_lines(started) == std::vector<std::string>{"0"}; },
                       10s));
    for (int n = 1; n < 6; ++n) {
        published.push_back(f.publish("recording.started", {{"n", n}}));
    }
    std::set<std::string> ours;
    for (const auto& event : published) {
        ours.insert(event.id);
    }
    const auto our_runs = [&] {
        std::vector<RunRecord> found;
        for (const auto& record : f.engine->runs("p")) {
            if (ours.contains(record.event_id)) {
                found.push_back(record);
            }
        }
        return found;
    };

    REQUIRE(wait_until(
        [&] { return std::ranges::count_if(our_runs(), [](const auto& r) { return r.ok; }) == 3; },
        20s));
    test::sleep_ms(600);
    const auto ran = test::read_lines(log);
    std::vector<std::string> dropped;
    for (const auto& record : our_runs()) {
        if (record.error.starts_with("Dropped")) {
            CHECK(record.error == "Dropped: too many events were waiting."); // A19-41
            dropped.push_back(record.event_id);
        }
    }
    REQUIRE(ran.size() == 3);
    CHECK(ran[0] == "0");
    CHECK(ran[1] == "4");
    CHECK(ran[2] == "5");
    CHECK(dropped.size() == 3);
    std::set<std::string> not_ran;
    for (const auto& event : published) {
        const std::string n = std::to_string(event.data["n"].get<int>());
        if (std::ranges::find(ran, n) == ran.end()) {
            not_ran.insert(event.id);
        }
    }
    CHECK(std::set<std::string>(dropped.begin(), dropped.end()) == not_ran);
}

TEST_CASE("test_a_hook_that_hangs_is_stopped_at_its_time_limit",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    f.save({program("p", {"sleep", "60"}, {{"timeout_s", 1}})});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    REQUIRE(wait_until([&] { return !f.engine->runs("p").empty(); }, 10s));
    const RunRecord record = f.engine->runs("p")[0];
    CHECK_FALSE(record.ok);
    CHECK_FALSE(record.exit_code);
    CHECK(record.error == "Timed out after 1 s");
    CHECK(record.elapsed_ms >= 900);
    CHECK(record.elapsed_ms < 6000);
}

TEST_CASE("test_a_failing_exit_code_is_recorded", "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    f.save({program("p", {"exit", "2"})});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    REQUIRE(wait_until([&] { return !f.engine->runs("p").empty(); }, 15s));
    const RunRecord record = f.engine->runs("p")[0];
    CHECK_FALSE(record.ok);
    CHECK(record.exit_code == 2);
    CHECK(record.error == "Exited with code 2");
}

TEST_CASE("test_reload_follows_saved_changes", "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    const std::string out = f.folder.file("runs.jsonl");
    f.save({program("p", record_run(out))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    REQUIRE(wait_until([&] { return lines(out).size() == 1; }, 15s));

    f.save({program("p", record_run(out), {{"events", {"recording.canceled"}}})});
    f.engine->reload();
    f.publish("recording.started", {{"job_id", "2"}});
    f.publish("recording.canceled", {{"job_id", "2"}});
    REQUIRE(wait_until([&] { return lines(out).size() == 2; }, 15s));

    f.save({program("p", record_run(out), {{"enabled", false}})});
    f.engine->reload();
    CHECK(f.bus.subscribers("programs") == 0);
    f.publish("recording.canceled", {{"job_id", "3"}});
    test::sleep_ms(500);
    std::vector<std::string> types;
    for (const auto& run : lines(out)) {
        types.push_back(Json::parse(run["stdin"].get<std::string>())["type"].get<std::string>());
    }
    CHECK(types == std::vector<std::string>{"recording.started", "recording.canceled"});
}

TEST_CASE("test_stop_unsubscribes_and_drops_waiting_events",
          "[test_automation_hooks_programs][platform]") {
    Programs f;
    const std::string out = f.folder.file("runs.jsonl");
    f.save({program("p", record_run(out))});
    f.engine->start();
    f.engine->stop();
    f.engine->stop();
    CHECK(f.bus.subscribers("programs") == 0);
    f.publish("recording.started", {{"job_id", "1"}});
    test::sleep_ms(400);
    CHECK_FALSE(std::filesystem::exists(fv::path_from_utf8(out)));
}

TEST_CASE("test_run_test_runs_a_program_test_event_even_when_disabled",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    const std::string out = f.folder.file("runs.jsonl");
    f.save({program("p", record_run(out), {{"enabled", false}})});
    auto answer = std::make_shared<std::promise<RunRecord>>();
    auto future = answer->get_future();
    const auto started = std::chrono::steady_clock::now();
    f.engine->run_test("p", [answer](RunRecord record) { answer->set_value(record); });
    CHECK(test::seconds_since(started) < 0.5);
    REQUIRE(future.wait_for(20s) == std::future_status::ready);
    const RunRecord record = future.get();

    CHECK(record.ok);
    CHECK(record.exit_code == 0);
    const Json run = lines(out).at(0);
    const Json stdin_event = Json::parse(run["stdin"].get<std::string>());
    CHECK(run["env"][env_name(fv::identity().env_prefix, "EVENT_TYPE")] == "program.test");
    CHECK(run["env"][env_name(fv::identity().env_prefix, "EVENT_ID")] == record.event_id);
    CHECK(stdin_event["data"]["program_id"] == "p");
    // A19-41: the test event's own words.
    CHECK(stdin_event["data"]["message"] ==
          std::format("This is a test run from {}.", fv::identity().product));
    CHECK(f.engine->runs("p") == std::vector<RunRecord>{record});
}

TEST_CASE("test_run_test_for_an_unsaved_program_says_so", "[test_automation_hooks_programs]") {
    Programs f;
    const RunRecord record = f.run_test("missing");
    CHECK_FALSE(record.ok);
    CHECK(record.error == "Save the program first.");
}

TEST_CASE("test_run_test_waits_for_the_running_event_then_goes_first",
          "[test_automation_hooks_programs][platform]") {
    test::require_program_start();
    Programs f;
    const std::string log = f.folder.file("log.txt");
    f.save(
        {program("p", {"read", "append", log, "start", "sleep", "0.5", "append", log, "{type}"})});
    f.engine->start();
    f.publish("recording.started", {{"n", 0}});
    f.publish("recording.started", {{"n", 1}});
    // The first event is running and the second is waiting.
    REQUIRE(wait_until(
        [&] {
            return test::read_text(log).find("start") != std::string::npos &&
                   f.engine->queued("p") == 1;
        },
        15s));
    f.run_test("p");
    const auto words = [&] {
        std::vector<std::string> found;
        for (const auto& line : test::read_lines(log)) {
            if (line != "start") {
                found.push_back(line);
            }
        }
        return found;
    };
    REQUIRE(wait_until([&] { return words().size() == 3; }, 20s));
    CHECK(words() ==
          std::vector<std::string>{"recording.started", "program.test", "recording.started"});
}

TEST_CASE("test_logs_never_carry_arguments_output_or_event_data",
          "[test_automation_hooks_programs][platform]") {
    test::ScopedLogCapture capture;
    Programs f;
    f.save({program("p", {"write", "program output here", "exit", "1", "secret-argument-123"})});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "dictated words here"}});
    REQUIRE(wait_until([&] { return !f.engine->runs("p").empty(); }, 15s));
    const std::string text = capture.text();
    for (const std::string& needle :
         {std::string("secret-argument-123"), std::string("program output here"),
          std::string("dictated words here"), test::hook_child()}) {
        CHECK(text.find(needle) == std::string::npos);
    }
}

// What the engine hands run_once, without a child process.
TEST_CASE("a program hook's run reads its record from run_once", "[programs]") {
    std::vector<std::vector<std::string>> argvs;
    std::vector<std::string> inputs;
    std::vector<double> timeouts;
    std::mutex mutex;
    ProgramOptions options;
    options.run_once = [&](const std::vector<std::string>& argv, std::string_view input,
                           double timeout_s, const fv::CancelToken&,
                           const std::map<std::string, std::string>& env) {
        std::lock_guard lock(mutex);
        argvs.push_back(argv);
        inputs.emplace_back(input);
        timeouts.push_back(timeout_s);
        CHECK(env.size() == 6);
        return fv::RunOnceResult{.exit_code = 3, .output = "ignored", .error = ""};
    };
    Programs f(options);
    f.save({{{"id", "p"},
             {"name", "P"},
             {"program", "C:\\tools\\hook.exe"},
             {"args", {"--flag", "x y"}},
             {"timeout_s", 7}}});
    f.engine->start();
    const Event event = f.publish("recording.started", {{"job_id", "1"}});
    REQUIRE(wait_until([&] { return !f.engine->runs("p").empty(); }));
    const RunRecord record = f.engine->runs("p")[0];
    CHECK(record ==
          RunRecord{event.id, record.at, false, 3, record.elapsed_ms, "Exited with code 3"});
    std::lock_guard lock(mutex);
    CHECK(argvs.at(0) == std::vector<std::string>{"C:\\tools\\hook.exe", "--flag", "x y"});
    CHECK(inputs.at(0) == event.to_json() + "\n");
    CHECK(timeouts.at(0) == 7.0);
}

TEST_CASE("run records say why a run failed", "[programs]") {
    CHECK(run_record("e", "t", fv::RunOnceResult{.exit_code = 0}, 5) ==
          RunRecord{"e", "t", true, 0, 5, ""});
    CHECK(run_record("e", "t", fv::RunOnceResult{.exit_code = 4}, 5) ==
          RunRecord{"e", "t", false, 4, 5, "Exited with code 4"});
    CHECK(run_record("e", "t", fv::RunOnceResult{.error = "Timed out after 1 s", .timed_out = true},
                     5) == RunRecord{"e", "t", false, std::nullopt, 5, "Timed out after 1 s"});
    CHECK(run_record("e", "t", fv::RunOnceResult{.error = "The program wasn't found."}, 0) ==
          RunRecord{"e", "t", false, std::nullopt, 0, "The program wasn't found."});
    // Python's `code != 0` with a code of None and no error: never ok.
    CHECK(run_record("e", "t", fv::RunOnceResult{}, 0) ==
          RunRecord{"e", "t", false, std::nullopt, 0, "Exited with code None"});
}

TEST_CASE("the fake program control answers held tests, a missing callback too", "[programs]") {
    FakeProgramControl fake;
    fake.set_test_answer(RunRecord{"evt_x", "t", false, 1, 3, "Exited with code 1"}, true);
    std::vector<RunRecord> answers;
    fake.run_test("p", [&](RunRecord record) { answers.push_back(std::move(record)); });
    fake.run_test("q", {});
    CHECK(answers.empty());
    fake.release_tests();
    CHECK(answers ==
          std::vector<RunRecord>{RunRecord{"evt_x", "t", false, 1, 3, "Exited with code 1"}});
    CHECK(fake.tests_run() == std::vector<std::string>{"p", "q"});
}
