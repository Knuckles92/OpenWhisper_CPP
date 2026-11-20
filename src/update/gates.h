#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "update/check.h"
#include "update/release_identity.h"

// The named mutexes an installed copy shares with its Inno setup
// (services/app_update_apply.py: create_named_mutex ... release_application_mutex_for_setup, the
// setup wait of maybe_exit_if_update_in_progress; main.py: _early_update_gate). The app mutex
// lets the setup see a running app (AppMutex); the setup mutex lets the app see a running setup
// (SetupMutex). The native updater's update mutexes and its refusal are M10's.
namespace fv::update {

using MutexHandle = std::uintptr_t;

// Named mutexes. win/ uses the kernel's; posix/ has none (create succeeds, nothing exists).
// testing/fake_mutex_api.h scripts them.
// Affinity: the thread that created a mutex releases it.
class MutexApi {
public:
    virtual ~MutexApi() = default;
    // A newly created mutex of that name, owned by the calling thread; nullopt when one existed
    // already (it is not kept) or none could be made.
    virtual std::optional<MutexHandle> create(std::string_view name) = 0;
    virtual bool exists(std::string_view name) = 0;
    virtual void release(MutexHandle) = 0;
    // Sleeps between polls.
    virtual void wait(std::chrono::milliseconds) = 0;
};

// The kernel's named mutexes on Windows.
MutexApi& system_mutex_api();

// How long a launch waits for a running setup, and how often it looks.
inline constexpr std::chrono::seconds kSetupLaunchWait{120};
inline constexpr std::chrono::milliseconds kLockPoll{250};

// "FioriVoce Setup is still running, so FioriVoce did not start. Finish the installer, then start
// FioriVoce again."
std::string setup_running_message();

// Whether the first command-line argument skips the gate: a self test, the workflow smoke, the
// version, help and diagnostics never wait for a setup (A01-70).
bool skips_launch_gate(std::span<const std::string> args);

// The installed copy's hold on the app mutexes, from the start of main() to exit.
// Affinity: the thread that entered it (main's), which alone may release and re-acquire.
class LaunchGate {
public:
    // Before any Qt object exists. A development build (not the registered install) creates no
    // mutex and never waits. An installed copy waits up to kSetupLaunchWait, polling every
    // kLockPoll, while the setup mutex exists (Inno's postinstall launch runs while the setup
    // still holds it) and then refuses with setup_running_message(). It then takes the setup
    // mutex for a moment (an empty refusal when a setup took it first: exit 0 without a word, as
    // OpenWhisper does) and creates the app mutexes. Unlike OpenWhisper it does not exit when
    // another instance already holds them: the single-instance handshake raises that window.
    // The caller shows a refusal with native_message_box and exits 0.
    static std::expected<LaunchGate, std::string> enter(const ReleaseIdentity&, const InstallProbe&,
                                                        MutexApi&, Clock&,
                                                        bool verified_health = false);

    LaunchGate(LaunchGate&&) noexcept;
    LaunchGate& operator=(LaunchGate&&) noexcept;
    LaunchGate(const LaunchGate&) = delete;
    LaunchGate& operator=(const LaunchGate&) = delete;
    // Releases what it holds.
    ~LaunchGate();

    // Lets go of the app mutexes immediately before the setup (or the restart) starts, so the
    // setup does not wait for this process.
    void release_for_setup();
    // After a setup that failed to start: takes the app mutexes again; false when another
    // process holds them now. Never refuses.
    bool reacquire();

    bool installed() const { return installed_; }
    bool holds_app_mutexes() const { return !handles_.empty(); }

private:
    LaunchGate(MutexApi* api, std::vector<std::string> app_mutexes, std::string setup_mutex,
               bool installed);
    // acquire_named_mutexes(APP_MUTEX_NAMES, 0): all or none.
    bool acquire_app_mutexes();
    void release_all();

    MutexApi* api_ = nullptr;
    std::vector<std::string> app_mutexes_;
    std::string setup_mutex_;
    bool installed_ = false;
    std::vector<MutexHandle> handles_;
};

// The last-resort error box when Qt is not running: MessageBoxW with the product as its title
// and the error icon, the text cut to 2,000 characters; stderr elsewhere. Blocks until the user
// closes it.
void native_message_box(std::string_view message);

} // namespace fv::update
