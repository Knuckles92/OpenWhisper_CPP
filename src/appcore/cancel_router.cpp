#include "appcore/cancel_router.h"

#include <algorithm>
#include <utility>

namespace fv {

void CancelRouter::add(int order, std::function<bool()> step) {
    const auto at =
        std::upper_bound(steps_.begin(), steps_.end(), order,
                         [](int value, const Step& entry) { return value < entry.order; });
    steps_.insert(at, Step{order, std::move(step)});
}

bool CancelRouter::cancel() {
    // A copy: a step may add another without disturbing this pass.
    const auto steps = steps_;
    for (const auto& entry : steps) {
        if (entry.step()) {
            return true;
        }
    }
    nothing_to_cancel.emit(std::string(kNothingToCancel));
    return false;
}

std::size_t CancelRouter::size() const {
    return steps_.size();
}

} // namespace fv
