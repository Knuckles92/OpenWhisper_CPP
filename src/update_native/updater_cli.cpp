#include "update_native/updater_cli.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <system_error>

#include "base/json.h"
#include "base/log.h"
#include "base/utf8.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include "update_native/setup_backup.h"
#include "update_native/transaction_runner.h"
#include "update_native/updater_log.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;

constexpr log::Logger kLog("scripts.updater_helper");

constexpr auto kOriginFile = "setup-origin.json";
constexpr auto kErrorFile = "setup-error.txt";

std::string usage(const UpdaterActions& actions) {
    std::string text;
    if (actions.setup_action)
        text +=
            "openwhisper-cpp-updater --setup-action prepare|finish|rollback --app-dir <install>\n";
    if (actions.run_transaction)
        text += "openwhisper-cpp-updater --apply --transaction-id <id>\n"
                "openwhisper-cpp-updater --recover --transaction-id <id>\n";
    text += "openwhisper-cpp-updater --write-manifest <tree> --version <version>\n"
            "openwhisper-cpp-updater --pack-archive <tree> --version <version> --out <zip>\n"
            "openwhisper-cpp-updater --verify-archive <zip> [--version <version>]\n";
    return text;
}

template <typename T> T positive_decimal(const std::string& value) {
    T parsed{};
    const auto end = value.data() + value.size();
    const auto [next, error] = std::from_chars(value.data(), end, parsed);
    if (error != std::errc{} || next != end || !parsed)
        throw std::invalid_argument("Invalid recovery launcher identity.");
    return parsed;
}

void print_to(const std::function<void(std::string_view)>& sink, std::FILE* stream,
              std::string_view text) {
    if (sink) {
        sink(text);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stream);
}
} // namespace

int run_setup_action(std::string_view action, const fs::path& install,
                     const SetupSession& session) {
    GuardedDirectory helper(session.helper_dir);
    try {
        const auto incoming =
            Json::parse(helper.read_file(update::manifest_name(), max_manifest_bytes));
        (void)validate_manifest(incoming);
        const bool registered = session.registered && session.registered(install);
        std::error_code error;
        if (fs::equivalent(session.helper_dir, install, error))
            throw UpdateError("Setup must use its temporary helper.");
        if (action == "prepare") {
            if (registered && session.retire_transactions)
                session.retire_transactions();
            prepare_setup(install, incoming, registered);
            helper.write_file_atomic(
                kOriginFile,
                Json{{"install", path_to_utf8(install)}, {"registered", registered}}.dump());
        } else if (action == "finish" || action == "rollback") {
            const auto origin = Json::parse(helper.read_file(kOriginFile, 32768));
            if (origin.at("install") != path_to_utf8(install))
                throw UpdateError("The setup destination changed after preparation.");
            if (action == "finish")
                finish_setup(install, incoming);
            else
                rollback_setup(install, incoming, origin.at("registered").get<bool>());
        } else {
            return 2;
        }
        return 0;
    } catch (const std::exception& failure) {
        kLog.error("Setup {} failed: {}", action, failure.what());
        helper.write_file_atomic(kErrorFile, failure.what());
        throw;
    }
}

int run_updater(std::span<const std::string> args, const UpdaterActions& actions) {
    const auto out = [&](std::string_view text) {
        print_to(actions.out, stdout, text);
    };
    const auto err = [&](std::string_view text) {
        print_to(actions.err, stderr, text);
    };
    const auto failed = [&](const std::exception& error) {
        err("openwhisper-cpp-updater: " + std::string(error.what()) + "\n");
        return 1;
    };
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        out(usage(actions));
        return 0;
    }
    const bool packaging =
        !args.empty() && (args[0] == "--write-manifest" || args[0] == "--pack-archive" ||
                          args[0] == "--verify-archive");
    const UpdaterLog log(packaging ? std::filesystem::path{} : actions.log_file);
    const bool from_gui =
        args.size() == 6 && args[0] == "--recover" && args[3] == "--wait-for-launcher";
    if (actions.run_transaction && (args.size() == 3 || from_gui) &&
        (args[0] == "--apply" || args[0] == "--recover") && args[1] == "--transaction-id") {
        std::uint32_t launcher_pid = 0;
        std::uint64_t launcher_time = 0;
        try {
            if (from_gui) {
                launcher_pid = positive_decimal<std::uint32_t>(args[4]);
                launcher_time = positive_decimal<std::uint64_t>(args[5]);
            }
        } catch (const std::exception& error) {
            return failed(error);
        }
        try {
            // Before anything is moved: a helper whose current directory is the install folder
            // keeps that folder from being renamed.
            if (actions.leave_launch_directory)
                actions.leave_launch_directory();
            const auto state = actions.run_transaction(args[2], args[0] == "--recover",
                                                       launcher_pid, launcher_time);
            out("Update transaction finished " + std::string(to_string(state)) + ".\n");
            return 0;
        } catch (const ReportedByApplication& error) {
            return failed(error);
        } catch (const UpdateError& error) {
            // Nobody else tells the user: the helper has no window of its own.
            if (actions.message_box)
                actions.message_box(error.what());
            return failed(error);
        } catch (const std::exception& error) {
            if (actions.message_box)
                actions.message_box("The update failed: " + std::string(error.what()));
            return failed(error);
        }
    }
    try {
        if (actions.setup_action && args.size() == 4 && args[0] == "--setup-action" &&
            args[2] == "--app-dir")
            return actions.setup_action(args[1], path_from_utf8(args[3]));
        if (args.size() == 4 && args[0] == "--write-manifest" && args[2] == "--version") {
            write_manifest(path_from_utf8(args[1]), args[3]);
        } else if (args.size() == 6 && args[0] == "--pack-archive" && args[2] == "--version" &&
                   args[4] == "--out") {
            pack_archive(path_from_utf8(args[1]), args[3], path_from_utf8(args[5]));
        } else if ((args.size() == 2 || (args.size() == 4 && args[2] == "--version")) &&
                   args[0] == "--verify-archive") {
            const auto doc =
                verify_archive(path_from_utf8(args[1]), args.size() == 4 ? args[3] : "");
            out("Verified " + doc.at("product").get<std::string>() + " " +
                doc.at("version").get<std::string>() + " (" +
                std::to_string(doc.at("files").size()) + " files).\n");
        } else {
            kLog.warning("The updater was started without a command it knows ({} arguments).",
                         args.size());
            err(usage(actions));
            return 2;
        }
    } catch (const std::exception& error) {
        return failed(error);
    }
    return 0;
}
} // namespace fv::update_native
