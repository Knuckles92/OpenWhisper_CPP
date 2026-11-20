#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "update/check.h"

namespace fv {

// What fiorivoce.exe was asked to do before anything loads Qt's application object, the data
// lease or the NeMo runtime (main.py's _handle_early_cli).
enum class EarlyCommand {
    Gui,         // no recognised option: open the app
    Help,        // -h or --help first: print the usage
    Version,     // --version alone
    Diagnostics, // --diagnostics first: the rest is its command line
    SelfTestGui, // --self-test-gui alone
};

struct EarlyCli {
    EarlyCommand command = EarlyCommand::Gui;
    // The arguments after the option (--diagnostics only).
    std::vector<std::string> rest;
};

// Dispatch on the first argument only (A01-68): -h and --help answer whatever follows,
// --diagnostics takes the rest of the line, and --version and --self-test-gui count only when
// they are the whole command line. Anything else, also "--version extra", opens the app.
// `args` is argv as UTF-8, args[0] the program.
EarlyCli parseEarlyCli(std::span<const std::string> args);

// What --help prints: main.py's _USAGE with the options this executable has. `executable` is
// the program's name ("fiorivoce"), `product` identity().product, `envPrefix`
// identity().env_prefix.
std::string guiUsageText(std::string_view executable, std::string_view product,
                         std::string_view envPrefix);

// Runs an early command and returns the exit code; Gui is not one (std::logic_error). Output
// goes to the console the executable was started from, when it has one. Help and Version
// touch nothing on disk; Diagnostics reads the settings and writes the report
// (services/diagnostics.py main), with `program` (argv[0]'s file name) in its usage line as
// argparse prints it.
int runEarlyCommand(const EarlyCli& cli, std::string_view program, int argc, char** argv);

// --diagnostics OUTPUT.json: a bad command line prints argparse's usage and error on stderr and
// returns 2, an existing file returns 1 with a sentence, a written report 0. The report's
// "packaged" (OpenWhisper's sys.frozen, true for every installed build) is whether this copy runs
// from the folder its install registration names (update::detect_channel is Installer): a build
// tree or an unregistered copy is not packaged. The first form probes this process's install.
int runDiagnostics(std::span<const std::string> args, std::string_view program);
int runDiagnostics(std::span<const std::string> args, std::string_view program,
                   const update::InstallProbe& install);

// A windowed executable has no console of its own: when its standard output is not redirected,
// write to the console of the process that started it (AttachConsole(ATTACH_PARENT_PROCESS)).
// Does nothing elsewhere or when there is no such console.
void attachParentConsole();

// Whether this process can write log lines to a console: its standard output goes somewhere
// (a console, a pipe, a file). A windowed executable started from Explorer has none, which is
// when Python's windowed build had no sys.stdout and logged to the file alone.
bool hasConsoleOutput();

} // namespace fv
