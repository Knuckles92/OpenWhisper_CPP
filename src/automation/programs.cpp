#include "automation/programs.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <set>
#include <utility>

#include "automation/detail/worker_threads.h"
#include "automation/hook_http.h"
#include "automation/hooks_model.h"
#include "automation/webhooks.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/errors.h"

namespace fv::automation {

namespace {

constexpr log::Logger kLog{"services.automation.programs"};

std::string type_name(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    return "RuntimeError";
}

bool wants(const hooks::Program& program, std::string_view type) {
    return std::ranges::find(program.events, type) != program.events.end();
}

} // namespace

std::map<std::string, std::string> event_environment(const Event& event) {
    std::map<std::string, std::string> env;
    for (const std::string_view prefix : {identity().env_prefix, legacy_identity().env_prefix}) {
        const std::string base(prefix);
        env[base + "EVENT_TYPE"] = event.type;
        env[base + "EVENT_ID"] = event.id;
        env[base + "EVENT_SEQ"] = std::to_string(event.seq);
    }
    return env;
}

RunRecord run_record(std::string event_id, std::string at, const RunOnceResult& result,
                     int elapsed_ms) {
    std::string error = result.error;
    // Python's `code != 0` also holds for a code of None, which run_once gives only with an error.
    if (error.empty() && result.exit_code.value_or(-1) != 0) {
        error = result.exit_code ? std::format("Exited with code {}", *result.exit_code)
                                 : std::string("Exited with code None");
    }
    const bool ok = error.empty();
    return RunRecord{std::move(event_id), std::move(at), ok,
                     result.exit_code,    elapsed_ms,    std::move(error)};
}

// What outlives one start/stop: the runners, the records and the threads.
struct ProgramEngine::State {
    State(SettingsStore& settings, EventBus& bus, ProgramOptions options)
        : settings(settings), bus(bus), options(std::move(options)),
          clock(this->options.clock != nullptr ? *this->options.clock : system_clock()) {}

    struct Item {
        Event event;
        // A test run carries its program and its answer; an event run looks its program up when
        // it starts, so a change in Settings applies at once.
        std::optional<hooks::Program> program;
        std::function<void(RunRecord)> done;
    };
    struct Runner {
        std::deque<Item> events;
        std::deque<Item> tests;
        std::uint64_t dropped = 0;
    };

    SettingsStore& settings;
    EventBus& bus;
    ProgramOptions options;
    const Clock& clock;

    // Python's _cond: the runners and which engine is current.
    std::mutex mutex;
    std::map<std::string, Runner, std::less<>> runners;
    std::shared_ptr<Core> engine;

    mutable std::mutex records_mutex;
    std::map<std::string, std::deque<RunRecord>, std::less<>> records;
    // Cancelled only by the destructor: a run under way at stop() finishes within its limit.
    CancelSource cancel;
    detail::WorkerThreads threads;

    void remember(const std::string& program_id, RunRecord record) {
        std::lock_guard lock(records_mutex);
        auto& kept = records[program_id];
        kept.push_front(std::move(record));
        while (kept.size() > programs::MAX_RECORDS) {
            kept.pop_back();
        }
    }

    void forget_records(const std::set<std::string, std::less<>>& keep) {
        std::lock_guard lock(records_mutex);
        std::erase_if(records, [&](const auto& entry) { return !keep.contains(entry.first); });
    }

    // _enqueue; caller holds `mutex`.
    void enqueue(const std::string& program_id, Item item) {
        const bool fresh = !runners.contains(program_id);
        Runner& runner = runners[program_id];
        if (item.done) {
            runner.tests.push_back(std::move(item));
        } else {
            if (runner.events.size() >= options.max_queued) {
                Item dropped = std::move(runner.events.front());
                runner.events.pop_front();
                runner.dropped += 1;
                if (runner.dropped == 1 || runner.dropped % 20 == 0) {
                    kLog.warning(
                        "Program hook {} is backing up; dropped its oldest event ({} so far)",
                        program_id, runner.dropped);
                }
                remember(program_id, RunRecord{dropped.event.id, hook_http::iso_now(clock), false,
                                               std::nullopt, 0, std::string(programs::DROPPED)});
            }
            runner.events.push_back(std::move(item));
        }
        if (fresh) {
            // The runner's thread drains it, then removes it; the next item starts a new one.
            threads.spawn("automation-program",
                          [self = this, program_id] { self->drain(program_id); });
        }
    }

