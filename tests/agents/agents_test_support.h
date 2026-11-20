#pragma once

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/installed_agents.h"
#include "base/utf8.h"
#include "support/scripted_launcher.h"

namespace fv::agents_test {

// A fresh folder under the system's temp folder, removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::random_device seed;
        std::mt19937_64 random(seed());
        path_ = std::filesystem::temp_directory_path() /
                ("fv-agents-test-" + std::to_string(random() % 1000000000000ULL));
        std::filesystem::create_directories(path_);
        path_ = std::filesystem::canonical(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// A program file: empty, since only the scripted launcher "runs" it.
inline std::filesystem::path make_program(const std::filesystem::path& path) {
    write_file(path, "");
    std::filesystem::permissions(path, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    return path;
}

inline bool inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto normal = std::filesystem::absolute(path).lexically_normal();
    const auto relative = normal.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

// InstalledAgents that counts find_agent calls (the Python tests patch find_agent to count),
// and can hold each lookup until a gate opens.
class CountingAgents final : public agents::InstalledAgents {
public:
    using agents::InstalledAgents::InstalledAgents;
    ~CountingAgents() override { wait_for_lookups(); }

    std::optional<agents::InstalledAgent> find_agent(std::string_view id) override {
        {
            std::lock_guard lock(mutex_);
            calls_.emplace_back(id);
        }
        if (gate) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!gate->is_open() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        return agents::InstalledAgents::find_agent(id);
    }

    std::vector<std::string> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    std::shared_ptr<Gate> gate;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
};

// A temporary home with the agents' folders, the user's environment pointing into it, and a
// scripted launcher. No test reads the real ~/.claude, ~/.codex or OpenCode configuration.
struct AgentsFixture {
    TempDir temp;
    std::filesystem::path home = temp.path() / "home";
    std::filesystem::path local = temp.path() / "local";
    std::filesystem::path appdata = temp.path() / "roaming";
    std::filesystem::path path_dir = temp.path() / "path";
    std::filesystem::path tmp = temp.path() / "tmp";
    std::shared_ptr<ScriptedLauncher> launcher = std::make_shared<ScriptedLauncher>();
    std::shared_ptr<std::map<std::string, std::string>> env =
        std::make_shared<std::map<std::string, std::string>>();

    AgentsFixture() {
        for (const auto& dir : {home, local, appdata, path_dir, tmp}) {
            std::filesystem::create_directories(dir);
        }
        *env = {
            {"USERPROFILE", path_to_utf8(home)},
            {"HOME", path_to_utf8(home)},
            {"LOCALAPPDATA", path_to_utf8(local)},
            {"APPDATA", path_to_utf8(appdata)},
            {"CLAUDE_CONFIG_DIR", path_to_utf8(temp.path() / "claude-config")},
            {"CODEX_HOME", path_to_utf8(temp.path() / "codex-home")},
            {"XDG_CONFIG_HOME", path_to_utf8(temp.path() / "xdg")},
            {"PATH", path_to_utf8(path_dir)},
            {"PATHEXT", ".COM;.EXE;.BAT;.CMD"},
        };
    }

    std::function<std::map<std::string, std::string>()> base_env() const {
        return [env = env] {
            return *env;
        };
    }

    template <class Agents = agents::InstalledAgents> std::shared_ptr<Agents> make() const {
        return std::make_shared<Agents>(launcher, base_env(), home, tmp);
    }

    // Every program and working folder the launcher saw is inside the temporary folder.
    void check_launcher_stayed_inside() const {
        for (const auto& spec : launcher->specs()) {
            CHECK(inside(spec.program, temp.path()));
            CHECK(inside(spec.cwd, temp.path()));
        }
    }
};

#ifdef _WIN32
inline constexpr std::string_view kExe = ".exe";
#else
inline constexpr std::string_view kExe = "";
#endif

inline std::filesystem::path exe(const std::filesystem::path& dir, std::string_view command) {
    return dir / path_from_utf8(std::string(command) + std::string(kExe));
}

} // namespace fv::agents_test
