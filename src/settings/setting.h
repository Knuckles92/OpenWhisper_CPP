#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "base/json.h"

namespace fv {

// Insertion order and the int/float/bool distinction are kept (the same alias as base/json.h).
using Json = nlohmann::ordered_json;

// The kinds of services/setting_types.py, one per Python class.
enum class SettingKind : std::uint8_t {
    Bool,
    Int,
    Float,
    Str,
    Choice,
    IntChoice,
    List,
    Dict,
    OptionalList,
    OptionalDict,
    Raw,
};

// The Python class name of a kind ("BoolSetting", ...).
std::string_view kind_name(SettingKind);

// The normalizers OpenWhisper's declarations use, by what they do: str.strip, _lower_stripped
// (strip, then lower) and _batch_instructions (strip, then keep the first `cut` code points).
enum class Normalize : std::uint8_t { None, Strip, LowerStripped, StripAndCut };

// A name an earlier build saved, and the choice it means now.
struct ChoiceAlias {
    std::string_view from;
    std::string_view to;
};

// What a declaration holds besides its key. Written by tools/gen/settings_keys.py through the
// spec:: makers and the setters below; every field is a literal, so each keys:: declaration is
// a constant. A List or Dict default is its JSON text, parsed when read.
struct SettingSpec {
    SettingKind kind = SettingKind::Raw;
    bool in_table = true; // the default belongs in keys::defaults_table() (SETTING_DEFAULTS)
    bool is_dropped = false;
    bool is_strict = false; // Int, IntChoice: a JSON integer only, never a bool, float or text
    bool flag_default = false;
    std::int64_t int_default = 0;
    double float_default = 0.0;
    std::string_view text_default;
    std::span<const std::string_view> choices;
    std::span<const ChoiceAlias> aliases;
    std::span<const std::int64_t> int_choices;
    std::optional<std::int64_t> valid_low; // outside [valid_low, valid_high]: the default
    std::optional<std::int64_t> valid_high;
    std::optional<std::int64_t> clamp_low; // then pulled into [clamp_low, clamp_high]
    std::optional<std::int64_t> clamp_high;
    Normalize normalize = Normalize::None;
    std::size_t cut = 0; // StripAndCut: code points kept

    constexpr SettingSpec strict() const {
        SettingSpec copy = *this;
        copy.is_strict = true;
        return copy;
    }
    constexpr SettingSpec valid(std::optional<std::int64_t> low,
                                std::optional<std::int64_t> high) const {
        SettingSpec copy = *this;
        copy.valid_low = low;
        copy.valid_high = high;
        return copy;
    }
    constexpr SettingSpec clamp(std::optional<std::int64_t> low,
                                std::optional<std::int64_t> high) const {
        SettingSpec copy = *this;
        copy.clamp_low = low;
        copy.clamp_high = high;
        return copy;
    }
    constexpr SettingSpec with_aliases(std::span<const ChoiceAlias> table) const {
        SettingSpec copy = *this;
        copy.aliases = table;
        return copy;
    }
    constexpr SettingSpec normalized(Normalize how) const {
        SettingSpec copy = *this;
        copy.normalize = how;
        return copy;
    }
    constexpr SettingSpec stripped_and_cut(std::size_t code_points) const {
        SettingSpec copy = *this;
        copy.normalize = Normalize::StripAndCut;
        copy.cut = code_points;
        return copy;
    }
    constexpr SettingSpec not_in_table() const {
        SettingSpec copy = *this;
        copy.in_table = false;
        return copy;
    }
    constexpr SettingSpec dropped() const {
        SettingSpec copy = *this;
        copy.is_dropped = true;
        return copy;
    }
};

// The constructors of setting_types.py: BoolSetting(key, default), IntSetting(key, default),
// and so on. The Optional kinds and Raw are never in the table, as in Python.
// Members are assigned, not designated: GCC's -Wextra warns about every member a designated
// initializer leaves out, in each file that includes this header.
namespace spec {
constexpr SettingSpec of_kind(SettingKind kind) {
    SettingSpec made;
    made.kind = kind;
    return made;
}
constexpr SettingSpec boolean(bool value) {
    SettingSpec made = of_kind(SettingKind::Bool);
    made.flag_default = value;
    return made;
}
constexpr SettingSpec integer(std::int64_t value) {
    SettingSpec made = of_kind(SettingKind::Int);
    made.int_default = value;
    return made;
}
constexpr SettingSpec number(double value) {
    SettingSpec made = of_kind(SettingKind::Float);
    made.float_default = value;
    return made;
}
constexpr SettingSpec text(std::string_view value) {
    SettingSpec made = of_kind(SettingKind::Str);
    made.text_default = value;
    return made;
}
constexpr SettingSpec choice(std::string_view value, std::span<const std::string_view> choices) {
    SettingSpec made = of_kind(SettingKind::Choice);
    made.text_default = value;
    made.choices = choices;
    return made;
}
constexpr SettingSpec int_choice(std::int64_t value, std::span<const std::int64_t> choices) {
    SettingSpec made = of_kind(SettingKind::IntChoice);
    made.int_default = value;
    made.int_choices = choices;
    return made;
}
constexpr SettingSpec list(std::string_view json_default = "[]") {
    SettingSpec made = of_kind(SettingKind::List);
    made.text_default = json_default;
    return made;
}
constexpr SettingSpec dict(std::string_view json_default = "{}") {
    SettingSpec made = of_kind(SettingKind::Dict);
    made.text_default = json_default;
    return made;
}
constexpr SettingSpec optional_list() {
    return of_kind(SettingKind::OptionalList).not_in_table();
}
constexpr SettingSpec optional_dict() {
    return of_kind(SettingKind::OptionalDict).not_in_table();
}
constexpr SettingSpec raw() {
    return of_kind(SettingKind::Raw).not_in_table();
}
} // namespace spec

namespace detail {
// ChoiceSetting's ValueError("<key>: default '<x>' is not one of its choices"), as
// std::invalid_argument. Reached during constant evaluation it stops the compile instead.
[[noreturn]] void throw_bad_choice_default(std::string_view key, std::string_view value);
[[noreturn]] void throw_kind_mismatch(std::string_view key);
} // namespace detail

// A settings key with its kind, default and validation, type-erased: keys::all() and
// keys::by_key() hand these out. Values travel as JSON: read_json of a Bool is a JSON bool, of
// an Int a JSON integer, and so on.
// Affinity: thread-safe (immutable).
class SettingBase {
public:
    constexpr SettingBase(std::string_view key, const SettingSpec& spec) : key_(key), spec_(spec) {
        if (spec.kind == SettingKind::Choice) {
            bool found = false;
            for (const std::string_view choice : spec.choices) {
                found = found || choice == spec.text_default;
            }
            if (!found) {
                detail::throw_bad_choice_default(key, spec.text_default);
            }
        }
    }

