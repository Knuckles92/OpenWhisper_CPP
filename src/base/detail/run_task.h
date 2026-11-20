#pragma once

#include <string_view>

#include "base/executor.h"

namespace fv::detail {

// Runs a task on an executor's thread. An exception never leaves it (architecture 4, rule 10): it
// is logged with its type under "base.executor", naming the executor.
void run_task(const Task& task, std::string_view executor_name) noexcept;

} // namespace fv::detail
