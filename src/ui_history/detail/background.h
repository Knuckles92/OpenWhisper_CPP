#pragma once

#include <memory>
#include <utility>

#include "appkit/qt_bridge.h"
#include "base/executor.h"

namespace fv::history_detail {

// Runs work() on io, then done(result) through ui, an executor bound to the widget that asked:
// once that widget is gone its executor drops the task, so done may use the widget freely and
// the worker never touches it. work must not throw; both must be copyable (Task is a
// std::function).
template <class Work, class Done>
void runInBackground(Executor& io, const std::shared_ptr<qt::QtUiExecutor>& ui, Work work,
                     Done done) {
    io.post([work = std::move(work), done = std::move(done), ui]() mutable {
        auto result = work();
        ui->post([done, result]() mutable { done(std::move(result)); });
    });
}

} // namespace fv::history_detail
