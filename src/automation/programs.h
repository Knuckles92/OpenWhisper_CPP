#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "automation/event_bus.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "process/run_once.h"
#include "settings/store.h"

// Running saved programs on events, Settings > Hooks (services/automation/programs.py). A program
// receives the event's wire JSON and a newline on stdin, and FIORIVOCE_EVENT_TYPE, _ID and _SEQ
// (with the legacy-prefixed names beside them, for scripts written for OpenWhisper) in the
// user's environment without this app's secrets, through run_once: no shell, no window, its own
// process group, stopped with everything it started when its time limit passes. Each program
// runs one event at a time; up to MAX_QUEUED more wait and the oldest is dropped beyond that.
// Program paths, arguments and output are never logged.
namespace fv::automation {

namespace programs {
// Events waiting per program; the oldest is dropped beyond this.
inline constexpr std::size_t MAX_QUEUED = 20;
// Recent runs kept per program (memory only).
inline constexpr std::size_t MAX_RECORDS = 20;
inline constexpr std::string_view TEST_EVENT_TYPE = "program.test";
inline constexpr std::string_view SUBSCRIPTION_LABEL = "programs";
inline constexpr std::string_view DROPPED = "Dropped: too many events were waiting.";
inline constexpr std::string_view SAVE_FIRST = "Save the program first.";
} // namespace programs

// One run of a program hook, as Settings > Hooks lists it.
// Affinity: value type.
struct RunRecord {
    std::string event_id;
    std::string at; // UTC, "2026-11-16T09:30:00.123Z"
    bool ok = false;
    std::optional<int> exit_code;
    int elapsed_ms = 0;
    std::string error;

    friend bool operator==(const RunRecord&, const RunRecord&) = default;
};

// The variables a program gets for `event`: <prefix>EVENT_TYPE, <prefix>EVENT_ID and
// <prefix>EVENT_SEQ, for identity()'s prefix and legacy_identity()'s.
std::map<std::string, std::string> event_environment(const Event& event);

// _run_event's record of a finished run_once: ok when it ran to its end with exit code 0, else
// its error, or "Exited with code N".
RunRecord run_record(std::string event_id, std::string at, const RunOnceResult& result,
                     int elapsed_ms);

// run_once as the engine calls it; tests replace it.
using RunOnceFn = std::function<RunOnceResult(
    const std::vector<std::string>& argv, std::string_view input, double timeout_s,
    const CancelToken& cancel, const std::map<std::string, std::string>& extra_env)>;

// What Settings > Hooks asks of program hooks; ProgramEngine implements it and
// testing/fake_hook_engines.h fakes it.
// Affinity: thread-safe.
class ProgramControl {
public:
    virtual ~ProgramControl() = default;
    // Recent runs of one program, newest first (memory only).
    virtual std::vector<RunRecord> runs(std::string_view program_id) const = 0;
    // Runs one saved program with a program.test event, whether or not it is enabled: after a
    // run of the same program already under way, ahead of its waiting events. `done` gets the
    // record on the runner's thread (at once, on the calling thread, for a program that is not
    // saved) and never an exception.
    virtual void run_test(std::string program_id, std::function<void(RunRecord)> done) = 0;
    // Saved programs changed.
    virtual void reload() = 0;
};

struct ProgramOptions {
    std::size_t max_queued = programs::MAX_QUEUED;
    const Clock* clock = nullptr; // record times; null: system_clock()
    RunOnceFn run_once;           // empty: fv::run_once with `launcher`
    ProcessLauncher* launcher = nullptr;
};

// The subscription (label "programs") and one serial runner per program. stop() drops waiting
// events; a run under way finishes within its limit, and test runs still run. start() after
// stop() subscribes afresh; records survive both. The destructor stops, stops the programs
// still running (with what they started) and waits for every thread it started.
// Affinity: thread-safe; the bus callback only enqueues.
class ProgramEngine final : public ProgramControl {
public:
    ProgramEngine(SettingsStore& settings, EventBus& bus, ProgramOptions options = {});
    ~ProgramEngine() override;
    ProgramEngine(const ProgramEngine&) = delete;
    ProgramEngine& operator=(const ProgramEngine&) = delete;

    // Begins running enabled programs on their events; idempotent.
    void start();
    void stop();
    void reload() override;

    std::vector<RunRecord> runs(std::string_view program_id) const override;
    void run_test(std::string program_id, std::function<void(RunRecord)> done) override;

    // Events waiting for one program (test runs not counted).
    std::size_t queued(std::string_view program_id) const;

    struct State;
    struct Core;

private:
    void reload_core(const std::shared_ptr<Core>& core);

    std::shared_ptr<State> state_;
    std::shared_ptr<Core> core_; // null while stopped
    mutable std::mutex engine_mutex_;
};

} // namespace fv::automation
