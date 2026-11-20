#include "ui_shell/early_cli.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <QtGlobal>

#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "engine/version.h"
#include "log/diagnostics_report.h"
#include "models/component_store.h"
#include "models/engine_resolution.h"
#include "models/gpu_probe.h"
#include "models/speech_catalog.h"
#include "ui_shell/self_test.h"
#include "update/install_probe.h"
#include "update/release_identity.h"

namespace fv {
namespace {

// argparse's _parse_optional for a parser whose only option is -h/--help: "--help" and its
// unique abbreviations ("--h", "--he", "--hel") are help.
bool isHelp(const std::string& arg) {
    return arg == "-h" || (arg.size() >= 3 && std::string_view("--help").starts_with(arg));
}

// An argument that starts with "-" is an option unless it is "-" alone, a negative number
// (^-\d+$|^-\d*\.\d+$) or contains a space.
bool isOption(const std::string& arg) {
    if (arg.size() < 2 || arg.front() != '-') {
        return false;
    }
    const std::string_view rest = std::string_view(arg).substr(1);
    const std::size_t dot = rest.find('.');
    const auto digits = [](std::string_view text) {
        return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    const bool negativeNumber =
        dot == std::string_view::npos
            ? digits(rest)
            : digits(rest.substr(0, dot)) && rest.size() > dot + 1 && digits(rest.substr(dot + 1));
    return !negativeNumber && arg.find(' ') == std::string::npos;
}

std::string diagnosticsUsage(std::string_view program) {
    return "usage: " + std::string(program) + " [-h] output\n";
}

int diagnosticsUsageError(std::string_view program, const std::string& message) {
    std::fputs(diagnosticsUsage(program).c_str(), stderr);
    std::fprintf(stderr, "%s: error: %s\n", std::string(program).c_str(), message.c_str());
    return 2;
}

std::string diagnosticsHelp(std::string_view program) {
    return diagnosticsUsage(program) + "\nExport a local, sanitized " +
           std::string(identity().product) +
           " support report. Nothing is uploaded.\n"
           "\n"
           "positional arguments:\n"
           "  output      New JSON file to create (existing files are not overwritten)\n"
           "\n"
           "options:\n"
           "  -h, --help  show this help message and exit\n";
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

// The first NVIDIA GPU as the report names it (services/gpu_info.nvidia_gpu).
std::optional<log::GpuInfo> reportGpu() {
    const std::optional<models::NvidiaGpu> gpu = models::nvidia_gpu();
    if (!gpu) {
        return std::nullopt;
    }
    log::GpuInfo info;
    info.name = gpu->name;
    info.total_mib = gpu->total_mib;
    info.compute_capability = gpu->compute_capability;
    return info;
}

// The version of the speech runtime component the saved settings select, when it is installed
// (its sentinel is there); nullopt otherwise. Nothing is loaded.
std::optional<std::string> installedRuntimeVersion(const Json& settings, const Paths& paths) {
    try {
        const Json saved = settings.is_object() ? settings : Json::object();
        const models::ResolvedEngine engine = models::resolve_parakeet(saved, paths);
        if (engine.runtime_id.empty()) {
            return std::nullopt;
        }
        const std::filesystem::path folder = paths.components_root() / engine.runtime_id;
        std::error_code error;
        if (!std::filesystem::exists(folder / ".installed", error)) {
            return std::nullopt;
        }
        const std::optional<Json> manifest = models::read_manifest_file(folder / "manifest.json");
        if (!manifest || !manifest->contains("version")) {
            return std::nullopt;
        }
        const Json& version = (*manifest)["version"];
        return version.is_string() ? version.get<std::string>() : version.dump();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

EarlyCli parseEarlyCli(std::span<const std::string> args) {
    EarlyCli cli;
    if (args.size() < 2) {
        return cli;
    }
    const std::string& first = args[1];
    if (first == "-h" || first == "--help") {
        cli.command = EarlyCommand::Help;
    } else if (first == "--diagnostics") {
        cli.command = EarlyCommand::Diagnostics;
        cli.rest.assign(args.begin() + 2, args.end());
    } else if (first == "--version" && args.size() == 2) {
        cli.command = EarlyCommand::Version;
    } else if (first == "--self-test-gui" && args.size() == 2) {
        cli.command = EarlyCommand::SelfTestGui;
    }
    return cli;
}

// Laid out as main.py's _USAGE: option names from column 2, descriptions from column 26, no line
// over 78 characters.
std::string guiUsageText(std::string_view executable, std::string_view product,
                         std::string_view envPrefix) {
    const std::string prefix(envPrefix);
    std::string text = "Usage: " + std::string(executable) +
                       " [OPTION]\n"
                       "\n"
                       "With no option, opens " +
                       std::string(product) +
                       ".\n"
                       "\n"
                       "Options:\n"
                       "  -h, --help              Show this help and exit.\n"
                       "  --version               Print the version and exit.\n"
                       "  --diagnostics OUTPUT.json\n"
                       "                          Write a sanitized support report to a new JSON "
                       "file\n"
                       "                          and exit. Nothing is uploaded.\n"
                       "  --api [--database PATH] [--port N]\n"
                       "                          Serve the read-only History API on 127.0.0.1 "
                       "(port\n"
                       "                          8766 by default) without opening the app. Set\n"
                       "                          ";
    text += prefix + "API_TOKEN to a random token of at least\n";
    text += "                          32 characters first.\n"
            "  --channel               Run as a Claude Code channel (an MCP server over\n"
            "                          stdio) that forwards ";
    text += std::string(product) + " actions into a\n" +
            "                          running Claude Code session. " + std::string(product) +
            " must be\n"
            "                          running with API & MCP on.\n";
    text += "  --self-test-gui         Check that the window system, the theme and the\n"
            "                          bundled icons load, and exit.\n"
            "\n"
            "Environment:\n"
            "  ";
    text += prefix + "DATA_DIR=PATH\n"
                     "                          Keep settings, history and recordings in PATH.\n"
                     "  ";
    text += prefix + "LOG_LEVEL=LEVEL\n"
                     "                          Log at DEBUG, INFO (the default), WARNING or "
                     "ERROR.\n";
    return text;
}

namespace {

// `install` is asked only once the command line asks for a report.
int writeDiagnostics(std::span<const std::string> args, std::string_view program,
                     const std::function<const update::InstallProbe&()>& install) {
    // argparse with one positional: -h anywhere answers first; other options and surplus words
    // are "unrecognized arguments", reported after a missing positional.
    std::vector<std::string> positionals;
    std::vector<std::string> unrecognized;
    bool optionsEnded = false;
    for (const std::string& arg : args) {
        if (!optionsEnded && arg == "--") {
            optionsEnded = true;
        } else if (!optionsEnded && isHelp(arg)) {
            std::fputs(diagnosticsHelp(program).c_str(), stdout);
            return 0;
        } else if (!optionsEnded && isOption(arg)) {
            unrecognized.push_back(arg);
        } else if (positionals.empty()) {
            positionals.push_back(arg);
        } else {
            unrecognized.push_back(arg);
        }
    }
    if (positionals.empty()) {
        return diagnosticsUsageError(program, "the following arguments are required: output");
    }
    if (!unrecognized.empty()) {
        std::string joined;
        for (const std::string& arg : unrecognized) {
            joined += (joined.empty() ? "" : " ") + arg;
        }
        return diagnosticsUsageError(program, "unrecognized arguments: " + joined);
    }

    const AppIdentity& id = identity();
    const Paths paths = Paths::resolve(id, system_env());
    const Json settings = log::read_json_file(paths.user_file(id.settings_file));
    log::ReportInputs inputs;
    inputs.app_version = kVersion;
    inputs.packaged = update::detect_channel(install()) == update::InstallChannel::Installer;
    inputs.settings = &settings;
    inputs.data_root = paths.data_root();
    // Qt is linked into this executable, so its version is known without starting it; the
    // runtimes are not loaded for a report that must answer at once, so the speech runtime's
    // version is its installed component's.
    inputs.components = {{"qt", std::string(qVersion())},
                         {"nemo_speech_runtime", installedRuntimeVersion(settings, paths)},
                         {"sqlite", std::nullopt},
                         {"openssl", std::nullopt}};
    inputs.nvidia_gpu = reportGpu;
    inputs.catalog_model = [](std::string_view key) {
        return models::model(key) != nullptr;
    };
    const Json report = log::collect_report(inputs);

    const std::string& output = positionals.front();
    const std::string name = std::string(program);
    try {
        log::write_report_new(path_from_utf8(output), report);
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() == std::errc::file_exists) {
            std::fprintf(stderr,
                         "%s: %s already exists, and a report never replaces a file. Choose a "
                         "new file name.\n",
                         name.c_str(), output.c_str());
        } else {
            std::fprintf(stderr, "%s: The report could not be written to %s (%s).\n", name.c_str(),
                         output.c_str(), error.code().message().c_str());
        }
        return 1;
    }
    return 0;
}

} // namespace

int runDiagnostics(std::span<const std::string> args, std::string_view program) {
    // Reads the install registration only for a command line that writes a report.
    std::optional<update::InstallProbe> install;
    return writeDiagnostics(args, program, [&install]() -> const update::InstallProbe& {
        if (!install) {
            install = update::probe_install(update::release_identity());
        }
        return *install;
    });
}

int runDiagnostics(std::span<const std::string> args, std::string_view program,
                   const update::InstallProbe& install) {
    return writeDiagnostics(args, program,
                            [&install]() -> const update::InstallProbe& { return install; });
}

int runEarlyCommand(const EarlyCli& cli, std::string_view program, int argc, char** argv) {
    attachParentConsole();
    const AppIdentity& id = identity();
    switch (cli.command) {
    case EarlyCommand::Help: {
        const std::string usage = guiUsageText(lowered(id.product), id.product, id.env_prefix);
        std::fputs(usage.c_str(), stdout);
        std::fflush(stdout);
        return 0;
    }
    case EarlyCommand::Version:
        std::printf("%s %s\n", std::string(id.product).c_str(), kVersion);
        std::fflush(stdout);
        return 0;
    case EarlyCommand::Diagnostics: {
        const int code = runDiagnostics(cli.rest, program);
        std::fflush(stdout);
        std::fflush(stderr);
        return code;
    }
    case EarlyCommand::SelfTestGui: {
        const int code = runSelfTestGui(argc, argv);
        std::fflush(stdout);
        std::fflush(stderr);
        return code;
    }
    case EarlyCommand::Gui:
        break;
    }
    throw std::logic_error("Opening the app is not an early command.");
}

} // namespace fv
