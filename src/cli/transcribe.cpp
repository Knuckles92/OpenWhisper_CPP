// The engine smoke path: transcribes a WAV file, for checking a runtime and model and for
// comparing against OpenWhisper. Includes the engine's headers only, so it also builds in the
// engine-only configuration.

#include "cli/transcribe.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/usage.h"
#include "engine/error.h"
#include "engine/platform.h"
#include "engine/recognizer.h"
#include "engine/wav.h"

namespace fv::cli {
namespace {

struct Arguments {
    std::string audio;
    std::optional<std::string> model;
    std::optional<std::string> runtime;
    int gpu = fv::kCpuDevice;
    std::string language;
    bool timestamps = false;
};

Arguments parse_arguments(const std::vector<std::string>& args, std::string_view env_prefix) {
    const std::string model_variable = std::string(env_prefix) + "MODEL";
    const std::string runtime_variable = std::string(env_prefix) + "RUNTIME_DIR";
    Arguments parsed;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw UsageError(arg + " needs a value.");
            }
            return args[++i];
        };
        if (arg == "--model") {
            parsed.model = value();
        } else if (arg == "--runtime") {
            parsed.runtime = value();
        } else if (arg == "--gpu") {
            const std::string& text = value();
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), parsed.gpu);
            if (error != std::errc() || end != text.data() + text.size() || parsed.gpu < 0) {
                throw UsageError("--gpu needs a number, 0 or higher.");
            }
        } else if (arg == "--language") {
            parsed.language = value();
        } else if (arg == "--timestamps") {
            parsed.timestamps = true;
        } else if (arg.starts_with("--")) {
            throw UsageError("Unknown option " + arg + ".");
        } else if (parsed.audio.empty()) {
            parsed.audio = arg;
        } else {
            throw UsageError("Only one audio file can be given.");
        }
    }
    if (parsed.audio.empty()) {
        throw UsageError("No audio file given.");
    }
    if (!parsed.model) {
        parsed.model = fv::env_utf8(model_variable.c_str());
    }
    if (!parsed.runtime) {
        parsed.runtime = fv::env_utf8(runtime_variable.c_str());
    }
    if (!parsed.model) {
        throw UsageError("No model given. Pass --model or set " + model_variable + ".");
    }
    if (!parsed.runtime) {
        throw UsageError("No speech runtime given. Pass --runtime or set " + runtime_variable +
                         ".");
    }
    return parsed;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void print_transcript(const fv::Transcript& transcript, bool timestamps) {
    if (!timestamps) {
        std::printf("%s\n", transcript.text.c_str());
        return;
    }
    for (const fv::Segment& segment : transcript.segments) {
        std::printf("[%7.2f - %7.2f] %s\n", segment.start, segment.end, segment.text.c_str());
    }
}

} // namespace

int transcribe(const std::vector<std::string>& args, std::string_view env_prefix) {
    const Arguments parsed = parse_arguments(args, env_prefix);
    const fv::AudioBuffer audio = fv::read_wav(fv::path_from_utf8(parsed.audio));

    fv::RecognizerConfig config;
    config.runtime_dir = fv::path_from_utf8(*parsed.runtime);
    config.model_path = fv::path_from_utf8(*parsed.model);
    config.gpu = parsed.gpu;

    auto started = std::chrono::steady_clock::now();
    fv::Recognizer recognizer(config);
    const double load_seconds = seconds_since(started);

    started = std::chrono::steady_clock::now();
    print_transcript(recognizer.transcribe(audio.samples, audio.sample_rate, {parsed.language}),
                     parsed.timestamps);
    const double decode_seconds = seconds_since(started);
    std::fflush(stdout);

    std::fprintf(
        stderr, "runtime %s on %s | audio %.1f s | load %.2f s | decode %.2f s (%.0fx real time)\n",
        recognizer.runtime_version().c_str(),
        parsed.gpu == fv::kCpuDevice ? "CPU" : ("GPU " + std::to_string(parsed.gpu)).c_str(),
        audio.duration(), load_seconds, decode_seconds,
        decode_seconds > 0.0 ? audio.duration() / decode_seconds : 0.0);
    return 0;
}

} // namespace fv::cli
