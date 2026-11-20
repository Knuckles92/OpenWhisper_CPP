#pragma once

#include <span>
#include <string>

namespace fv::cli {

// `--api [--database PATH] [--port N]` (services/agent_api/cli.py main): serves the read-only
// History API on 127.0.0.1 without the app, until Ctrl+C or Ctrl+Break, then returns 0. `args`
// is what follows the flag, parsed as argparse parses it: -h prints the help and returns 0; a bad
// command line, a port outside 1 to 65535 or no <env_prefix>API_TOKEN (the legacy prefix's
// variable is read when it is unset or empty) prints the usage and the error and returns 2. The
// default database is the data folder's, whose startup lease is held while it is served (paths
// compared after normalizing case and resolving links); a failure to start prints
// "History API: <message>" and returns 1.
int run_api(std::span<const std::string> args);

} // namespace fv::cli
