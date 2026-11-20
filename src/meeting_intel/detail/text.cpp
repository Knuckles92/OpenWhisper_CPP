#include "meeting_intel/detail/text.h"

#include <algorithm>
#include <cmath>
#include <typeinfo>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace fv::meeting_intel::detail {

bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    return !value.empty();
}

std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    return value.dump();
}

std::string text_or_empty(const Json& value) {
    return truthy(value) ? py_str(value) : std::string();
}

std::string sanitize(std::string_view text) {
    static const py::Regex evidence_id(R"(\bsg_[0-9a-fA-F]{6,}\b)");
    return evidence_id.sub("[id]", text);
}

std::string clip(std::string_view text, std::size_t max_chars) {
    std::string cleaned = py::strip(sanitize(text));
    if (py::len(cleaned) <= max_chars) {
        return cleaned;
    }
    const auto keep = static_cast<std::int64_t>(max_chars) - 1;
    return py::rstrip(py::slice(cleaned, 0, keep)) + "…";
}

std::string type_name(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    const char* raw = typeid(error).name();
#if defined(__GNUG__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    std::string name = status == 0 && demangled != nullptr ? demangled : raw;
    std::free(demangled);
    return name;
#else
    std::string_view name = raw;
    for (std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.remove_prefix(prefix.size());
        }
    }
    return std::string(name);
#endif
}

std::int64_t clamp_limit(const Json& limit, std::int64_t fallback, std::int64_t max_limit) {
    const auto value = py::py_int_checked(limit);
    if (value) {
        return std::clamp<std::int64_t>(*value, 1, max_limit);
    }
    if (value.error() != py::IntError::Overflow) {
        return fallback;
    }
    // Python's int is unbounded: a finite value beyond int64 still clamps by its sign.
    if (limit.is_number_float() && std::isinf(limit.get<double>())) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    if (limit.is_string()) {
        const std::string text = py::strip(limit.get<std::string>());
        return text.starts_with('-') ? 1 : max_limit;
    }
    if (limit.is_number_float()) {
        return limit.get<double>() < 0 ? 1 : max_limit;
    }
    return max_limit;
}

} // namespace fv::meeting_intel::detail
