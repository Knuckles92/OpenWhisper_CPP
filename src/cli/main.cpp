// fiorivoce-cli: the speech engine and the checks that need no window. Qt-free.
//
// Dispatch is on the first argument only, as OpenWhisper's _handle_early_cli: --help and -h answer
// whatever follows, --diagnostics, --api, --channel and --workflow-smoke take the rest of the
// line, and --version
// and --self-test count only when they are the whole command line (--self-test also as
// "--self-test --installed"). Anything else is the transcribe command's business.
// --help and --version answer before anything reads the settings, touches the data folder or
// loads the NeMo runtime.

#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cli/transcribe.h"
#include "cli/usage.h"
#include "engine/error.h"
#include "engine/platform.h"
#include "engine/version.h"

#ifdef FV_CLI_ENGINE_ONLY
// The engine-only configuration builds without fv_base, so identity() is not there.
namespace {
constexpr std::string_view kProduct = "FioriVoce";
constexpr std::string_view kEnvPrefix = "FIORIVOCE_";
} // namespace
#else
#include "base/identity.h"
#include "cli/api_command.h"
#include "cli/channel_command.h"
#include "cli/commands.h"
#include "cli/installed_check.h"
#include "cli/workflow_smoke.h"
namespace {
const std::string_view kProduct = fv::identity().product;
const std::string_view kEnvPrefix = fv::identity().env_prefix;
} // namespace
#endif

namespace {

bool is_installed_check(const std::vector<std::string>& args) {
    return args.size() == 3 && args[2] == "--installed";
}

#ifdef FV_CLI_ENGINE_ONLY
int not_in_this_build(std::string_view flag) {
    std::fprintf(stderr,
                 "fiorivoce-cli: %s is not in the engine-only build. Configure with a vcpkg "
                 "preset (see BUILDING.md).\n",
                 std::string(flag).c_str());
    return 1;
}
#endif

} // namespace

int main(int argc, char** argv) {
    fv::use_utf8_console();
    const std::vector<std::string> args = fv::utf8_args(argc, argv);
    const std::string_view command = args.size() > 1 ? std::string_view(args[1]) : "";
    const std::string usage = fv::cli::usage_text(kProduct, kEnvPrefix);

    if (command == "--help" || command == "-h") {
        std::fputs(usage.c_str(), stdout);
        return 0;
    }
    if (command == "--version" && args.size() == 2) {
        std::printf("%s %s\n", std::string(kProduct).c_str(), fv::kVersion);
        return 0;
    }
    try {
#ifdef FV_CLI_ENGINE_ONLY
        if (command == "--diagnostics") {
            return not_in_this_build(command);
        }
        if (command == "--self-test" && (args.size() == 2 || is_installed_check(args))) {
            return not_in_this_build(command);
        }
        if (command == "--workflow-smoke" || command == "--api" || command == "--channel") {
            return not_in_this_build(command);
        }
#else
        // main.py's _handle_early_cli: --api and --channel take the rest of the line, and answer
        // before anything loads a speech runtime (Claude Code waits on --channel's initialize).
        if (command == "--api") {
            return fv::cli::run_api(std::span<const std::string>(args).subspan(2));
        }
        if (command == "--channel") {
            return fv::cli::run_channel(std::span<const std::string>(args).subspan(2));
        }
        if (command == "--diagnostics") {
            return fv::cli::run_diagnostics(std::span<const std::string>(args).subspan(2));
        }
        if (command == "--self-test" && args.size() == 2) {
            return fv::cli::run_self_test();
        }
        if (command == "--self-test" && is_installed_check(args)) {
            return fv::cli::run_installed_self_test();
        }
        if (command == "--workflow-smoke") {
            return fv::cli::run_workflow_smoke_command(
                std::span<const std::string>(args).subspan(2));
        }
#endif
        if (command != "transcribe") {
            throw fv::cli::UsageError(command.empty()
                                          ? "No command given."
                                          : "Unknown command " + std::string(command) + ".");
        }
        return fv::cli::transcribe(args, kEnvPrefix);
    } catch (const fv::cli::UsageError& error) {
        std::fprintf(stderr, "fiorivoce-cli: %s\n\n%s", error.what(), usage.c_str());
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fiorivoce-cli: %s\n", error.what());
        return 1;
    }
}
