#include "update/code_identity.h"

#include <mutex>

#include "update/detail/platform.h"

namespace fv::update {

namespace {

struct Launch {
    std::mutex mutex;
    std::filesystem::path exe;
    std::optional<std::string> identity;
};

Launch& launch() {
    static Launch state;
    return state;
}

} // namespace

std::optional<std::string> code_identity(const std::filesystem::path& exe) {
    return detail::file_identity(exe);
}

void CodeIdentity::remember_launch(const std::filesystem::path& exe) {
    auto identity = code_identity(exe);
    auto& state = launch();
    std::lock_guard lock(state.mutex);
    state.exe = exe;
    state.identity = std::move(identity);
}

bool CodeIdentity::changed_since_launch() {
    std::filesystem::path exe;
    std::optional<std::string> at_launch;
    {
        auto& state = launch();
        std::lock_guard lock(state.mutex);
        exe = state.exe;
        at_launch = state.identity;
    }
    if (!at_launch) {
        return false;
    }
    const auto now = code_identity(exe);
    return now.has_value() && *now != *at_launch;
}

void CodeIdentity::forget_launch() {
    auto& state = launch();
    std::lock_guard lock(state.mutex);
    state.exe.clear();
    state.identity.reset();
}

} // namespace fv::update
