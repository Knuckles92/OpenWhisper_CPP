#pragma once

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/agent_actions.h"
#include "agents/installed_agents.h"
#include "agents_test_support.h"
#include "base/utf8.h"
#include "process/launch.h"
#include "process/testing/fake_launch_backend.h"

// The seams of the agent action tests: agents looked up from a table, links and terminals
// recorded by the launch backend, and the instance the actions use installed for one test.
namespace fv::agents_test {

// The words every handoff and ask test sends: reserved characters, non-ASCII, quotes, an option
// and a line break.
inline const std::string HANDOFF_WORDS = "Fix the build & ship it? 100% #now / \xC3\xBCn\xC3\xAF"
                                         "c\xC3\xB6"
                                         "d\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \"q\" 'x' --help\n"
                                         "line two";

// InstalledAgents whose lookups answer from a table (the Python tests patch _find and
// find_agent), counting them, and whose sign-in is the agent as given.
class TableAgents final : public agents::InstalledAgents {
public:
    using agents::InstalledAgents::InstalledAgents;
    ~TableAgents() override { wait_for_lookups(); }

    std::optional<agents::InstalledAgent> find_agent(std::string_view id) override {
        std::lock_guard lock(mutex_);
        ++lookups_;
        const auto it = table_.find(std::string(id));
        if (it == table_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    agents::InstalledAgent with_sign_in(const agents::InstalledAgent& agent) override {
        return agent;
    }

    void set(const agents::InstalledAgent& agent) {
        std::lock_guard lock(mutex_);
        table_.insert_or_assign(agent.id, agent);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        table_.clear();
    }
    int lookups() const {
        std::lock_guard lock(mutex_);
        return lookups_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, agents::InstalledAgent> table_;
    int lookups_ = 0;
};

// Makes `agents` the instance installed_agents() returns for the length of a test.
class ScopedAgents {
public:
    explicit ScopedAgents(std::shared_ptr<agents::InstalledAgents> agents) {
        agents::set_installed_agents(std::move(agents));
    }
    ~ScopedAgents() { agents::set_installed_agents(nullptr); }
    ScopedAgents(const ScopedAgents&) = delete;
    ScopedAgents& operator=(const ScopedAgents&) = delete;
};

// What a terminal was asked to run.
struct Terminal {
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    std::map<std::string, std::string> env;
};

// Handlers by scheme (true, false or unknown), links opened through Explorer, links that fail to
// open, and terminals; installed as the launch backend for the length of a test.
class Desk {
public:
    Desk() {
        backend_->set_assoc_function([this](std::string_view scheme, AssocString what) {
            std::lock_guard lock(mutex_);
            const auto it = handlers_.find(std::string(scheme));
            if (it == handlers_.end() || it->second == false) {
                return AssocAnswer{};
            }
            if (it->second == true) { // unknown (nullopt) falls through to E_FAIL
                return what == AssocString::delegate_execute
                           ? AssocAnswer{ASSOC_OK, "{00000000-0000-0000-0000-000000000000}"}
                           : AssocAnswer{};
            }
            return AssocAnswer{0x80004005, ""}; // E_FAIL: the lookup can't tell
        });
        backend_->set_explorer([this](std::string_view file, std::string_view) -> bool {
            const std::string uri(file);
            const auto scheme = uri.substr(0, uri.find(':'));
            std::lock_guard lock(mutex_);
            attempted_.push_back(uri);
            if (no_answer_) {
                opened_.push_back(uri);
                throw NoAnswer(std::string(NO_ANSWER_MESSAGE));
            }
            if (all_broken_ || broken_.contains(scheme)) {
                throw LaunchError("That app didn't open.");
            }
            opened_.push_back(uri);
            return true;
        });
        set_launch_backend(backend_);
    }
    ~Desk() { set_launch_backend(nullptr); }
    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    void handler(const std::string& scheme, std::optional<bool> found) {
        {
            std::lock_guard lock(mutex_);
            handlers_.insert_or_assign(scheme, found);
        }
        forget_handlers();
    }
    void broken(const std::string& scheme) {
        std::lock_guard lock(mutex_);
        broken_.insert(scheme);
    }
    void all_broken() {
        std::lock_guard lock(mutex_);
        all_broken_ = true;
    }
    void no_answer() {
        std::lock_guard lock(mutex_);
        no_answer_ = true;
    }

    std::vector<std::string> opened() const {
        std::lock_guard lock(mutex_);
        return opened_;
    }
    std::vector<std::string> attempted() const {
        std::lock_guard lock(mutex_);
        return attempted_;
    }
    std::vector<Terminal> terminals() const {
        std::vector<Terminal> out;
        for (const auto& spec : backend_->consoles()) {
            Terminal terminal{{path_to_utf8(spec.program)}, spec.cwd, spec.env};
            terminal.argv.insert(terminal.argv.end(), spec.args.begin(), spec.args.end());
            out.push_back(std::move(terminal));
        }
        return out;
    }
    FakeLaunchBackend& backend() { return *backend_; }

private:
    std::shared_ptr<FakeLaunchBackend> backend_ = std::make_shared<FakeLaunchBackend>();
    mutable std::mutex mutex_;
    std::map<std::string, std::optional<bool>> handlers_;
    std::set<std::string> broken_;
    bool all_broken_ = false;
    bool no_answer_ = false;
    std::vector<std::string> opened_;
    std::vector<std::string> attempted_;
};

// urllib's unquote of a query value: %XX and "+" as a space.
inline std::string unquote(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size()) {
            out += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// (scheme://netloc/path, {key: value}) of a link, as the Python tests' _query reads it.
inline std::pair<std::string, std::map<std::string, std::string>> query(std::string_view uri) {
    const auto mark = uri.find('?');
    std::pair<std::string, std::map<std::string, std::string>> out;
    out.first = std::string(uri.substr(0, mark));
    if (mark == std::string_view::npos) {
        return out;
    }
    auto rest = uri.substr(mark + 1);
    while (!rest.empty()) {
        const auto amp = rest.find('&');
        const auto pair = rest.substr(0, amp);
        const auto equals = pair.find('=');
        out.second.emplace(unquote(pair.substr(0, equals)), equals == std::string_view::npos
                                                                ? ""
                                                                : unquote(pair.substr(equals + 1)));
        if (amp == std::string_view::npos) {
            break;
        }
        rest = rest.substr(amp + 1);
    }
    return out;
}

inline std::string scheme_of(std::string_view uri) {
    return std::string(uri.substr(0, uri.find(':')));
}

// An agent folder, home and table of the three agents (CLAUDE, CODEX, OPENCODE of the Python
// tests), installed for the length of a test.
struct ActionsFixture : AgentsFixture {
    std::shared_ptr<TableAgents> table = make<TableAgents>();
    ScopedAgents scoped{table};
    agents::InstalledAgent claude{"claude_code", temp.path() / "agents" / "claude.exe", "2.1.296"};
    agents::InstalledAgent codex{"codex", temp.path() / "agents" / "codex.exe", "0.162.1"};
    agents::InstalledAgent opencode{"opencode_cli", temp.path() / "agents" / "opencode.exe",
                                    "2.0.18"};

    ActionsFixture() {
        for (const auto* agent : {&claude, &codex, &opencode}) {
            table->set(*agent);
        }
    }
    const agents::InstalledAgent& agent(std::string_view id) const {
        return id == "claude_code" ? claude : id == "codex" ? codex : opencode;
    }
};

} // namespace fv::agents_test
