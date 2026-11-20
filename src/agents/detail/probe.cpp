#include "agents/detail/probe.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

#include "agents/detail/pyvalue.h"
#include "base/log.h"
#include "base/utf8.h"

namespace fv::agents::detail {

namespace {

constexpr log::Logger logger("services.installed_agents");

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds millis(double seconds) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(seconds));
}

// What communicate() collects: both pipes to their end.
struct Captured {
    std::mutex mutex;
    std::condition_variable ended;
    std::string out;
    std::string err;
    int open = 2;
};

} // namespace

ProbeResult run_probe(ProcessLauncher& launcher, const std::vector<std::string>& argv,
                      double timeout_s, const std::filesystem::path& cwd,
                      std::map<std::string, std::string> env, const CancelToken& cancel) {
    if (argv.empty()) {
        return {};
    }
    ProcessSpec spec;
    spec.program = path_from_utf8(argv.front());
    spec.args.assign(argv.begin() + 1, argv.end());
    spec.cwd = cwd;
    spec.env = std::move(env);
    spec.own_tree = true;
    spec.pipes = true;
    spec.stdin_null = true;

    const auto deadline = Clock::now() + millis(timeout_s);
    auto started = launcher.start(spec);
    if (!started) {
        logger.debug("Agent probe failed to start");
        return {};
    }
    auto& process = **started;
    auto captured = std::make_shared<Captured>();
    const auto sink = [captured](std::string Captured::*target) {
        return [captured, target](std::span<const char> bytes) {
            {
                std::lock_guard lock(captured->mutex);
                if (bytes.empty()) {
                    --captured->open;
                } else {
                    ((*captured).*target).append(bytes.data(), bytes.size());
                }
            }
            captured->ended.notify_all();
        };
    };
    process.on_bytes(sink(&Captured::out), sink(&Captured::err));

    ProbeResult result;
    const auto remaining = [&] {
        return std::max(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()),
            std::chrono::milliseconds(0));
    };
    // The wait is sliced so that a cancel (Quit) ends it, with the tree killed below.
    std::optional<int> code;
    while (!cancel.cancelled()) {
        const auto left = remaining();
        code = process.wait(std::min(left, PROBE_CANCEL_POLL));
        if (code || left == std::chrono::milliseconds(0)) {
            break;
        }
    }
    bool ended = false;
    if (code && !cancel.cancelled()) {
        std::unique_lock lock(captured->mutex);
        ended = captured->ended.wait_until(lock, deadline, [&] { return captured->open == 0; });
    }
    if (code && ended) {
        std::lock_guard lock(captured->mutex);
        result.code = code;
        result.text = text_mode(captured->out) + text_mode(captured->err);
    } else {
        logger.debug("Agent probe timed out");
    }
    kill_process_tree(process);
    process.release_pipes(std::chrono::milliseconds(0));
    return result;
}

} // namespace fv::agents::detail
