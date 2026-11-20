// fiorivoce-cli --channel. It answers Claude Code's initialize before anything loads a speech
// runtime, opens the database or touches the data folder.

#include "cli/channel_command.h"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>

#include "automation/channel_bridge.h"
#include "automation/channel_link.h"
#include "base/env.h"
#include "base/log.h"
#include "engine/version.h"
#include "update/check.h"
#include "update/gates.h"
#include "update/install_probe.h"
#include "update/release_identity.h"

namespace fv::cli {
namespace {

constexpr log::Logger kLog{"services.automation.channel_bridge"};

// What the exception was, for the log, as type(exc).__name__: never its text, which may quote a
// request. "class std::runtime_error" (MSVC) or a mangled name (GCC) becomes the last word.
std::string type_of(const std::exception& error) {
    std::string name = typeid(error).name();
    if (const std::size_t space = name.rfind(' '); space != std::string::npos) {
        name.erase(0, space + 1);
    }
    if (const std::size_t scope = name.rfind("::"); scope != std::string::npos) {
        name.erase(0, scope + 2);
    }
    return name;
}

} // namespace

bool update_in_progress() {
#ifdef _WIN32
    const update::ReleaseIdentity& release = update::release_identity();
    if (update::detect_channel(update::probe_install(release)) !=
        update::InstallChannel::Installer) {
        return false;
    }
    update::MutexApi& mutexes = update::system_mutex_api();
    for (const std::string& name : release.update_mutexes()) {
        if (mutexes.exists(name)) {
            return true;
        }
    }
    return mutexes.exists(release.setup_mutex());
#else
    return false;
#endif
}

int run_channel(std::span<const std::string> args) {
    if (!args.empty()) {
        const bool help_asked = args.front() == "-h" || args.front() == "--help";
        const std::string text =
            help_asked ? automation::channel_usage("fiorivoce-cli")
                       : std::string("Unexpected arguments. Run --channel --help for usage.\n");
        std::fputs(text.c_str(), stderr);
        return help_asked ? 0 : 2;
    }
    // Nothing may escape: the windowed app that forwards here would have no one to tell.
    try {
        automation::configure_channel_logging(system_env());
        std::optional<automation::ProtocolStreams> streams = automation::protocol_streams();
        if (!streams) {
            kLog.error("Claude Code starts the channel with pipes on stdin and stdout; it can't "
                       "run without them.");
            return 2;
        }
        automation::AppLink link(automation::AppLink::from_environment(system_env(), kVersion));
        automation::ChannelServerOptions options;
        options.version = kVersion;
        options.update_check = update_in_progress;
        automation::ChannelServer server(*streams->reader, *streams->writer, link,
                                         std::move(options));
        return server.serve();
    } catch (const std::exception& error) {
        kLog.error("The channel stopped: {}", type_of(error));
        return 1;
    }
}

} // namespace fv::cli
