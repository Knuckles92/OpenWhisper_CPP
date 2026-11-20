#include "process/agent_process.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <typeinfo>

#include "base/log.h"
#include "base/utf8.h"
#include "process/detail/text.h"
#include "process/process_env.h"

namespace fv {

namespace {

constexpr log::Logger logger("services.installed_agents");

using Clock = std::chrono::steady_clock;

// Clamped to [0, a year]: a NaN, a negative or an infinite time (or one that would overflow the
// clock) can't reach the conversion, which would be undefined.
std::chrono::milliseconds millis(double seconds) {
    constexpr double longest = 365.0 * 24 * 3600;
    if (!(seconds > 0)) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(std::min(seconds, longest)));
}

// What communicate() collects: both pipes to their end.
struct Captured {
    std::mutex mutex;
    std::condition_variable ended;
    std::string out;
    std::string err;
    int open = 2;
};

// subprocess's text mode: decoded with errors replaced, then "\r\n" and "\r" read as "\n".
std::string text_mode(std::string_view bytes) {
    const auto decoded = process_detail::decode_utf8_replace(bytes);
    std::string out;
    out.reserve(decoded.size());
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i] == '\r') {
            out += '\n';
            if (i + 1 < decoded.size() && decoded[i + 1] == '\n') {
                ++i;
            }
        } else {
            out += decoded[i];
        }
    }
    return out;
}

} // namespace

void kill_process_tree(Process& process, double wait_s) {
    try {
        process.kill_tree();
        if (!process.wait(millis(wait_s))) {
            process.kill_tree();
            process.wait(std::chrono::seconds(1));
        }
    } catch (const std::exception& error) {
        logger.debug("Could not stop agent process ({})", typeid(error).name());
    }
}

ProbeResult run_agent_probe(const std::vector<std::string>& argv, double timeout_s,
                            const std::filesystem::path& cwd, ProcessLauncher* launcher) {
    if (argv.empty()) {
        return {};
    }
    ProcessSpec spec;
    spec.program = path_from_utf8(argv.front());
    spec.args.assign(argv.begin() + 1, argv.end());
    spec.cwd = cwd;
    spec.env = to_spec_env(child_env());
    spec.own_tree = true;
    spec.pipes = true;
    spec.stdin_null = true;

    std::shared_ptr<ProcessLauncher> owned;
    if (launcher == nullptr) {
        owned = make_process_launcher();
        launcher = owned.get();
    }
    const auto deadline = Clock::now() + millis(timeout_s);
    auto started = launcher->start(spec);
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
    const auto code = process.wait(remaining());
    bool ended = false;
    if (code) {
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

} // namespace fv
