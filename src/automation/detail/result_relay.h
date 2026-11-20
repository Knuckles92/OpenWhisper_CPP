#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "automation/action_result.h"
#include "base/executor.h"

namespace fv::automation::detail {

// Brings results from the action runtime's workers to the UI executor (runtime.py _ResultRelay).
// A worker hands over only the token its run was given and the result; whoever still waits for
// that token gets it on the UI executor, and a token dropped meanwhile (the runtime closed, or
// the run was canceled before it started) is ignored. A callback that throws is logged by its
// exception's type only and the relay goes on serving.
// Affinity: thread-safe; callbacks run on `ui`, which must outlive every result handed over.
class ResultRelay : public std::enable_shared_from_this<ResultRelay> {
public:
    using Callback = std::function<void(ActionResult)>;

    explicit ResultRelay(Executor& ui) : ui_(ui) {}
    ResultRelay(const ResultRelay&) = delete;
    ResultRelay& operator=(const ResultRelay&) = delete;

    // A token no other run of the process has had.
    static std::uint64_t next_token();

    void expect(std::uint64_t token, Callback callback);
    void drop(const std::vector<std::uint64_t>& tokens);
    // Any thread: posts the result to the UI executor for whoever waits for `token` then.
    void finished(std::uint64_t token, ActionResult result);
    // Tokens still waited for.
    std::size_t waiting() const;

private:
    void deliver(std::uint64_t token, ActionResult result);

    Executor& ui_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Callback> waiting_;
};

} // namespace fv::automation::detail
