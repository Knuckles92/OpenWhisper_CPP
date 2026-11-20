#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/env.h"
#include "base/json.h"
#include "bench/cleanup_levels.h"
#include "bench/engine.h"
#include "bench/live_preview.h"
#include "bench/meeting_run.h"
#include "process/process_env.h"

// fiorivoce-bench's commands, here rather than in the executable so their exit texts are
// testable:
//
//   live-preview <manifest> [--output P] [--chunk S] [--overlap S] [--repeats N] [--limit N]
//   live-preview-report <report.json>... --output P   (writes P.json and P.md)
//   cleanup-levels [--provider P] [--model M] [--reasoning R] [--level L]... [--case ID]...
//                  [--delay S] [--json P]
//   meeting [--data-dir D] [--results-dir D] [--language L] [--meetings A,B] [--max-meetings N]
//           [--target-sec S] [--max-sec S] [--offline-pass | --no-offline-pass] [--force]
//   asr <audio>... [--reference TXT]... [--runs N] [--output P]
//   asr-corpus <manifest> [--output P]
//
// The decoding commands also take --runtime DIR, --model GGUF (else FIORIVOCE_TEST_RUNTIME_DIR
// and FIORIVOCE_TEST_MODEL) and --device cpu|cuda. Exit codes: 0 done, 1 a measured run failed,
// 2 a usage error, a missing engine or provider, or no matching case.
namespace fv::bench {

// A bad command line (argparse's parser.error): the message, printed after the usage line.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A cleaner the cleanup-levels command can run, as TranscriptCleanup answers for it.
struct CleanupClientHandle {
    Cleaner cleaner;
    std::function<bool()> is_available;
    std::string provider;
    std::string model;
    std::string reasoning;
    // Keeps whatever the handle's functions use alive.
    std::shared_ptr<void> owner;
};

struct CommandContext {
    std::ostream& out;
    std::ostream& err;
    const EnvReader& env;
    MutableEnv& mutable_env;
    // The app's saved settings (cleanup provider, model and thinking level).
    std::function<Json()> load_settings;
    // A TranscriptCleanup for (provider, model, reasoning).
    std::function<CleanupClientHandle(const std::string&, const std::string&, const std::string&)>
        make_cleaner;
    // The engine a decoding command runs on; load_parakeet when empty.
    std::function<std::unique_ptr<BenchEngine>(const EngineFiles&)> load_engine;
    // The checkout whose source the provenance hashes and whose HEAD it names.
    std::optional<std::filesystem::path> source_root;
};

// The command lines of live-preview and meeting, validated as argparse would (parse_args);
// UsageError for a bad one. `args` follows the command name.
LivePreviewArgs parse_live_preview_args(const std::vector<std::string>& args);
MeetingArgs parse_meeting_args(const std::vector<std::string>& args);

// Runs `fiorivoce-bench <args>` (argv without the program name) and returns the exit code.
// Never throws: an error is printed to err as one line.
int run_command(const std::vector<std::string>& args, CommandContext& context);

} // namespace fv::bench
