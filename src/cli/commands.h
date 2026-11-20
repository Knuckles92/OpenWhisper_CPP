#pragma once

#include <span>
#include <string>

#include "update/check.h"

namespace fv::cli {

// `--diagnostics OUTPUT.json` (services/diagnostics.py main). `args` is what follows the flag.
// Writes the sanitized support report to a new file and returns 0; a bad command line prints
// argparse's usage and error on stderr and returns 2, a file that exists returns 1. The report's
// "packaged" (OpenWhisper's sys.frozen, true for every installed build) is whether this copy runs
// from the folder its install registration names (update::detect_channel is Installer): a build
// tree or an unregistered copy is not packaged. The first form probes this process's install.
int run_diagnostics(std::span<const std::string> args);
int run_diagnostics(std::span<const std::string> args, const update::InstallProbe& install);

// `--self-test`: the Qt-free half of main.py's _run_package_self_test, ending, as it does, with
// the workflow smoke (whose report line it prints). Prints "<product> package self-test passed"
// and returns 0, or says what failed on stderr and returns 1. Starts no process.
int run_self_test();

} // namespace fv::cli
