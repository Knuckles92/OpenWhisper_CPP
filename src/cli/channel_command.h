#pragma once

#include <span>
#include <string>

namespace fv::cli {

// `--channel` (services/automation/channel_bridge.py main): the Claude Code channel on this
// process's standard input and output, until Claude Code closes standard input; returns 0 then.
// `args` is what follows the flag: -h or --help first prints the usage to standard error and
// returns 0, anything else says "Unexpected arguments. Run --channel --help for usage." and
// returns 2. Without pipes on standard input and output it logs why and returns 2; an
// unexpected failure logs its type and returns 1. Logs go to standard error only.
int run_channel(std::span<const std::string> args);

// Whether a setup or update is about to replace the installed copy this executable runs from
// (channel_bridge.py update_in_progress): its update or setup mutex exists. Always false for a
// copy that is not the registered install, and off Windows.
bool update_in_progress();

} // namespace fv::cli
