#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The user's environment, as opposed to the one this app runs in (services/process_env.py).
//
// The app changes its own environment to run (folders put first on PATH for the speech runtime,
// display variables), and its .env may hold keys an editor that started it also loaded. Programs
// started for the user (actions, hooks, agents, terminals, links) must not inherit any of that:
// the keys would change which account an agent bills. Code that changes this process's
// environment records the original value here (set_for_app, prepend_for_app); external_env undoes
// those changes. OpenWhisper's frozen-loader and virtualenv clean-up has nothing to undo here.
namespace fv {

// Environment names as the system compares them: without regard to case on Windows (as
// os.environ upper-cases them), exactly elsewhere.
struct EnvNameLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const;
};
using EnvMap = std::map<std::string, std::string, EnvNameLess>;

// A process environment as os.environ holds it. set throws std::invalid_argument where the
// platform's putenv refuses ("illegal environment variable name": empty, or "=" after the first
// character on Windows and anywhere elsewhere; a NUL character; on Windows more than 32,767
// UTF-16 units for "name=value").
// Affinity: thread-safe.
class MutableEnv {
public:
    virtual ~MutableEnv() = default;
    virtual std::optional<std::string> get(std::string_view name) const = 0;
    virtual void set(std::string_view name, std::string_view value) = 0;
    virtual void unset(std::string_view name) = 0;
    // Every variable, names as os.environ gives them (upper case on Windows).
    virtual std::vector<std::pair<std::string, std::string>> items() const = 0;
};

// Throws what MutableEnv::set throws for this assignment under Windows' or POSIX's rules.
void check_env_assignment(std::string_view name, std::string_view value, bool windows);

// This process's environment. On Windows names are read upper-cased and variables whose name
// starts with "=" (the per-drive folders) are left out, as the C runtime's environment does.
MutableEnv& system_mutable_env();

class DotEnv;

// Affinity: thread-safe.
class ProcessEnv {
public:
    // windows: Windows' rules for names (case-insensitive, upper-cased keys) and ";" between
    // path entries; otherwise POSIX's.
    explicit ProcessEnv(MutableEnv& env, bool windows = kWindows);
    ProcessEnv(const ProcessEnv&) = delete;
    ProcessEnv& operator=(const ProcessEnv&) = delete;

    // Over system_mutable_env().
    static ProcessEnv& instance();

    // Sets `name` for this process; programs it starts get the original back. keep_existing
    // leaves a value the user already set alone. Throws std::invalid_argument as MutableEnv::set.
    void set_for_app(std::string_view name, std::string_view value, bool keep_existing = false);
    // Python's load_for_app: adds each variable that is unset, never over the user's, and
    // records it (and one already set to the same value) as the app's, unset for the user. One
    // set to anything else is the user's and stays. Returns the names no environment can hold;
    // those are left out and the rest still load.
    std::vector<std::string>
    load_for_app(std::span<const std::pair<std::string, std::string>> values);
    // What startup does instead of load_for_app, since .env is never loaded into this process's
    // environment: a variable whose current value equals the file's (an editor that started the
    // app, such as VS Code, loads the same .env) is recorded as the app's. Names are matched as
    // the system matches them, whatever case the file writes them in.
    void adopt_dotenv(const DotEnv& dotenv);
    // Puts `entries` first on a path list such as PATH, for this process only.
    void prepend_for_app(std::string_view name, std::span<const std::string> entries);

    // The environment for a program started for the user: this process's minus what the app
    // changed for itself, plus `extra`.
    EnvMap external_env(const std::map<std::string, std::string>& extra = {}) const;
    // Removes `names` from `env`, matching names the way the system does.
    void drop(EnvMap& env, std::span<const std::string> names) const;
    // Drops the recorded originals (tests).
    void forget();

    bool windows() const { return windows_; }
    // The name as the recorded originals key it: upper case on Windows.
    std::string key(std::string_view name) const;

#ifdef _WIN32
    static constexpr bool kWindows = true;
#else
    static constexpr bool kWindows = false;
#endif

private:
    void remember(std::string_view name, std::optional<std::string> original);

    MutableEnv& env_;
    bool windows_;
    mutable std::mutex mutex_;
    // Variables this process changed for itself: key -> value before the first change.
    std::map<std::string, std::optional<std::string>> original_;
};

// The app's own internal variables, never passed to a child: <prefix>SIDECAR_TOKEN,
// <prefix>LLM_API_KEY and <prefix>LLM_BASE_URL under this app's and OpenWhisper's prefix.
std::vector<std::string> internal_env_names();

// installed_agents.agent_child_env: external_env() minus internal_env_names(), plus `extra`.
// Every child the app starts for the user gets this (programs.child_env, launch._child_env).
EnvMap child_env(const std::map<std::string, std::string>& extra = {});
EnvMap child_env(const ProcessEnv& env, const std::map<std::string, std::string>& extra = {});

// An EnvMap as ProcessSpec::env holds it.
std::map<std::string, std::string> to_spec_env(const EnvMap& env);

} // namespace fv
