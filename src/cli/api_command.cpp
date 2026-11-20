// openwhisper-cpp-cli --api: the headless History API (services/agent_api/cli.py). No microphone,
// speech runtime or window; the database is read only.

#include "cli/api_command.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "agentapi/history_routes.h"
#include "backup/startup_gate.h"
#include "base/data_lease.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "cli/detail/console_interrupt.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::cli {
namespace {

constexpr int kDefaultPort = 8766;
constexpr std::string_view kProg = "openwhisper-cpp-cli --api";
// uvicorn's request handlers ran in a thread pool of their own; SQLite reads block.
constexpr int kWorkers = 4;
// How long requests in progress may finish once the user stopped the server.
constexpr std::chrono::milliseconds kStopGrace{5000};

std::string usage() {
    return "usage: " + std::string(kProg) + " [-h] [--database DATABASE] [--port PORT]\n";
}

std::string help() {
    const std::string product(identity().product);
    return usage() + "\nServe the read-only " + product +
           " History API on 127.0.0.1.\n"
           "\n"
           "options:\n"
           "  -h, --help           show this help message and exit\n"
           "  --database DATABASE  Existing database path; defaults to " +
           product +
           "'s data\n"
           "                       directory.\n"
           "  --port PORT          Local TCP port (default: 8766).\n";
}

// parser.error: the usage, then "<prog>: error: <message>", status 2.
int usage_error(const std::string& message) {
    std::fputs(usage().c_str(), stderr);
    std::fprintf(stderr, "%s: error: %s\n", std::string(kProg).c_str(), message.c_str());
    return 2;
}

// argparse's view of an argument: an option unless it is "-" alone, a negative number (the
// parser has no option that looks like one) or holds a space without naming an option.
bool looks_like_option(const std::string& arg) {
    if (arg.size() < 2 || arg.front() != '-') {
        return false;
    }
    const std::string_view rest = std::string_view(arg).substr(1);
    const auto digits = [](std::string_view text) {
        return !text.empty() &&
               std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    const std::size_t dot = rest.find('.');
    const bool negative_number = dot == std::string_view::npos ? digits(rest)
                                                               : (rest.substr(0, dot).empty() ||
                                                                  digits(rest.substr(0, dot))) &&
                                                                     digits(rest.substr(dot + 1));
    return !negative_number;
}

// The long option `name` abbreviates ("--d" is --database), with any "=value"; nullopt when it
// names none.
std::optional<std::string> long_option(std::string_view arg, std::optional<std::string>& value) {
    const std::size_t equals = arg.find('=');
    const std::string_view name = arg.substr(0, equals);
    if (name.size() < 3 || !name.starts_with("--")) {
        return std::nullopt;
    }
    for (std::string_view option : {"--help", "--database", "--port"}) {
        if (option.starts_with(name)) {
            if (equals != std::string_view::npos) {
                value = std::string(arg.substr(equals + 1));
            }
            return std::string(option);
        }
    }
    return std::nullopt;
}

struct Options {
    std::optional<std::string> database;
    std::int64_t port = kDefaultPort;
};

// os.path.normcase(os.path.realpath(path)).
std::string normalized(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path real = std::filesystem::weakly_canonical(path, error);
    if (error) {
        real = std::filesystem::absolute(path, error);
    }
#ifdef _WIN32
    return py::lower(path_to_utf8(real.make_preferred()));
#else
    return path_to_utf8(real);
#endif
}

std::optional<std::string> env_or_legacy(std::string_view suffix) {
    const EnvReader& env = system_env();
    if (auto value = env.get(std::string(identity().env_prefix) + std::string(suffix));
        value && !value->empty()) {
        return value;
    }
    if (auto value = env.get(std::string(legacy_identity().env_prefix) + std::string(suffix));
        value && !value->empty()) {
        return value;
    }
    return std::nullopt;
}

int serve(agentapi::HistoryApi& api, int port) {
    detail::prepare_interrupt();
    ThreadPool workers("history-api", kWorkers);
    net::Reactor reactor(2);
    std::unique_ptr<net::HttpServer> server = net::HttpServer::create(reactor);
    server->set_fallback([&api, &workers](const net::HttpRequest& request, bool,
                                          std::shared_ptr<net::Responder> responder) {
        // The request is valid only while this runs on the reactor; the History API blocks.
        auto copy = std::make_shared<net::HttpRequest>(request);
        workers.post([&api, copy, responder = std::move(responder)] {
            try {
                agentapi::ApiResponse answer = api.handle(*copy);
                responder->send(answer.status, std::move(answer.headers), std::move(answer.body));
            } catch (const std::exception&) {
                responder->send(500, {{"content-type", "text/plain; charset=utf-8"}},
                                "Internal Server Error");
            }
        });
    });
    const auto listening =
        server->listen({"127.0.0.1", static_cast<std::uint16_t>(port), std::nullopt, true});
    if (!listening) {
        std::fprintf(stderr, "History API: %s\n", listening.error().c_str());
        return 1;
    }
    std::fprintf(stderr, "History API: serving on http://127.0.0.1:%d (press Ctrl+C to quit)\n",
                 static_cast<int>(listening->port));
    std::fflush(stderr);
    detail::wait_for_interrupt();
    server->stop(kStopGrace);
    server.reset();
    reactor.stop(std::chrono::milliseconds(0));
    return 0;
}

} // namespace

int run_api(std::span<const std::string> args) {
    Options options;
    bool positional_only = false;
    std::vector<std::string> unrecognized;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (!positional_only && arg == "--") {
            positional_only = true;
            continue;
        }
        if (positional_only || !looks_like_option(arg)) {
            unrecognized.push_back(arg);
            continue;
        }
        if (arg == "-h") {
            std::fputs(help().c_str(), stdout);
            return 0;
        }
        std::optional<std::string> inline_value;
        const std::optional<std::string> option = long_option(arg, inline_value);
        if (!option) {
            unrecognized.push_back(arg);
            continue;
        }
        if (*option == "--help") {
            if (inline_value) {
                return usage_error("argument -h/--help: ignored explicit argument '" +
                                   *inline_value + "'");
            }
            std::fputs(help().c_str(), stdout);
            return 0;
        }
        const std::string metavar = *option == "--port" ? "PORT" : "DATABASE";
        std::string value;
        if (inline_value) {
            value = *inline_value;
        } else if (i + 1 < args.size() && !looks_like_option(args[i + 1])) {
            value = args[++i];
        } else {
            return usage_error("argument " + *option + ": expected one argument");
        }
        if (*option == "--database") {
            options.database = value;
        } else {
            const std::optional<std::int64_t> port = py::py_int(value);
            if (!port) {
                return usage_error("argument --port: invalid int value: " + py::repr(value));
            }
            options.port = *port;
        }
    }
    if (!unrecognized.empty()) {
        std::string joined;
        for (const std::string& arg : unrecognized) {
            joined += (joined.empty() ? "" : " ") + arg;
        }
        return usage_error("unrecognized arguments: " + joined);
    }
    if (options.port < 1 || options.port > 65535) {
        return usage_error("--port must be between 1 and 65535");
    }
    const AppIdentity& id = identity();
    const std::optional<std::string> token = env_or_legacy("API_TOKEN");
    if (!token) {
        return usage_error("Set " + std::string(id.env_prefix) +
                           "API_TOKEN to a randomly generated token of at least 32 characters.");
    }

