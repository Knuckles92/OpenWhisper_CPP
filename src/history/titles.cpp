#include "history/titles.h"

#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv {

std::optional<std::string> title_error(std::string_view value) {
    if (py::strip(value).empty()) {
        return "invalid_text";
    }
    if (py::len(value) > MAX_TITLE_LENGTH) {
        return "text_too_long";
    }
    for (const char32_t cp : py::to_code_points(value)) {
        if (cp < 32 || cp == 127) {
            return "invalid_text";
        }
    }
    return std::nullopt;
}

std::optional<std::string> title_error(const Json& value) {
    if (!value.is_string()) {
        return "invalid_text";
    }
    return title_error(std::string_view(value.get_ref<const std::string&>()));
}

std::string normalize_title(std::string_view value) {
    if (title_error(value)) {
        throw py::ValueError(std::string(TITLE_ERROR_MESSAGE));
    }
    return py::strip(value);
}

std::string normalize_title(const Json& value) {
    if (!value.is_string()) {
        throw py::ValueError(std::string(TITLE_ERROR_MESSAGE));
    }
    return normalize_title(std::string_view(value.get_ref<const std::string&>()));
}

} // namespace fv