    constexpr std::string_view key() const { return key_; }
    constexpr SettingKind kind() const { return spec_.kind; }
    constexpr bool in_table() const { return spec_.in_table; }
    // Keys of dropped engines: preserved, never read.
    constexpr bool dropped() const { return spec_.is_dropped; }
    constexpr const SettingSpec& spec() const { return spec_; }

    // Setting.default: a fresh copy, so a caller may change what it gets.
    Json default_json() const;
    // Setting._convert: the value a stored `raw` stands for, or nullopt when it is not valid.
    std::optional<Json> convert(const Json& raw) const;
    // Setting.parse: the converted value, or the default.
    Json parse_json(const Json& raw) const;
    // Setting.read: unset or invalid -> the default. `snapshot` is a settings mapping.
    Json read_json(const Json& snapshot) const;
    // Setting.stored: unset or invalid -> nullopt, never the default.
    std::optional<Json> stored_json(const Json& snapshot) const;

private:
    std::string_view key_;
    SettingSpec spec_;
};

namespace detail {
template <class T> constexpr bool holds(SettingKind kind) {
    if constexpr (std::is_same_v<T, bool>) {
        return kind == SettingKind::Bool;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return kind == SettingKind::Int || kind == SettingKind::IntChoice;
    } else if constexpr (std::is_same_v<T, double>) {
        return kind == SettingKind::Float;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return kind == SettingKind::Str || kind == SettingKind::Choice;
    } else if constexpr (std::is_same_v<T, Json>) {
        return kind == SettingKind::List || kind == SettingKind::Dict ||
               kind == SettingKind::OptionalList || kind == SettingKind::OptionalDict ||
               kind == SettingKind::Raw;
    } else {
        static_assert(sizeof(T) == 0, "Setting<T>: T is bool, std::int64_t, double, "
                                      "std::string or Json");
    }
}

template <class T> T from_json(const Json& value) {
    if constexpr (std::is_same_v<T, Json>) {
        return value;
    } else {
        return value.get<T>();
    }
}
} // namespace detail

// One declared settings key with Python's kind, default and fallback rules. Kinds: Bool, Int
// (strict/valid/clamp), Float, Str, Choice, IntChoice, List, Dict, Optional*, Raw; each kind's
// alias (BoolSetting, ...) is defined below. A List, Dict, OptionalList, OptionalDict or Raw
// setting reads as Json; an Optional kind or Raw reads null when unset (Python's None).
// Affinity: thread-safe (immutable).
template <class T> class Setting : public SettingBase {
public:
    constexpr Setting(std::string_view key, const SettingSpec& spec) : SettingBase(key, spec) {
        if (!detail::holds<T>(spec.kind)) {
            detail::throw_kind_mismatch(key);
        }
    }

    constexpr std::string_view key() const { return SettingBase::key(); }
    T read(const Json& snapshot) const { return detail::from_json<T>(read_json(snapshot)); }
    std::optional<T> stored(const Json& snapshot) const {
        auto value = stored_json(snapshot);
        if (!value) {
            return std::nullopt;
        }
        return detail::from_json<T>(*value);
    }
    T default_value() const { return detail::from_json<T>(default_json()); }
    T parse(const Json& raw) const { return detail::from_json<T>(parse_json(raw)); }
    bool dropped() const { return SettingBase::dropped(); }
};

using BoolSetting = Setting<bool>;
using IntSetting = Setting<std::int64_t>;
using FloatSetting = Setting<double>;
using StrSetting = Setting<std::string>;
using ChoiceSetting = Setting<std::string>;
using IntChoiceSetting = Setting<std::int64_t>;
using ListSetting = Setting<Json>;
using DictSetting = Setting<Json>;
using OptionalListSetting = Setting<Json>;
using OptionalDictSetting = Setting<Json>;
using RawSetting = Setting<Json>;

} // namespace fv
