#include "models/gpu_probe.h"

#include <cstring>
#include <format>
#include <mutex>

#include "base/log.h"
#include "models/detail/native_library.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::models {

namespace {

constexpr log::Logger kLog("services.gpu_info");

// nvmlDeviceGetName's buffer, as gpu_info.py sizes it (NVML_DEVICE_NAME_V2_BUFFER_SIZE).
constexpr unsigned int kNameBufferBytes = 96;

template <class F> F bind(const detail::NativeLibrary& library, const char* name) {
    return reinterpret_cast<F>(library.symbol(library.handle.get(), name));
}

template <class F>
F bind_preferring(const detail::NativeLibrary& library, const char* v2, const char* v1) {
    F function = bind<F>(library, v2);
    return function != nullptr ? function : bind<F>(library, v1);
}

} // namespace

std::string NvidiaGpu::short_name() const {
    std::string_view rest = name;
    for (const std::string_view prefix : {"NVIDIA ", "GeForce ", "Quadro "}) {
        if (rest.starts_with(prefix)) {
            rest.remove_prefix(prefix.size());
        }
    }
    return rest.empty() ? name : std::string(rest);
}

std::string format_gb(std::int64_t mib) {
    return std::format("{:.1f} GB", static_cast<double>(mib) / 1024);
}

std::vector<std::filesystem::path> nvml_candidates() {
    return detail::nvml_candidate_paths();
}

std::optional<NvmlApi> load_nvml() {
    for (const auto& candidate : nvml_candidates()) {
        auto library = detail::open_native_library(candidate);
        if (!library) {
            continue;
        }
        NvmlApi api;
        api.init = bind_preferring<NvmlApi::Init>(*library, "nvmlInit_v2", "nvmlInit");
        api.shutdown = bind<NvmlApi::Shutdown>(*library, "nvmlShutdown");
        api.get_count = bind_preferring<NvmlApi::GetCount>(*library, "nvmlDeviceGetCount_v2",
                                                           "nvmlDeviceGetCount");
        api.get_handle = bind_preferring<NvmlApi::GetHandle>(
            *library, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
        api.get_name = bind<NvmlApi::GetName>(*library, "nvmlDeviceGetName");
        api.get_memory = bind<NvmlApi::GetMemory>(*library, "nvmlDeviceGetMemoryInfo");
        api.get_compute_capability =
            bind<NvmlApi::GetCapability>(*library, "nvmlDeviceGetCudaComputeCapability");
        api.keep_loaded = library->handle;
        // ctypes resolves the attribute at its first call; a library missing a required one
        // fails the query there, which the probe answers as "no GPU".
        if (api.init == nullptr || api.get_count == nullptr || api.get_handle == nullptr ||
            api.get_name == nullptr || api.get_memory == nullptr) {
            return std::nullopt;
        }
        return api;
    }
    return std::nullopt;
}

std::optional<NvidiaGpu> query_nvml(const NvmlApi& api) {
    if (api.init == nullptr || api.init() != 0) {
        return std::nullopt;
    }
    struct Shutdown {
        const NvmlApi& api;
        ~Shutdown() {
            if (api.shutdown != nullptr) {
                api.shutdown();
            }
        }
    } shutdown{api};

    unsigned int count = 0;
    if (api.get_count(&count) != 0 || count == 0) {
        return std::nullopt;
    }
    void* handle = nullptr;
    if (api.get_handle(0, &handle) != 0) {
        return std::nullopt;
    }
    char name[kNameBufferBytes] = {};
    if (api.get_name(handle, name, kNameBufferBytes) != 0) {
        return std::nullopt;
    }
    NvmlApi::Memory memory{};
    if (api.get_memory(handle, &memory) != 0) {
        return std::nullopt;
    }
    std::optional<std::pair<int, int>> capability;
    int major = 0;
    int minor = 0;
    if (api.get_compute_capability != nullptr &&
        api.get_compute_capability(handle, &major, &minor) == 0) {
        capability = std::pair{major, minor};
    }
    const std::string_view raw(name, strnlen(name, kNameBufferBytes));
    NvidiaGpu gpu;
    gpu.name = py::strip(py::to_utf8(py::to_code_points(raw)));
    gpu.total_mib = static_cast<std::int64_t>(memory.total / (1024ULL * 1024ULL));
    gpu.compute_capability = capability;
    return gpu;
}

struct GpuProbe::State {
    Loader loader;
    std::mutex mutex;
    bool asked = false;
    std::optional<NvidiaGpu> answer;
};

GpuProbe::GpuProbe(Loader loader) : state_(std::make_shared<State>()) {
    state_->loader = std::move(loader);
}

std::optional<NvidiaGpu> GpuProbe::gpu() {
    std::lock_guard lock(state_->mutex);
    if (!state_->asked) {
        state_->asked = true;
        try {
            const auto api = state_->loader ? state_->loader() : load_nvml();
            state_->answer = api ? query_nvml(*api) : std::nullopt;
        } catch (const std::exception& error) {
            kLog.debug("NVML query failed: {}", error.what());
            state_->answer = std::nullopt;
        }
    }
    return state_->answer;
}

std::optional<NvidiaGpu> nvidia_gpu() {
    static GpuProbe probe;
    return probe.gpu();
}

} // namespace fv::models
