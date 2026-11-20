#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/json.h"

// services/release_checks.py: the offline synthetic service lifecycle gate. It exercises real
// audio decoding, the live preview's windows on the speech service and their queued delivery,
// cancel and restart, and database persistence; only the speech model is a deterministic
// fixture. The numbers measure plumbing, never ASR quality or inference speed.
namespace fv::cli {

// What more than one argument after --workflow-smoke prints (main.py:208).
inline constexpr const char* kWorkflowSmokeUsage = "Usage: --workflow-smoke [report.json]";

// peak_rss_mb: the process's peak working set (GetProcessMemoryInfo's PeakWorkingSetSize) or,
// elsewhere, its maximum resident set, in MB of 1,000,000 bytes; nullopt when the system does
// not say.
std::optional<double> peak_rss_mb();

// run_workflow_smoke: `repeats` stop-to-result runs, a cancel while a decode is blocked, a
// restart that must deliver exactly one queued result, history kept across a database reopen,
// then the data-recovery checks (an interrupted recording recovered exactly once, a backup
// created and restored through the startup data lease, the replaced data kept). Everything
// happens in a new temporary folder ("<product>-smoke-*", removed afterwards), never in the data
// root. Prints the report on stdout as json.dumps(report, sort_keys=True) and returns it:
// {schema: 2, kind: "synthetic_service_lifecycle", repeats, metrics, checks, passed,
// limitations}. Throws std::invalid_argument("repeats must be between 1 and 1000"), and
// std::runtime_error with the sentence of the check that failed.
// Blocking; never on the GUI thread.
Json run_workflow_smoke(int repeats = 5);

// The `--workflow-smoke [report.json]` command; `args` is what follows the flag. More than one
// argument prints kWorkflowSmokeUsage on stderr and returns 1 (Python's SystemExit with a
// message). Otherwise runs the smoke, saves the report to the path when one is given (replacing
// a file there) and returns 0; a failed check returns 1 with the reason on stderr.
int run_workflow_smoke_command(std::span<const std::string> args);

// --- pieces of the smoke, public for the tests ---

// The report as `--workflow-smoke REPORT.json` saves it: json.dumps(report, indent=2) written in
// text mode, so lines end with CRLF on Windows; no final newline.
std::string report_file_text(const Json& report);

// statistics.median of the values (the mean of the two middle ones for an even count); the
// values must not be empty.
double median(std::vector<double> values);
// numpy.percentile with its default linear interpolation, `q` in [0, 100]; the values must not
// be empty.
double percentile(std::vector<double> values, double q);

// The bytes of the WAV file's data chunk (wave's readframes of every frame). Throws
// std::runtime_error when the file is not a RIFF WAVE file with a data chunk.
std::string wav_data(const std::filesystem::path& path);

// The fixture: 16000 samples of 1000 at 16 kHz mono, 16-bit (release_checks.py:117).
std::vector<std::int16_t> fixture_samples();

// Writes the fixture as Python's wave module does (through audio::WavWriter, no tags).
void write_fixture_wav(const std::filesystem::path& path, std::span<const std::int16_t> samples);

} // namespace fv::cli
