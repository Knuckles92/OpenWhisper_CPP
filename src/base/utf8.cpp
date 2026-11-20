#include "base/utf8.h"

#include "base/env.h"

namespace fv {
inline namespace base_utf8 {

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::optional<std::string> env_utf8(std::string_view name) {
    auto value = system_env().get(name);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace base_utf8
} // namespace fv
