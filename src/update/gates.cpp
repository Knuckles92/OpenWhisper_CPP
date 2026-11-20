#include "update/gates.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/identity.h"
#include "base/log.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.app_update_apply");

} // namespace

std::string setup_running_message() {
    const std::string product(identity().product);
    return product + " Setup is still running, so " + product +
           " did not start. Finish the installer, then start " + product + " again.";
}

bool skips_launch_gate(std::span<const std::string> args) {
    static constexpr std::array<std::string_view, 5> kSkipped{
        "--self-test-gui", "--workflow-smoke", "--version", "--help", "--diagnostics"};
    return !args.empty() && std::ranges::find(kSkipped, args.front()) != kSkipped.end();
}

LaunchGate::LaunchGate(MutexApi* api, std::vector<std::string> app_mutexes, std::string setup_mutex,
                       bool installed)
    : api_(api), app_mutexes_(std::move(app_mutexes)), setup_mutex_(std::move(setup_mutex)),
      installed_(installed) {}

LaunchGate::LaunchGate(LaunchGate&& other) noexcept
    : api_(other.api_), app_mutexes_(std::move(other.app_mutexes_)),
      setup_mutex_(std::move(other.setup_mutex_)), installed_(other.installed_),
      handles_(std::exchange(other.handles_, {})) {}

LaunchGate& LaunchGate::operator=(LaunchGate&& other) noexcept {
    if (this != &other) {
        release_all();
        api_ = other.api_;
        app_mutexes_ = std::move(other.app_mutexes_);
        setup_mutex_ = std::move(other.setup_mutex_);
        installed_ = other.installed_;
        handles_ = std::exchange(other.handles_, {});
    }
    return *this;
}

LaunchGate::~LaunchGate() {
    release_all();
}

void LaunchGate::release_all() {
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
        api_->release(*it);
    }
    handles_.clear();
}

bool LaunchGate::acquire_app_mutexes() {
    std::vector<MutexHandle> acquired;
    for (const auto& name : app_mutexes_) {
        const auto handle = api_->create(name);
        if (!handle) {
            for (auto it = acquired.rbegin(); it != acquired.rend(); ++it) {
                api_->release(*it);
            }
            return false;
        }
        acquired.push_back(*handle);
    }
    handles_ = std::move(acquired);
    return true;
}

std::expected<LaunchGate, std::string> LaunchGate::enter(const ReleaseIdentity& release,
                                                         const InstallProbe& install, MutexApi& api,
                                                         Clock& clock, bool verified_health) {
    const bool installed = detect_channel(install) == InstallChannel::Installer;
    LaunchGate gate(&api, release.app_mutexes(), release.setup_mutex(), installed);
    if (!installed) {
        return gate;
    }
    if (std::ranges::any_of(release.update_mutexes(),
                            [&api](const auto& name) { return api.exists(name); })) {
        if (!verified_health || !gate.acquire_app_mutexes()) {
            return std::unexpected(std::string(identity().product) +
                                   " is finishing an update. Try again in a moment.");
        }
        return gate;
    }
    if (verified_health) {
        return std::unexpected(std::string("The update helper is no longer running."));
    }
    const auto deadline = clock.steady() + kSetupLaunchWait;
    while (api.exists(gate.setup_mutex_)) {
        if (clock.steady() >= deadline) {
            return std::unexpected(setup_running_message());
        }
        api.wait(kLockPoll);
    }
    const auto setup_gate = api.create(gate.setup_mutex_);
    if (!setup_gate) {
        // A setup started after the wait: OpenWhisper leaves without a word.
        return std::unexpected(std::string());
    }
    if (!gate.acquire_app_mutexes()) {
        kLog.info("Another {} process holds the application mutex", identity().product);
    }
    api.release(*setup_gate);
    return gate;
}

void LaunchGate::release_for_setup() {
    release_all();
}

bool LaunchGate::reacquire() {
    if (!installed_ || !handles_.empty()) {
        return !handles_.empty();
    }
    const auto setup_gate = api_->create(setup_mutex_);
    const bool acquired = acquire_app_mutexes();
    if (setup_gate) {
        api_->release(*setup_gate);
    }
    return acquired;
}

} // namespace fv::update
