#include "engine/nemo_api.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "engine/error.h"
#include "engine/platform.h"

namespace fv {

namespace {

std::unique_ptr<NemoApi> load(const std::filesystem::path& library_path) {
    auto api = std::make_unique<NemoApi>();
    // services/local_asr/nvidia.py adds the runtime's bin folder before loading it: ggml loads
    // its backends (ggml-cpu, ggml-cuda) by name once the runtime runs.
    add_dll_directory(library_path.parent_path());
    api->library = SharedLibrary(library_path);

#define FV_BIND(name)                                                                              \
    api->name =                                                                                    \
        reinterpret_cast<decltype(api->name)>(api->library.symbol("nemo_speech_asr_" #name));      \
    if (api->name == nullptr) {                                                                    \
        throw EngineError("The speech runtime at " + path_to_utf8(api->library.path()) +           \
                          " is missing nemo_speech_asr_" #name ".");                               \
    }
    FV_NEMO_ASR_FUNCTIONS(FV_BIND)
#undef FV_BIND

    return api;
}

} // namespace

const NemoApi& NemoApi::get(const std::filesystem::path& library_path) {
    // Never destroyed, so no runtime is unloaded during static destruction
    // either (see the header for why).
    static std::mutex mutex;
    static auto& loaded = *new std::map<std::filesystem::path, std::unique_ptr<NemoApi>>();

    const std::filesystem::path key = std::filesystem::absolute(library_path).lexically_normal();
    const std::lock_guard lock(mutex);
    std::unique_ptr<NemoApi>& api = loaded[key];
    if (api == nullptr) {
        try {
            api = load(key);
        } catch (...) {
            loaded.erase(key);
            throw;
        }
    }
    return *api;
}

} // namespace fv
