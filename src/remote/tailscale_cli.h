#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/env.h"
#include "base/json.h"
#include "net/tailscale.h"

// Tailscale, when this computer has it: the CLI runs that answer `status --json` and
// `whois --json` (services/remote_asr/tailscale.py's find_cli, _run_json, status, whois and
// own_device). Both are read-only questions the local tailscaled answers from what it already
// knows; they need no admin rights, and nothing here talks to Tailscale's servers. The host uses
// whois to learn which Tailscale user owns the computer on the other end of a connection, which
// Tailscale learns from the WireGuard key the packets arrived under, so it can't be claimed by
// the connecting side. Status parsing is net::parse_status.
namespace fv {
class ProcessLauncher;
}

namespace fv::remote {

using PeerIdentity = net::PeerIdentity;

// The CLI is missing, tailscaled isn't running, or its answer was unreadable.
class TailscaleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// How long one CLI run may take, in seconds.
inline constexpr double TAILSCALE_TIMEOUT_S = 4.0;
// What whois reports as the owner of a tagged (server) node.
inline constexpr std::string_view TAGGED_OWNER = "tagged-devices";

enum class CliPlatform { win32, darwin, other }; // as Python's sys.platform tells them apart
CliPlatform current_cli_platform();

// The `tailscale` executable: shutil.which("tailscale") over the environment's PATH (with
// PATHEXT on Windows), else where the installers put it: %ProgramFiles%\Tailscale\tailscale.exe
// then %ProgramFiles(x86)%\... on Windows; the app bundle, Homebrew and /usr/local on macOS;
// /usr/bin, /usr/local/bin and /snap/bin elsewhere. Unlike shutil.which on Windows, the current
// folder is not searched first. nullopt when none is a file.
std::optional<std::filesystem::path> find_cli(const EnvReader& env,
                                              CliPlatform platform = current_cli_platform());

// Variables a CLI run adds to the user's environment: on macOS TAILSCALE_BE_CLI=1, because the
// app bundle's executable also holds the CLI and, started without terminal variables (from
// Finder or the Dock), would otherwise open the app and print no JSON; nothing elsewhere.
std::map<std::string, std::string> cli_extra_env(CliPlatform platform);

// whois's reading of a `whois --json` document: the node's ComputedName, else the first label of
// its Name, else the address; the owner's LoginName unless the node is tagged (Tags, or the owner
// "tagged-devices"). nullopt for a document that is not an object (the CLI's answer was
// unreadable).
std::optional<PeerIdentity> parse_whois(const Json& data, std::string_view address);

// Affinity: thread-safe; blocking (each call may run the CLI for up to its timeout), never on
// the GUI thread.
class TailscaleCli {
public:
    virtual ~TailscaleCli() = default;
    // This computer's Tailscale state and its peers. Never throws: "not_installed" when there is
    // no CLI, "unavailable" with the reason when it fails or its answer is unreadable.
    virtual net::TailscaleStatus status() = 0;
    // Which Tailscale node and user `address` belongs to; nullopt for an address that is not
    // Tailscale's or when the CLI fails (logged).
    virtual std::optional<PeerIdentity> whois(std::string_view address) = 0;
};

// The CLI found through `env` (find_cli), run without a window, with stdin closed and the user's
// environment (process/process_env.h child_env), for at most `timeout_s`. A failed run is a
// TailscaleError with the first line of its stderr, else "Tailscale isn't running"; a slow one
// "Tailscale didn't answer in time" (it is stopped); one that couldn't start "Couldn't run
// Tailscale: <why>"; output that is not a JSON object "Tailscale's answer was unreadable".
// `env` and `launcher` must outlive it; a null launcher is the real one.
class ProcessTailscaleCli final : public TailscaleCli {
public:
    explicit ProcessTailscaleCli(const EnvReader& env,
                                 std::shared_ptr<ProcessLauncher> launcher = nullptr,
                                 double timeout_s = TAILSCALE_TIMEOUT_S,
                                 CliPlatform platform = current_cli_platform());

    net::TailscaleStatus status() override;
    std::optional<PeerIdentity> whois(std::string_view address) override;

    // _run_json: the CLI's answer to `args` as a JSON object. Throws TailscaleError as above, and
    // CliMissing (Python's FileNotFoundError("tailscale")) when there is no CLI.
    Json run_json(const std::vector<std::string>& args);

    struct CliMissing : std::runtime_error {
        CliMissing() : std::runtime_error("tailscale") {}
    };

private:
    const EnvReader& env_;
    std::shared_ptr<ProcessLauncher> launcher_;
    double timeout_s_;
    CliPlatform platform_;
};

// ProcessTailscaleCli over the real launcher.
std::unique_ptr<TailscaleCli> make_tailscale_cli(const EnvReader& env);

// The peer behind a connection, if it's one of `owner`'s own computers: both ends Tailscale
// addresses (so the connection came in over the tailnet) and whois naming the same, untagged
// user. whois is asked only when the rest holds.
std::optional<PeerIdentity> own_device(std::string_view remote, std::string_view local,
                                       std::string_view owner, TailscaleCli& cli);

} // namespace fv::remote
