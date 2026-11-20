#pragma once

#include <catch2/catch_tostring.hpp>

#include <optional>
#include <string>

#include "base/json.h"
#include "focus/snapshot.h"

// Golden-vector encodings of the focus types (tools/parity/cases/services.focus_context.py):
// objects of the dataclass fields, missing fields at their defaults, null for None.
namespace fv::focus::test {

inline std::optional<AppIdentity> identity_from(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    AppIdentity identity;
    identity.app_id = value.value("app_id", "");
    identity.name = value.value("name", "");
    if (const auto pid = value.find("pid"); pid != value.end() && !pid->is_null()) {
        identity.pid = pid->get<std::uint32_t>();
    }
    identity.window = value.value("window", "");
    identity.title_hint = value.value("title_hint", "");
    identity.is_self = value.value("is_self", false);
    identity.platform = value.value("platform", "");
    return identity;
}

inline std::optional<TextContext> context_from(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return TextContext{.before = value.value("before", ""),
                       .selected = value.value("selected", ""),
                       .after = value.value("after", ""),
                       .caret_known = value.value("caret_known", false),
                       .selection_known = value.value("selection_known", false),
                       .source = value.value("source", ""),
                       .blocked = value.value("blocked", false)};
}

inline std::optional<FocusSnapshot> snapshot_from(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return FocusSnapshot{identity_from(value.value("identity", Json())),
                         context_from(value.value("text", Json()))};
}

inline TextContext caret(std::string before = "", std::string after = "", std::string selected = "",
                         bool selection_known = false) {
    return TextContext{.before = std::move(before),
                       .selected = std::move(selected),
                       .after = std::move(after),
                       .caret_known = true,
                       .selection_known = selection_known,
                       .source = "uia"};
}

inline std::string describe(const std::optional<AppIdentity>& identity) {
    if (!identity) {
        return "None";
    }
    return "AppIdentity(" + identity->app_id + ", " + identity->name +
           ", pid=" + (identity->pid ? std::to_string(*identity->pid) : std::string("None")) +
           ", window=" + identity->window + ", hint=" + identity->title_hint +
           (identity->is_self ? ", self" : "") + ", " + identity->platform + ")";
}

inline std::string describe(const std::optional<TextContext>& context) {
    if (!context) {
        return "None";
    }
    return "TextContext(before=" + context->before + ", selected=" + context->selected +
           ", after=" + context->after + ", caret=" + (context->caret_known ? "1" : "0") +
           ", selection=" + (context->selection_known ? "1" : "0") + ", source=" + context->source +
           (context->blocked ? ", blocked" : "") + ")";
}

} // namespace fv::focus::test

template <> struct Catch::StringMaker<fv::focus::FocusSnapshot> {
    static std::string convert(const fv::focus::FocusSnapshot& snapshot) {
        return "FocusSnapshot(" + fv::focus::test::describe(snapshot.identity) + ", " +
               fv::focus::test::describe(snapshot.text) + ")";
    }
};

template <> struct Catch::StringMaker<fv::focus::AppIdentity> {
    static std::string convert(const fv::focus::AppIdentity& identity) {
        return fv::focus::test::describe(identity);
    }
};

template <> struct Catch::StringMaker<fv::focus::TextContext> {
    static std::string convert(const fv::focus::TextContext& context) {
        return fv::focus::test::describe(context);
    }
};
