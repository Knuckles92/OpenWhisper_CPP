#include "settings/setting.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "pyport/number.h"
#include "pyport/text.h"

namespace fv {

namespace {

// int(raw) as setting_types.IntSetting takes it, where Python's int is unbounded: a value that
// passes int() but does not fit int64 is only known to lie above or below every bound.
struct PyInt {
    enum class State { Invalid, Value, Above, Below } state = State::Invalid;
    std::int64_t value = 0;
};

PyInt python_int(const Json& raw, bool strict) {
    if (strict) {
        // type(raw) is int: a JSON integer, never a bool.
        if (raw.is_number_unsigned()) {
            const auto value = raw.get<std::uint64_t>();
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return {PyInt::State::Above};
            }
            return {PyInt::State::Value, static_cast<std::int64_t>(value)};
        }
        if (raw.is_number_integer()) {
            return {PyInt::State::Value, raw.get<std::int64_t>()};
        }
        return {};
    }
    const auto converted = py::py_int_checked(raw);
    if (converted) {
        return {PyInt::State::Value, *converted};
    }
    if (converted.error() != py::IntError::Overflow) {
        return {};
    }
    // OverflowError proper (infinity) is Python's too; anything else Python holds.
    if (raw.is_number_float()) {
        const double value = raw.get<double>();
        if (!std::isfinite(value)) {
            return {};
        }
        return {value > 0 ? PyInt::State::Above : PyInt::State::Below};
    }
    if (raw.is_string()) {
        const std::string text = py::strip(raw.get_ref<const std::string&>());
        return {!text.empty() && text.front() == '-' ? PyInt::State::Below : PyInt::State::Above};
    }
    return {PyInt::State::Above}; // a JSON integer beyond int64
}

std::string normalized(const SettingSpec& spec, const std::string& text) {
    switch (spec.normalize) {
    case Normalize::None:
        return text;
    case Normalize::Strip:
        return py::strip(text);
    case Normalize::LowerStripped:
        return py::lower(py::strip(text));
    case Normalize::StripAndCut:
        return py::slice(py::strip(text), 0, static_cast<std::int64_t>(spec.cut));
    }
    return text;
}

std::optional<Json> convert_int(const SettingSpec& spec, const Json& raw) {
    const PyInt number = python_int(raw, spec.is_strict);
    switch (number.state) {
    case PyInt::State::Invalid:
        return std::nullopt;
    case PyInt::State::Value: {
        std::int64_t value = number.value;
        if ((spec.valid_low && value < *spec.valid_low) ||
            (spec.valid_high && value > *spec.valid_high)) {
            return std::nullopt;
        }
        if (spec.clamp_low) {
            value = std::max(*spec.clamp_low, value);
        }
        if (spec.clamp_high) {
            value = std::min(*spec.clamp_high, value);
        }
        return Json(value);
    }
    case PyInt::State::Above:
        if (spec.valid_high) {
            return std::nullopt;
        }
        if (spec.clamp_high) {
            return Json(*spec.clamp_high); // min(high, max(low, huge))
        }
        // Python would read the huge number itself, which int64 cannot hold.
        return std::nullopt;
    case PyInt::State::Below:
        if (spec.valid_low) {
            return std::nullopt;
        }
        if (spec.clamp_low) {
            return Json(spec.clamp_high ? std::min(*spec.clamp_low, *spec.clamp_high)
                                        : *spec.clamp_low);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Json> convert_int_choice(const SettingSpec& spec, const Json& raw) {
    const PyInt number = python_int(raw, spec.is_strict);
    if (number.state != PyInt::State::Value) {
        return std::nullopt; // a number beyond int64 is none of the choices
    }
    for (const std::int64_t choice : spec.int_choices) {
        if (choice == number.value) {
            return Json(number.value);
        }
    }
    return std::nullopt;
}

std::optional<Json> convert_choice(const SettingSpec& spec, const Json& raw) {
    if (!raw.is_string()) {
        return std::nullopt;
    }
    const auto& text = raw.get_ref<const std::string&>();
    for (const ChoiceAlias& alias : spec.aliases) {
        if (alias.from == text) {
            return Json(std::string(alias.to));
        }
    }
    std::string value = normalized(spec, text);
    for (const std::string_view choice : spec.choices) {
        if (choice == value) {
            return Json(std::move(value));
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view kind_name(SettingKind kind) {
    switch (kind) {
    case SettingKind::Bool:
        return "BoolSetting";
    case SettingKind::Int:
        return "IntSetting";
    case SettingKind::Float:
        return "FloatSetting";
    case SettingKind::Str:
        return "StrSetting";
    case SettingKind::Choice:
        return "ChoiceSetting";
    case SettingKind::IntChoice:
        return "IntChoiceSetting";
    case SettingKind::List:
        return "ListSetting";
    case SettingKind::Dict:
        return "DictSetting";
    case SettingKind::OptionalList:
        return "OptionalListSetting";
    case SettingKind::OptionalDict:
        return "OptionalDictSetting";
    case SettingKind::Raw:
        return "RawSetting";
    }
    return "Setting";
}

namespace detail {

void throw_bad_choice_default(std::string_view key, std::string_view value) {
    throw std::invalid_argument(std::string(key) + ": default " + py::repr(value) +
                                " is not one of its choices");
}

void throw_kind_mismatch(std::string_view key) {
    throw std::invalid_argument(std::string(key) + ": the spec's kind does not read as this type");
}

} // namespace detail

Json SettingBase::default_json() const {
    switch (spec_.kind) {
    case SettingKind::Bool:
        return Json(spec_.flag_default);
    case SettingKind::Int:
    case SettingKind::IntChoice:
        return Json(spec_.int_default);
    case SettingKind::Float:
        return Json(spec_.float_default);
    case SettingKind::Str:
    case SettingKind::Choice:
        return Json(std::string(spec_.text_default));
    case SettingKind::List:
    case SettingKind::Dict:
        return Json::parse(spec_.text_default);
    case SettingKind::OptionalList:
    case SettingKind::OptionalDict:
    case SettingKind::Raw:
        return Json(nullptr);
    }
    return Json(nullptr);
}

std::optional<Json> SettingBase::convert(const Json& raw) const {
    switch (spec_.kind) {
    case SettingKind::Bool:
        return raw.is_boolean() ? std::optional<Json>(raw) : std::nullopt;
    case SettingKind::Int:
        return convert_int(spec_, raw);
    case SettingKind::Float:
        // A JSON number, not a bool (nlohmann keeps booleans apart from numbers).
        if (!raw.is_number()) {
            return std::nullopt;
        }
        return Json(raw.get<double>());
    case SettingKind::Str:
        if (!raw.is_string()) {
            return std::nullopt;
        }
        return Json(normalized(spec_, raw.get_ref<const std::string&>()));
    case SettingKind::Choice:
        return convert_choice(spec_, raw);
    case SettingKind::IntChoice:
        return convert_int_choice(spec_, raw);
    case SettingKind::List:
        return raw.is_array() ? std::optional<Json>(raw) : std::nullopt;
    case SettingKind::Dict:
        return raw.is_object() ? std::optional<Json>(raw) : std::nullopt;
    case SettingKind::OptionalList:
        // A wrong type reads as an empty list, not as the default (setting_types.py:326-337).
        return std::optional<Json>(std::in_place, raw.is_array() ? raw : Json::array());
    case SettingKind::OptionalDict:
        return std::optional<Json>(std::in_place, raw.is_object() ? raw : Json::object());
    case SettingKind::Raw:
        return std::optional<Json>(std::in_place, raw);
    }
    return std::nullopt;
}

Json SettingBase::parse_json(const Json& raw) const {
    auto value = convert(raw);
    return value ? std::move(*value) : default_json();
}

Json SettingBase::read_json(const Json& snapshot) const {
    if (!snapshot.is_object()) {
        return default_json();
    }
    const auto found = snapshot.find(key_);
    if (found == snapshot.end()) {
        return default_json();
    }
    return parse_json(*found);
}

std::optional<Json> SettingBase::stored_json(const Json& snapshot) const {
    if (!snapshot.is_object()) {
        return std::nullopt;
    }
    const auto found = snapshot.find(key_);
    if (found == snapshot.end()) {
        return std::nullopt;
    }
    return convert(*found);
}

} // namespace fv
