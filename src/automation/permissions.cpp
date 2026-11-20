#include "automation/permissions.h"

#include <format>
#include <utility>

#include "base/identity.h"
#include "settings/resolve.h"

namespace fv::automation::permissions {

namespace {

constexpr std::array<Permission, 5> kAll = {Permission::LiveEvents, Permission::RunActions,
                                            Permission::ControlRecording, Permission::TextFilters,
                                            Permission::AgentPrompts};

std::string denied_message(std::string_view permission) {
    const std::string_view product = identity().product;
    const auto where = [&](std::string_view what) {
        return std::format("Allow {} in {} Settings > API & MCP.", what, product);
    };
    if (permission == LIVE_EVENTS) {
        return where("live events");
    }
    if (permission == RUN_ACTIONS) {
        return where("running actions");
    }
    if (permission == CONTROL_RECORDING) {
        return where("starting recordings");
    }
    if (permission == TEXT_FILTERS) {
        return where("changing dictated text");
    }
    if (permission == AGENT_PROMPTS) {
        return where("sending words to your coding agents");
    }
    return "This action is not allowed.";
}

} // namespace

std::string_view key(Permission permission) {
    return ALL[static_cast<std::size_t>(permission)];
}

std::optional<Permission> from_key(std::string_view value) {
    for (const Permission permission : kAll) {
        if (key(permission) == value) {
            return permission;
        }
    }
    return std::nullopt;
}

std::string_view public_name(Permission permission) {
    switch (permission) {
    case Permission::LiveEvents:
        return "live_events";
    case Permission::RunActions:
        return "run_actions";
    case Permission::ControlRecording:
        return "control_recording";
    case Permission::TextFilters:
        return "text_filters";
    case Permission::AgentPrompts:
        return "agent_prompts";
    }
    return {};
}

PermissionDenied::PermissionDenied(std::string permission)
    : std::runtime_error(denied_message(permission)), permission_(std::move(permission)) {}

bool granted(const Json& settings, std::string_view permission) {
    if (!from_key(permission)) {
        return false;
    }
    static const Json kEmpty = Json::object();
    return resolve_bool_setting(permission, settings.is_object() ? settings : kEmpty);
}

bool granted(const Json& settings, Permission permission) {
    return granted(settings, key(permission));
}

void require(const Json& settings, std::string_view permission) {
    if (!granted(settings, permission)) {
        throw PermissionDenied(std::string(permission));
    }
}

void require(const Json& settings, Permission permission) {
    require(settings, key(permission));
}

Json describe(const Json& settings) {
    Json result = Json::object();
    for (const Permission permission : kAll) {
        result[std::string(public_name(permission))] = granted(settings, permission);
    }
    return result;
}

} // namespace fv::automation::permissions
