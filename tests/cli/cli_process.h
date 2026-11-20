#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fv::clitest {

struct CliResult {
    std::optional<std::uint32_t> exit_code; // nullopt: it did not finish in time and was killed
    std::string out;
    std::string err;
    // Every process the run started, itself included. nullopt where the platform cannot count.
    std::optional<std::uint32_t> processes;
};

// An environment variable to set (a value) or remove (nullopt) for one run.
using EnvOverride = std::pair<std::string, std::optional<std::string>>;

// Runs fiorivoce-cli without a window and waits for it. FIORIVOCE_RUNTIME_DIR, FIORIVOCE_MODEL
// and FIORIVOCE_LOG_LEVEL are removed unless `env` sets them, so a developer's own settings never
// reach the run. The calling process's environment is restored afterwards.
CliResult run_cli(const std::vector<std::string>& args, const std::vector<EnvOverride>& env,
                  std::chrono::milliseconds timeout = std::chrono::seconds(60));

std::filesystem::path cli_executable();
std::string read_text(const std::filesystem::path& path);
void write_text(const std::filesystem::path& path, const std::string& bytes);

} // namespace fv::clitest
