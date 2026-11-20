#pragma once

#include <functional>
#include <string>

#include "automation/action_model.h"

namespace fv::automation {

// How an action's run went (services/actions/runtime.py ActionResult): `message` is shown as it
// is; `output` is an answer's text ("" for none). `stopped`: its cancel stopped the work before
// it was done (a program killed or never started, a quick answer that gave none).
// Affinity: value type.
struct ActionResult {
    bool ok = false;
    std::string message;
    std::string output;
    bool stopped = false;

    friend bool operator==(const ActionResult&, const ActionResult&) = default;
};

// "Try it" for Settings: runs `action` on `sample` words through the app's action runtime and
// calls `done` once with the result on the thread that called test. cancel_tests stops the
// tries still running and returns how many it stopped.
// Affinity: GUI thread.
class ActionTester {
public:
    virtual ~ActionTester() = default;
    virtual void test(const Action& action, std::string sample,
                      std::function<void(ActionResult)> done) = 0;
    virtual int cancel_tests() = 0;
};

} // namespace fv::automation
