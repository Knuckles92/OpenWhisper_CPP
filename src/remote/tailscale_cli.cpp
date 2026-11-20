#include "remote/tailscale_cli.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "process/launcher.h"
#include "process/process_env.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "remote/detail/pyvalue.h"

namespace fv::remote {

namespace {

constexpr log::Logger kLog("services.remote_asr.tailscale");

// shutil._WIN_DEFAULT_PATHEXT.
constexpr std::string_view kDefaultPathext = ".COM;.EXE;.BAT;.CMD;.VBS;.JS;.WS;.MSC";

const Json& member(const Json& object, std::string_view key) {
    static const Json none;
    if (!object.is_object()) {
        return none;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? none : *found;
}

std::vector<std::string> split_on(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(separator, start);
        parts.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool is_runnable(const std::filesystem::path& path, CliPlatform platform) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status) || std::filesystem::is_directory(status)) {
        return false;
    }
    if (platform == CliPlatform::win32) {
        return true; // os.access(X_OK) is true for any existing file there
    }
    using std::filesystem::perms;
    return (status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) !=
           perms::none;
}

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

// shutil.which("tailscale"): the directories of PATH in order, each once, and on Windows the
// name with each PATHEXT extension. Empty entries (which Python resolves against the current
// folder) are skipped, as the current folder itself is.
std::optional<std::filesystem::path> which(const EnvReader& env, CliPlatform platform) {
    const auto path = env.get("PATH");
    if (!path || path->empty()) {
        return std::nullopt;
    }
    const bool windows = platform == CliPlatform::win32;
    std::vector<std::string> files;
    if (windows) {
        const auto pathext = env.get("PATHEXT");
        const std::string source =
            pathext && !pathext->empty() ? *pathext : std::string(kDefaultPathext);
        for (auto ext : split_on(source, ';')) {
            if (ext.empty()) {
                continue;
            }
            // ext.rstrip('.'): ".EXE." names the same files as ".EXE".
            while (!ext.empty() && ext.back() == '.') {
                ext.pop_back();
            }
            files.push_back("tailscale" + ext);
        }
    } else {
        files.emplace_back("tailscale");
    }
    std::set<std::string> seen;
    for (const auto& dir : split_on(*path, windows ? ';' : ':')) {
        if (dir.empty()) {
            continue;
        }
        if (!seen.insert(windows ? lower_ascii(dir) : dir).second) {
            continue;
        }
        for (const auto& file : files) {
            const auto candidate = path_from_utf8(dir) / path_from_utf8(file);
            if (is_runnable(candidate, platform)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

// The first line of str.splitlines() of the stripped text.
std::string first_line(std::string_view text) {
    const std::string stripped = py::strip(text);
    std::size_t pos = 0;
    while (pos < stripped.size()) {
        const std::size_t start = pos;
        const char32_t cp = py::next_code_point(stripped, pos);
        switch (cp) {
        case U'\n':
        case U'\r':
        case U'\v':
        case U'\f':
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x85:
        case 0x2028:
        case 0x2029:
            return stripped.substr(0, start);
        default:
            break;
        }
    }
    return stripped;
}

std::string short_name(std::string_view dns_name, std::string_view fallback) {
    std::string label;
    if (!dns_name.empty()) {
        label = std::string(dns_name.substr(0, dns_name.find('.')));
    }
    return label.empty() ? std::string(fallback) : label;
}

struct Captured {
    std::mutex mutex;
    std::condition_variable ended;
    std::string out;
    std::string err;
    int open = 2;
};

} // namespace

CliPlatform current_cli_platform() {
#if defined(_WIN32)
    return CliPlatform::win32;
#elif defined(__APPLE__)
    return CliPlatform::darwin;
#else
    return CliPlatform::other;
#endif
}

std::map<std::string, std::string> cli_extra_env(CliPlatform platform) {
    if (platform == CliPlatform::darwin) {
        return {{"TAILSCALE_BE_CLI", "1"}};
    }
    return {};
}

std::optional<std::filesystem::path> find_cli(const EnvReader& env, CliPlatform platform) {
    if (auto found = which(env, platform)) {
        return found;
    }
    std::vector<std::filesystem::path> candidates;
    if (platform == CliPlatform::win32) {
        for (const char* root : {"ProgramFiles", "ProgramFiles(x86)"}) {
            const auto value = env.get(root);
            if (value && !value->empty()) {
                candidates.push_back(path_from_utf8(*value) / "Tailscale" / "tailscale.exe");
            }
        }
    } else if (platform == CliPlatform::darwin) {
        candidates = {"/Applications/Tailscale.app/Contents/MacOS/Tailscale",
                      "/opt/homebrew/bin/tailscale", "/usr/local/bin/tailscale"};
    } else {
        candidates = {"/usr/bin/tailscale", "/usr/local/bin/tailscale", "/snap/bin/tailscale"};
    }
    for (const auto& candidate : candidates) {
        if (is_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<PeerIdentity> parse_whois(const Json& data, std::string_view address) {
    if (!data.is_object()) {
        return std::nullopt;
    }
    const Json& node_raw = member(data, "Node");
    const Json& profile_raw = member(data, "UserProfile");
    const Json node = node_raw.is_object() ? node_raw : Json::object();
    const Json profile = profile_raw.is_object() ? profile_raw : Json::object();
    const std::string owner = detail::py_str_or_empty(member(profile, "LoginName"));
    const bool tagged = detail::truthy(member(node, "Tags")) || owner == TAGGED_OWNER;
    std::string name = detail::py_str_or_empty(member(node, "ComputedName"));
    if (name.empty()) {
        name = short_name(py::rstrip(detail::py_str_or_empty(member(node, "Name")), "."), address);
    }
    return PeerIdentity{std::move(name), tagged ? std::string() : owner, tagged};
}

ProcessTailscaleCli::ProcessTailscaleCli(const EnvReader& env,
                                         std::shared_ptr<ProcessLauncher> launcher,
                                         double timeout_s, CliPlatform platform)
    : env_(env), launcher_(launcher ? std::move(launcher) : make_process_launcher()),
      timeout_s_(timeout_s), platform_(platform) {}

Json ProcessTailscaleCli::run_json(const std::vector<std::string>& args) {
    const auto cli = find_cli(env_, platform_);
    if (!cli) {
        throw CliMissing();
    }
    ProcessSpec spec;
    spec.program = *cli;
    spec.args = args;
    spec.env = to_spec_env(child_env(cli_extra_env(platform_)));
    spec.own_tree = true;
    spec.pipes = true;
    spec.stdin_null = true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(timeout_s_));
    auto started = launcher_->start(spec);
    if (!started) {
        throw TailscaleError("Couldn't run Tailscale: " + started.error());
    }
    Process& process = **started;
    auto captured = std::make_shared<Captured>();
    const auto sink = [captured](std::string Captured::*target) {
        return [captured, target](std::span<const char> bytes) {
            {
                std::lock_guard lock(captured->mutex);
                if (bytes.empty()) {
                    --captured->open;
                } else {
                    ((*captured).*target).append(bytes.data(), bytes.size());
                }
            }
            captured->ended.notify_all();
        };
    };
    process.on_bytes(sink(&Captured::out), sink(&Captured::err));
    const auto remaining = [&] {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds(0));
    };
    const auto code = process.wait(remaining());
    bool drained = false;
    if (code) {
        std::unique_lock lock(captured->mutex);
        drained = captured->ended.wait_until(lock, deadline, [&] { return captured->open == 0; });
    }
    if (!code || !drained) {
        process.kill_tree();
        (void)process.wait(std::chrono::seconds(1));
        process.release_pipes(std::chrono::milliseconds(0));
        throw TailscaleError("Tailscale didn't answer in time");
    }
    process.release_pipes(std::chrono::milliseconds(0));
    std::string out;
    std::string err;
    {
        std::lock_guard lock(captured->mutex);
        out = std::move(captured->out);
        err = std::move(captured->err);
    }
    if (*code != 0) {
        const std::string line = first_line(detail::decode_utf8_replace(err));
        throw TailscaleError(line.empty() ? "Tailscale isn't running" : line);
    }
    Json data;
    try {
        data = py::loads(detail::decode_utf8_replace(out));
    } catch (const py::PyError&) {
        throw TailscaleError("Tailscale's answer was unreadable");
    }
    if (!data.is_object()) {
        throw TailscaleError("Tailscale's answer was unreadable");
    }
    return data;
}

net::TailscaleStatus ProcessTailscaleCli::status() {
    Json data;
    try {
        data = run_json({"status", "--json"});
    } catch (const CliMissing&) {
        net::TailscaleStatus status;
        status.state = "not_installed";
        return status;
    } catch (const std::exception& error) {
        net::TailscaleStatus status;
        status.state = "unavailable";
        status.detail = error.what();
        return status;
    }
    try {
        return net::parse_status(data);
    } catch (const std::exception&) {
        // Python's parse_status would raise TypeError out of status() for such a document; the
        // contract here is that status() never throws.
        net::TailscaleStatus status;
        status.state = "unavailable";
        status.detail = "Tailscale's answer was unreadable";
        return status;
    }
}

std::optional<PeerIdentity> ProcessTailscaleCli::whois(std::string_view address) {
    if (!net::is_tailscale_address(address)) {
        return std::nullopt;
    }
    Json data;
    try {
        data = run_json({"whois", "--json", std::string(address)});
    } catch (const std::exception& error) {
        kLog.info("Tailscale whois for {} failed: {}", address, error.what());
        return std::nullopt;
    }
    return parse_whois(data, address);
}

std::unique_ptr<TailscaleCli> make_tailscale_cli(const EnvReader& env) {
    return std::make_unique<ProcessTailscaleCli>(env);
}

std::optional<PeerIdentity> own_device(std::string_view remote, std::string_view local,
                                       std::string_view owner, TailscaleCli& cli) {
    if (owner.empty() || !net::is_tailscale_address(remote) || !net::is_tailscale_address(local)) {
        return std::nullopt;
    }
    auto identity = cli.whois(remote);
    if (!identity || identity->tagged || identity->owner != owner) {
        return std::nullopt;
    }
    return identity;
}

} // namespace fv::remote
