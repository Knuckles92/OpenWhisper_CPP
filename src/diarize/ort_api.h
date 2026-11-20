#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <utility>

// The C API's own types, from onnxruntime_c_api.h, which only .cpp files include.
struct OrtApi;
struct OrtEnv;

namespace fv::diarize {

// The ONNX Runtime API version FioriVoce asks for: the vendored header's (ONNX Runtime 1.29).
// A runtime that does not provide it is refused.
inline constexpr unsigned kOrtApiVersion = 29;

// ONNX Runtime loaded at run time by its full path, never linked: the user may have none, and
// recent Windows has an older onnxruntime.dll in System32 that a search by name would find. The
// library's dependencies are searched in its own folder and System32 only. A loaded runtime is
// never unloaded and its environment never released, as with the NeMo runtime.
// Affinity: thread-safe; the API is ONNX Runtime's (sessions run from any thread).
class OrtRuntime {
public:
    // The runtime at `library`, loaded on the first call for that path; later calls return the
    // same runtime. The error is a sentence to show: the file is missing, it cannot load, it is
    // not ONNX Runtime, it is older than kOrtApiVersion, or its environment cannot start.
    static std::expected<OrtRuntime*, std::string> load(const std::filesystem::path& library);

    const OrtApi& api() const { return *api_; }
    // The process's environment of this runtime (warnings and worse go to its default logger).
    OrtEnv* env() const { return env_; }
    // The runtime's own version string, "1.29.0".
    const std::string& version() const { return version_; }
    const std::filesystem::path& path() const { return path_; }

    OrtRuntime(const OrtRuntime&) = delete;
    OrtRuntime& operator=(const OrtRuntime&) = delete;

private:
    OrtRuntime(const OrtApi* api, OrtEnv* env, std::string version, std::filesystem::path path)
        : api_(api), env_(env), version_(std::move(version)), path_(std::move(path)) {}

    const OrtApi* api_;
    OrtEnv* env_;
    std::string version_;
    std::filesystem::path path_;
};

} // namespace fv::diarize
