#include "models/onnx_runtime.h"

#include <string>
#include <system_error>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "models/data/onnxruntime_catalog.json.h"
#include "pyport/text.h"

namespace fv::models {

namespace fs = std::filesystem;

namespace {

constexpr log::Logger kLog("services.components");

bool is_regular(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

} // namespace

const Json& onnxruntime_catalog() {
    static const Json catalog = [] {
        const Json data = Json::parse(embedded::models_onnxruntime_catalog);
        Json built = Json::object();
        built["schema"] = 2;
        built["components"] = data.at("components");
        return built;
    }();
    return catalog;
}

std::optional<fs::path> locate_onnxruntime(const EnvReader& env, const ComponentStore& store) {
    const std::string variable = std::string(identity().env_prefix) + "ONNXRUNTIME";
    const std::string raw = py::strip(env.get(variable).value_or(std::string()));
    if (!raw.empty()) {
        const fs::path given = fv::base_utf8::path_from_utf8(raw);
        if (is_regular(given)) {
            std::error_code error;
            const fs::path absolute = fs::absolute(given, error);
            return error ? given : absolute.lexically_normal();
        }
        kLog.warning("{} is set but no file was found at {}", variable, raw);
    }
    if (!store.is_installed(kOnnxRuntimeComponent)) {
        return std::nullopt;
    }
    const fs::path root = store.component_dir(kOnnxRuntimeComponent);
    const fs::path library = fv::base_utf8::path_from_utf8(kOnnxRuntimeLibrary);
    for (const fs::path& candidate : {root / "lib" / library, root / library}) {
        if (is_regular(candidate)) {
            return candidate;
        }
    }
    kLog.warning("The {} component has no {}", kOnnxRuntimeComponent, kOnnxRuntimeLibrary);
    return std::nullopt;
}

} // namespace fv::models
