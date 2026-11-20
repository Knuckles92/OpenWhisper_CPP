#pragma once

#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "automation/action_model.h"
#include "automation/routing.h"
#include "base/json.h"
#include "golden/golden.h"
#include "golden_support.h"

// Shared by the Route by voice tests: actions as OpenWhisper's routing tests write them, and the
// named action sets the routing vectors (tools/parity/cases/services.actions.routing.py) use.
namespace fv::automation::test {

inline constexpr std::string_view kRoutingModule = "services.actions.routing";

inline Action make_action(std::string id, std::string name, std::string kind,
                          Json params = Json::object(), std::vector<std::string> spoken = {},
                          std::string description = {}, bool enabled = true) {
    Action action;
    action.id = std::move(id);
    action.name = std::move(name);
    action.kind = std::move(kind);
    action.params = std::move(params);
    action.spoken_names = std::move(spoken);
    action.description = std::move(description);
    action.enabled = enabled;
    return action;
}

// The ids of the actions `text` names.
inline std::vector<std::string> named(std::string_view text, const std::vector<Action>& actions,
                                      std::string_view route = ROUTE_AUTO) {
    std::vector<std::string> ids;
    for (const GateMatch& match : gate(text, actions, route)) {
        ids.push_back(match.action_id);
    }
    return ids;
}

// A named action set of the routing vectors ("gate", "presets", "benchmark:everyday", ...).
inline const std::vector<Action>& golden_set(const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, std::vector<Action>, std::less<>> sets;
    std::lock_guard lock(mutex);
    if (sets.empty()) {
        for (const Json& item : golden::load(kRoutingModule).at("functions").at("action_set")) {
            std::vector<Action> actions;
            for (const Json& spec : item.at("result")) {
                actions.push_back(action_from(spec));
            }
            sets[item.at("args").at(0).get<std::string>()] = std::move(actions);
        }
    }
    return sets.at(name);
}

// The actions argument of a vector: a set's name.
inline const std::vector<Action>& actions_arg(const golden::Case& c, std::size_t index) {
    return golden_set(c.arg<std::string>(index));
}

} // namespace fv::automation::test
