#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "base/executor.h"
#include "base/json.h"
#include "ui_record/quick_record_tab.h"

namespace fv {

// What Quick Record's Command Mode hint knows of the AI model: whether it is set up. The check
// reads the credential store, which blocks and never runs on the GUI thread, while the tab asks
// on the GUI thread at every refresh; so the supplier answers with what the last check found
// (nullopt, "treated as working", before the first), and starts a new check each time it is
// asked. A check that finds another answer than the last one calls `changed`, which makes the
// tab read again.
// Affinity: GUI thread. The probe runs on `worker`.
class CommandHintReadiness {
public:
    // The check: llm::provider_readiness over the settings it is given. It blocks.
    using Probe = std::function<std::optional<bool>(const Json& settings)>;

    // `worker` and `ui` outlive the object and the checks queued on them.
    CommandHintReadiness(Probe probe, Executor& worker, Executor& ui);
    // Checks still running are dropped when they land.
    ~CommandHintReadiness();
    CommandHintReadiness(const CommandHintReadiness&) = delete;
    CommandHintReadiness& operator=(const CommandHintReadiness&) = delete;

    // The supplier for QuickRecordTab::setCommandModeReadiness. It must not outlive this object.
    QuickRecordTab::CommandModeReadiness supplier();
    // Runs on the GUI thread when a check finds another answer than the last one.
    void setChanged(std::function<void()> changed);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace fv