    // _current_program; caller holds `mutex`.
    std::optional<hooks::Program> current_program(std::string_view program_id,
                                                  std::string_view event_type) const;

    // _drain: one program's waiting items, one at a time, until none are left.
    void drain(const std::string& program_id) {
        while (true) {
            Item item;
            hooks::Program program;
            {
                std::lock_guard lock(mutex);
                Runner& runner = runners[program_id];
                if (!runner.tests.empty()) {
                    item = std::move(runner.tests.front());
                    runner.tests.pop_front();
                    program = *item.program;
                } else if (!runner.events.empty()) {
                    item = std::move(runner.events.front());
                    runner.events.pop_front();
                    auto current = current_program(program_id, item.event.type);
                    if (!current) {
                        continue;
                    }
                    program = std::move(*current);
                } else {
                    runners.erase(program_id);
                    return;
                }
            }
            RunRecord record;
            try {
                record = run_event(program, item.event);
            } catch (const std::exception& error) {
                kLog.warning("Program hook {} failed: {}", program_id, type_name(error));
                record = RunRecord{item.event.id,
                                   hook_http::iso_now(clock),
                                   false,
                                   std::nullopt,
                                   0,
                                   std::format("The program couldn't run ({}).", type_name(error))};
            }
            remember(program_id, record);
            if (item.done) {
                try {
                    item.done(std::move(record));
                } catch (const std::exception& error) {
                    kLog.warning("A program test's answer failed: {}", type_name(error));
                }
            }
        }
    }

    // _run_event.
    RunRecord run_event(const hooks::Program& program, const Event& event) {
        const std::string at = hook_http::iso_now(clock);
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> argv{program.program};
        argv.insert(argv.end(), program.args.begin(), program.args.end());
        const std::string input = event.to_json() + "\n";
        const auto env = event_environment(event);
        const double timeout = static_cast<double>(program.timeout_s);
        RunOnceResult result =
            options.run_once
                ? options.run_once(argv, input, timeout, cancel.token(), env)
                : fv::run_once(argv, input, timeout, cancel.token(), {}, env, options.launcher);
        const int elapsed_ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count());
        return run_record(event.id, at, result, elapsed_ms);
    }
};

// One start: which programs want which events, and the bus subscription.
struct ProgramEngine::Core : std::enable_shared_from_this<ProgramEngine::Core> {
    explicit Core(std::shared_ptr<State> state) : state(std::move(state)) {}

    std::shared_ptr<State> state;
    std::mutex configure_mutex;
    // Under state->mutex.
    std::vector<hooks::Program> programs; // enabled, in saved order
    std::shared_ptr<EventSubscription> subscription;
    std::set<std::string, std::less<>> types;
    bool subscribed = false;
    std::int64_t last_seq = 0;
    bool stopping = false;

    const hooks::Program* find(std::string_view id) const {
        for (const auto& program : programs) {
            if (program.id == id) {
                return &program;
            }
        }
        return nullptr;
    }

    void configure(std::vector<hooks::Program> saved) {
        std::vector<hooks::Program> enabled;
        std::set<std::string, std::less<>> wanted;
        for (auto& program : saved) {
            if (program.enabled && !program.events.empty()) {
                wanted.insert(program.events.begin(), program.events.end());
                enabled.push_back(std::move(program));
            }
        }
        std::lock_guard configuring(configure_mutex);
        std::shared_ptr<EventSubscription> old;
        {
            std::lock_guard lock(state->mutex);
            if (stopping) {
                return;
            }
            programs = std::move(enabled);
            for (auto& [id, runner] : state->runners) {
                const hooks::Program* program = find(id);
                std::erase_if(runner.events, [&](const State::Item& item) {
                    return program == nullptr || !wants(*program, item.event.type);
                });
            }
            if (subscribed && wanted == types) {
                return;
            }
            old = subscription;
            subscribed = true;
            types = wanted;
        }
        // Subscribe before closing the old one; an event delivered to both is dropped by its
        // sequence number.
        std::shared_ptr<EventSubscription> fresh;
        if (!wanted.empty()) {
            std::weak_ptr<Core> weak = weak_from_this();
            fresh = state->bus.subscribe(
                [weak](const Event& event) {
                    if (auto core = weak.lock()) {
                        core->on_event(event);
                    }
                },
                std::vector<std::string>(wanted.begin(), wanted.end()),
                std::string(programs::SUBSCRIPTION_LABEL));
        }
        {
            std::lock_guard lock(state->mutex);
            subscription = fresh;
        }
        if (old) {
            old->close();
        }
    }

