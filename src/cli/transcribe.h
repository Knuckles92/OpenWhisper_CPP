#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fv::cli {

// `fiorivoce-cli transcribe AUDIO.wav [OPTION]...`: args[0] is the program and args[1] the
// word "transcribe". Prints the text on stdout and the timings on stderr, and returns 0.
// Throws UsageError for a bad command line and fv::EngineError for a failed decode.
// `env_prefix` is identity().env_prefix: <prefix>MODEL and <prefix>RUNTIME_DIR stand in for the
// options left out.
int transcribe(const std::vector<std::string>& args, std::string_view env_prefix);

} // namespace fv::cli
