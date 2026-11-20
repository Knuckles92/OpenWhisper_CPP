#include "models/pdfium_runtime.h"

#include <string>
#include <system_error>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "models/data/pdfium_catalog.json.h"
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

const Json& pdfium_catalog() {
    static const Json catalog = [] {
        const Json data = Json::parse(embedded::models_pdfium_catalog);
        Json built = Json::object();
        built["schema"] = 2;
        built["components"] = data.at("components");
        return built;
    }();
    return catalog;
}

std::optional<fs::path> locate_pdfium(const EnvReader& env, const ComponentStore& store) {
    const std::string variable = std::string(identity().env_prefix) + "PDFIUM";
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
    if (!store.is_installed(kPdfiumComponent)) {
        return std::nullopt;
    }
    const fs::path root = store.component_dir(kPdfiumComponent);
    const fs::path library = fv::base_utf8::path_from_utf8(kPdfiumLibrary);
    for (const fs::path& candidate :
         {root / "bin" / library, root / "lib" / library, root / library}) {
        if (is_regular(candidate)) {
            return candidate;
        }
    }
    kLog.warning("The {} component has no {}", kPdfiumComponent, kPdfiumLibrary);
    return std::nullopt;
}

} // namespace fv::models
