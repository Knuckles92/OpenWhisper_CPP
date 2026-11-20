// The checks that need the data folder and the settings: --diagnostics and --self-test.
// Neither starts a process, opens the data folder's database or loads the NeMo runtime; the
// self-test's workflow smoke works in a temporary folder of its own.

#include "cli/commands.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "cli/workflow_smoke.h"
#include "engine/runtime.h"
#include "engine/version.h"
#include "log/diagnostics_report.h"
#include "models/component_store.h"
#include "models/engine_resolution.h"
#include "models/gpu_probe.h"
#include "models/speech_catalog.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "update/install_probe.h"
#include "update/release_identity.h"

namespace fv::cli {
namespace {

constexpr const char* kDiagnosticsUsage = "usage: fiorivoce-cli --diagnostics [-h] output\n";

// argparse's exit for a bad command line: usage, then "<prog>: error: <message>", status 2.
int diagnostics_usage_error(const std::string& message) {
    std::fputs(kDiagnosticsUsage, stderr);
    std::fprintf(stderr, "fiorivoce-cli --diagnostics: error: %s\n", message.c_str());
    return 2;
}

std::string diagnostics_help() {
    return std::string(kDiagnosticsUsage) +
           "\n"
           "Export a local, sanitized " +
           std::string(identity().product) +
           " support report. Nothing is uploaded.\n"
           "\n"
           "positional arguments:\n"
           "  output      New JSON file to create (existing files are not overwritten)\n"
           "\n"
           "options:\n"
           "  -h, --help  show this help message and exit\n";
}

// argparse's _parse_optional for a parser whose only option is -h/--help: "--help" and its
// unique abbreviations ("--h", "--he", "--hel") are help, and an argument that starts with "-" is
// an option unless it is "-" alone, a negative number or contains a space.
bool is_help(const std::string& arg) {
    return arg == "-h" || (arg.size() >= 3 && std::string_view("--help").starts_with(arg));
}

bool is_option(const std::string& arg) {
    if (arg.size() < 2 || arg.front() != '-') {
        return false;
    }
    // ^-\d+$|^-\d*\.\d+$
    const std::string_view rest = std::string_view(arg).substr(1);
    const std::size_t dot = rest.find('.');
    const auto digits = [](std::string_view text) {
        return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    const bool negative_number =
        dot == std::string_view::npos
            ? digits(rest)
            : digits(rest.substr(0, dot)) && rest.size() > dot + 1 && digits(rest.substr(dot + 1));
    return !negative_number && arg.find(' ') == std::string::npos;
}

// The data folder must take a new file; the probe file is removed again.
void check_data_root_writable(const std::filesystem::path& root) {
    const std::filesystem::path probe = root / ".self-test-write";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        out << "ok";
        out.flush();
        if (!out) {
            throw std::runtime_error("The data folder " + path_to_utf8(root) + " is not writable.");
        }
    }
    std::error_code ignored;
    std::filesystem::remove(probe, ignored);
}

// The runtime folder to check: the environment's, which names one run, before the saved setting.
std::optional<std::string> configured_runtime_dir(const SettingsStore& settings) {
    const AppIdentity& id = identity();
    const std::string variable = std::string(id.env_prefix) + "RUNTIME_DIR";
    if (const std::optional<std::string> value = system_env().get(variable);
        value && !value->empty()) {
        return value;
    }
    std::string saved = settings.get(keys::fv_runtime_dir);
    if (!saved.empty()) {
        return saved;
    }
    return std::nullopt;
}

// The first NVIDIA GPU as the report names it (services/gpu_info.nvidia_gpu).
std::optional<log::GpuInfo> report_gpu() {
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
std::optional<std::string> installed_runtime_version(const Json& settings, const Paths& paths) {
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

// `install` is asked only once the command line asks for a report.
int write_diagnostics(std::span<const std::string> args,
                      const std::function<const update::InstallProbe&()>& install) {
    // argparse with one positional: -h anywhere answers first; other options and surplus words
    // are "unrecognized arguments", reported after a missing positional.
    std::vector<std::string> positionals;
    std::vector<std::string> unrecognized;
    bool options_ended = false;
    for (const std::string& arg : args) {
        if (!options_ended && arg == "--") {
            options_ended = true;
        } else if (!options_ended && is_help(arg)) {
            std::fputs(diagnostics_help().c_str(), stdout);
            return 0;
        } else if (!options_ended && is_option(arg)) {
            unrecognized.push_back(arg);
        } else if (positionals.empty()) {
            positionals.push_back(arg);
        } else {
            unrecognized.push_back(arg);
        }
    }
    if (positionals.empty()) {
        return diagnostics_usage_error("the following arguments are required: output");
    }
    if (!unrecognized.empty()) {
        std::string joined;
        for (const std::string& arg : unrecognized) {
            joined += (joined.empty() ? "" : " ") + arg;
        }
        return diagnostics_usage_error("unrecognized arguments: " + joined);
    }

    const AppIdentity& id = identity();
    const Paths paths = Paths::resolve(id, system_env());
    const Json settings = log::read_json_file(paths.user_file(id.settings_file));

    // The runtimes are not loaded for a report that must answer at once: the speech runtime's
    // version is its installed component's, and the others are the "not loaded" null.
    log::ReportInputs inputs;
    inputs.app_version = kVersion;
    inputs.packaged = update::detect_channel(install()) == update::InstallChannel::Installer;
    inputs.settings = &settings;
    inputs.data_root = paths.data_root();
    inputs.components = {{"qt", std::nullopt},
                         {"nemo_speech_runtime", installed_runtime_version(settings, paths)},
                         {"sqlite", std::nullopt},
                         {"openssl", std::nullopt}};
    inputs.nvidia_gpu = report_gpu;
    inputs.catalog_model = [](std::string_view key) {
        return models::model(key) != nullptr;
    };
    const Json report = log::collect_report(inputs);

    const std::filesystem::path output = path_from_utf8(positionals.front());
    try {
        log::write_report_new(output, report);
    } catch (const std::filesystem::filesystem_error& error) {
        // filesystem_error::what() spells the path in the ANSI code page; the argument is UTF-8.
        if (error.code() == std::errc::file_exists) {
            std::fprintf(stderr,
                         "fiorivoce-cli: %s already exists, and a report never replaces a file. "
                         "Choose a new file name.\n",
                         positionals.front().c_str());
        } else {
            std::fprintf(stderr, "fiorivoce-cli: The report could not be written to %s (%s).\n",
                         positionals.front().c_str(), error.code().message().c_str());
        }
        return 1;
    }
    return 0;
}

} // namespace

int run_diagnostics(std::span<const std::string> args) {
    // Reads the install registration only for a command line that writes a report.
    std::optional<update::InstallProbe> install;
    return write_diagnostics(args, [&install]() -> const update::InstallProbe& {
        if (!install) {
            install = update::probe_install(update::release_identity());
        }
        return *install;
    });
}

int run_diagnostics(std::span<const std::string> args, const update::InstallProbe& install) {
    return write_diagnostics(args, [&install]() -> const update::InstallProbe& { return install; });
}

int run_self_test() {
    try {
        const AppIdentity& id = identity();
        const Paths paths = Paths::resolve(id, system_env());
        check_data_root_writable(paths.data_root());

        const SettingsStore settings(paths.user_file(id.settings_file));
        if (!settings.snapshot()->is_object()) {
            throw std::runtime_error("The settings did not load.");
        }

        const std::optional<std::string> runtime_dir = configured_runtime_dir(settings);
        if (runtime_dir) {
            const std::filesystem::path library =
                find_runtime_library(path_from_utf8(*runtime_dir));
            std::printf("Speech runtime: %s\n", path_to_utf8(library).c_str());
        } else {
            std::printf("Speech runtime: none configured, not checked\n");
        }
        std::fflush(stdout);
        // main.py's _run_package_self_test ends with the workflow smoke, which prints its report.
        run_workflow_smoke();
        std::printf("%s package self-test passed\n", std::string(id.product).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fiorivoce-cli: self-test failed: %s\n", error.what());
        return 1;
    }
}

} // namespace fv::cli
