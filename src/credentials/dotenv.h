#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/env.h"

namespace fv {

// The optional .env file (Paths::env_file()), parsed as python-dotenv 1.2.3's dotenv_values
// does: `export` prefixes, single and double quotes with their escapes, inline comments, and
// ${VAR} / ${VAR:-default} interpolation from the names above in the file, then the process
// environment. A name written without "=" has no value. A missing file, a folder, or a file that
// is not UTF-8 reads as empty. Re-read when the file's size or time changes.
// Affinity: thread-safe.
class DotEnv {
public:
    explicit DotEnv(std::filesystem::path file);
    // interpolation_env must outlive the DotEnv.
    DotEnv(std::filesystem::path file, const EnvReader& interpolation_env);

    std::optional<std::string> get(std::string_view name) const; // nullopt: absent or no value
    // As Python's dict: one entry per name, at its first line's position, with its last value.
    std::vector<std::pair<std::string, std::optional<std::string>>> values() const;

    const std::filesystem::path& file() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

// python-dotenv's parse and interpolation over decoded text, as python-dotenv parses a stream.
// DotEnv translates a file's line ends to "\n" first (Python's universal newlines); text passed
// here as it is keeps "\r\n" and "\r", which end lines too. Unparsable statements are skipped and
// logged by line number, as python-dotenv does.
std::vector<std::pair<std::string, std::optional<std::string>>>
parse_dotenv(std::string_view text, const EnvReader& interpolation_env);

} // namespace fv