    const Paths paths = Paths::resolve(id, system_env());
    const std::filesystem::path primary = paths.user_file(id.database_file);
    // `args.database or config.DATABASE_FILE`: an empty --database names the primary one too.
    const bool named = options.database && !options.database->empty();
    const std::filesystem::path database = named ? path_from_utf8(*options.database) : primary;
    // The default database belongs to the data root a restore replaces at startup: its shared
    // lease is held while it is served, so a pending restore cannot replace it under this server.
    const bool use_primary_data = normalized(database) == normalized(primary);
    DataLease lease;
    if (use_primary_data) {
        try {
            lease = backup::acquire_startup_data_lease(paths.data_root(), id);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "History API: Could not safely open application data: %s\n",
                         error.what());
            return 1;
        }
    }
    int code = 1;
    {
        std::unique_ptr<agentapi::HistoryApi> api;
        try {
            api = std::make_unique<agentapi::HistoryApi>(database, *token);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "History API: %s\n", error.what());
        } catch (const py::ValueError& error) {
            std::fprintf(stderr, "History API: %s\n", error.what());
        } catch (const std::exception&) {
            // SQLite's errors carry paths and queries.
            std::fprintf(stderr,
                         "History API: Could not open the existing database. Open %s first and "
                         "verify --database.\n",
                         std::string(id.product).c_str());
        }
        if (api) {
            code = serve(*api, static_cast<int>(options.port));
        }
    }
    if (use_primary_data) {
        backup::release_startup_data_lease();
        lease.release();
    }
    return code;
}

} // namespace fv::cli