    void stop() {
        std::lock_guard configuring(configure_mutex);
        std::shared_ptr<EventSubscription> closing;
        {
            std::lock_guard lock(state->mutex);
            stopping = true;
            programs.clear();
            for (auto& [id, runner] : state->runners) {
                runner.events.clear();
            }
            closing = std::move(subscription);
        }
        if (closing) {
            closing->close();
        }
    }

    // The bus callback (dispatch thread): only enqueues.
    void on_event(const Event& event) {
        std::lock_guard lock(state->mutex);
        if (stopping || event.seq <= last_seq) {
            return;
        }
        last_seq = event.seq;
        for (const auto& program : programs) {
            if (wants(program, event.type)) {
                state->enqueue(program.id, State::Item{event, std::nullopt, {}});
            }
        }
    }
};

std::optional<hooks::Program>
ProgramEngine::State::current_program(std::string_view program_id,
                                      std::string_view event_type) const {
    if (!engine) {
        return std::nullopt;
    }
    const hooks::Program* program = engine->find(program_id);
    if (program == nullptr || !wants(*program, event_type)) {
        return std::nullopt;
    }
    return *program;
}

ProgramEngine::ProgramEngine(SettingsStore& settings, EventBus& bus, ProgramOptions options)
    : state_(std::make_shared<State>(settings, bus, std::move(options))) {}

ProgramEngine::~ProgramEngine() {
    stop();
    state_->cancel.cancel();
    state_->threads.join_all();
}

void ProgramEngine::start() {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        if (!core_) {
            core_ = std::make_shared<Core>(state_);
            std::lock_guard state_lock(state_->mutex);
            state_->engine = core_;
        }
        core = core_;
    }
    reload_core(core);
}

void ProgramEngine::stop() {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        core = std::exchange(core_, nullptr);
        if (core) {
            std::lock_guard state_lock(state_->mutex);
            if (state_->engine == core) {
                state_->engine = nullptr;
            }
        }
    }
    if (core) {
        core->stop();
    }
}

void ProgramEngine::reload() {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        core = core_;
    }
    if (core) {
        reload_core(core);
    }
}

void ProgramEngine::reload_core(const std::shared_ptr<Core>& core) {
    auto saved = hooks::load_programs(*state_->settings.snapshot());
    std::set<std::string, std::less<>> keep;
    for (const auto& program : saved) {
        keep.insert(program.id);
    }
    state_->forget_records(keep);
    core->configure(std::move(saved));
}

std::vector<RunRecord> ProgramEngine::runs(std::string_view program_id) const {
    std::lock_guard lock(state_->records_mutex);
    const auto found = state_->records.find(program_id);
    if (found == state_->records.end()) {
        return {};
    }
    return {found->second.begin(), found->second.end()};
}

void ProgramEngine::run_test(std::string program_id, std::function<void(RunRecord)> done) {
    std::optional<hooks::Program> program;
    try {
        program = hooks::find_program(*state_->settings.snapshot(), program_id);
    } catch (const std::exception& error) {
        kLog.warning("Could not read saved programs: {}", type_name(error));
    }
    if (!program) {
        RunRecord record{"", hook_http::iso_now(state_->clock), false, std::nullopt,
                         0,  std::string(programs::SAVE_FIRST)};
        if (done) {
            done(std::move(record));
        }
        return;
    }
    Event event = synthetic_event(
        programs::TEST_EVENT_TYPE,
        Json{{"message", std::format("This is a test run from {}.", identity().product)},
             {"program_id", program->id}},
        state_->clock);
    if (!done) {
        done = [](RunRecord) {
        };
    }
    std::lock_guard lock(state_->mutex);
    const std::string id = program->id;
    state_->enqueue(id, State::Item{std::move(event), std::move(program), std::move(done)});
}

std::size_t ProgramEngine::queued(std::string_view program_id) const {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->runners.find(program_id);
    return found == state_->runners.end() ? 0 : found->second.events.size();
}

} // namespace fv::automation
