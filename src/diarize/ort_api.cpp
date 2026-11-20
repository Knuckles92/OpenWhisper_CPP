#include "diarize/ort_api.h"

#include <map>
#include <memory>
#include <mutex>
#include <system_error>

#include "base/utf8.h"
#include "diarize/detail/ort_loader.h"

#include "onnxruntime_c_api.h"

namespace fv::diarize {

static_assert(kOrtApiVersion == ORT_API_VERSION,
              "kOrtApiVersion names the vendored onnxruntime_c_api.h's version");

namespace {

using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();

struct Registry {
    std::mutex mutex;
    // Runtimes live until the process ends (never unloaded), so the pointers handed out stay
    // valid; keyed by the absolute path.
    std::map<std::filesystem::path, std::unique_ptr<OrtRuntime>> runtimes;
};

Registry& registry() {
    // Leaked on purpose: runtimes must outlive every static that might still run a session.
    static auto* instance = new Registry();
    return *instance;
}

} // namespace

std::expected<OrtRuntime*, std::string> OrtRuntime::load(const std::filesystem::path& library) {
    if (library.empty()) {
        return std::unexpected(std::string("No ONNX Runtime library is set."));
    }
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(library, error);
    if (error) {
        absolute = library;
    }
    absolute.make_preferred();

    Registry& runtimes = registry();
    std::lock_guard lock(runtimes.mutex);
    if (const auto found = runtimes.runtimes.find(absolute); found != runtimes.runtimes.end()) {
        return found->second.get();
    }
    if (!std::filesystem::is_regular_file(absolute, error)) {
        return std::unexpected("ONNX Runtime was not found at " + path_to_utf8(absolute) + ".");
    }

    const auto symbol = detail::load_ort_get_api_base(absolute);
    if (!symbol) {
        return std::unexpected(symbol.error());
    }
    const OrtApiBase* base = reinterpret_cast<GetApiBase>(*symbol)();
    if (base == nullptr) {
        return std::unexpected(path_to_utf8(absolute) + " is not ONNX Runtime: it has no API.");
    }
    const char* version_text = base->GetVersionString != nullptr ? base->GetVersionString() : "";
    std::string version = version_text != nullptr ? version_text : "";
    const OrtApi* api = base->GetApi(kOrtApiVersion);
    if (api == nullptr) {
        return std::unexpected("ONNX Runtime " + (version.empty() ? "" : version + " ") + "at " +
                               path_to_utf8(absolute) +
                               " is too old: version 1.29 or later is needed.");
    }

    OrtEnv* env = nullptr;
    if (OrtStatus* status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "speaker-id", &env)) {
        std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        return std::unexpected("ONNX Runtime could not start: " + message + ".");
    }
    auto runtime =
        std::unique_ptr<OrtRuntime>(new OrtRuntime(api, env, std::move(version), absolute));
    OrtRuntime* pointer = runtime.get();
    runtimes.runtimes.emplace(std::move(absolute), std::move(runtime));
    return pointer;
}

} // namespace fv::diarize
