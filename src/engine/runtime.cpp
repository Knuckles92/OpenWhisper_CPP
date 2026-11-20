#include "engine/runtime.h"

#include <string>
#include <string_view>
#include <system_error>

#include "engine/error.h"
#include "engine/platform.h"

namespace fv {

const char* runtime_library_name() {
#if defined(_WIN32)
    return "nemo_speech_asr_c.dll";
#elif defined(__APPLE__)
    return "libnemo_speech_asr_c.dylib";
#else
    return "libnemo_speech_asr_c.so";
#endif
}

std::filesystem::path find_runtime_library(const std::filesystem::path& runtime_dir) {
    static constexpr const char* kFolders[] = {"bin", "lib", "nemo-speech/lib", "nemo-speech/bin",
                                               ""};
    for (const char* folder : kFolders) {
        const std::filesystem::path candidate = runtime_dir / folder / runtime_library_name();
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    throw EngineError("No speech runtime in " + path_to_utf8(runtime_dir) + ": " +
                      runtime_library_name() + " was not found.");
}

bool runtime_has_gpu_backend(const std::filesystem::path& runtime_dir) {
    std::filesystem::path folder;
    try {
        folder = find_runtime_library(runtime_dir).parent_path();
    } catch (const EngineError&) {
        return false;
    }
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = path_to_utf8(entry.path().filename());
        for (const std::string_view gpu :
             {"ggml-cuda", "libggml-cuda", "ggml-vulkan", "libggml-vulkan", "libggml-metal"}) {
            if (name.starts_with(gpu)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace fv
