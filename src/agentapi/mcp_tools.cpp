#include "agentapi/mcp_tools.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <typeinfo>
#include <utility>

#include "agentapi/agent_controls.h"
#include "agentapi/data/mcp_tools.json.h"
#include "agentapi/detail/pydantic.h"
#include "agentapi/history_routes.h"
#include "automation/catalog.h"
#include "automation/control.h"
#include "automation/event_bus.h"
#include "automation/permissions.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "settings/store.h"

namespace fv::agentapi {

namespace {

constexpr log::Logger kLog{"services.agent_mcp.app"};

namespace permissions = automation::permissions;
using detail::LineError;

// A failure the agent reads after "Error executing tool <name>: ".
class ToolFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// ---- Arguments ------------------------------------------------------------------------------

enum class Kind {
    Str,      // str with Field constraints; never JSON-pre-parsed
    OptStr,   // str | None
    Int,      // int with bounds
    OptInt,   // int | None
    OptFloat, // float | None, >= 0, finite (Seconds)
    Bool,     // bool
    Literal,  // Literal[...] of str
    OptDate,  // datetime | None
    OptTypes, // list[Literal[PUBLIC_TYPES]] | None, 1..len(PUBLIC_TYPES) items
    Changes,  // dict[str, StrictBool | StrictInt | StrictFloat | StrictStr | None]
};

struct Field {
    std::string_view name;
    Kind kind;
    bool required = false;
    Json fallback = nullptr;
    std::size_t min_length = 0;
    std::size_t max_length = 0;
    bool record_id = false;
    std::optional<std::int64_t> ge;
    std::optional<std::int64_t> le;
    std::vector<std::string_view> choices;
};

Field record_id(std::string_view name, bool required = true) {
    Field field{name, required ? Kind::Str : Kind::OptStr};
    field.required = required;
    field.min_length = 1;
    field.max_length = 200;
    field.record_id = true;
    return field;
}

Field text(std::string_view name, std::size_t min_length, std::size_t max_length,
           Kind kind = Kind::Str, bool required = true) {
    Field field{name, kind};
    field.required = required;
    field.min_length = min_length;
    field.max_length = max_length;
    return field;
}

Field limit() {
    Field field{"limit", Kind::Int};
    field.fallback = 20;
    field.ge = 1;
    field.le = 100;
    return field;
}

Field cursor() {
    Field field{"cursor", Kind::OptStr};
    field.max_length = 2048;
    return field;
}

Field flag(std::string_view name) {
    Field field{name, Kind::Bool};
    field.fallback = false;
    return field;
}

Field date(std::string_view name) {
    return Field{name, Kind::OptDate};
}

Field seconds(std::string_view name) {
    Field field{name, Kind::OptFloat};
    field.ge = 0;
    return field;
}

std::string plural(std::size_t count, std::string_view noun) {
    return std::to_string(count) + " " + std::string(noun) + (count == 1 ? "" : "s");
}

std::string literal_message(const std::vector<std::string_view>& choices) {
    std::string out = "Input should be ";
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i > 0) {
            out += i + 1 == choices.size() ? " or " : ", ";
        }
        out += py::repr(choices[i]);
    }
    return out;
}

bool matches_record_id(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char32_t point : py::to_code_points(value)) {
        if (point < 0x20 || point == '/' || point == '\\' || point == '?' || point == '#') {
            return false;
        }
    }
    return true;
}

const std::vector<std::string_view>& event_types() {
    static const std::vector<std::string_view> types = automation::catalog::PUBLIC_TYPES();
    return types;
}

// What FuncMetadata.pre_parse_json does for a parameter not annotated as plain str: a string
// holding a JSON list, object or null is replaced by it (Claude Desktop sends lists that way).
Json pre_parsed(const Field& field, const Json& value) {
    if (field.kind == Kind::Str || !value.is_string()) {
        return value;
    }
    try {
        Json parsed = py::loads(value.get_ref<const std::string&>());
        if (parsed.is_array() || parsed.is_object() || parsed.is_null()) {
            return parsed;
        }
    } catch (const py::PyError&) {
    }
    return value;
}

void refuse(std::vector<LineError>& errors, std::string loc, const detail::Refusal& refusal,
            const Json& input) {
    errors.push_back({std::move(loc), refusal.type, refusal.message, input});
}

// One field's value, or its errors appended.
std::optional<Json> validate_field(const Field& field, const Json& value,
                                   std::vector<LineError>& errors) {
    const std::string loc(field.name);
    const bool nullable = field.kind == Kind::OptStr || field.kind == Kind::OptInt ||
                          field.kind == Kind::OptFloat || field.kind == Kind::OptDate ||
                          field.kind == Kind::OptTypes;
    if (value.is_null() && nullable) {
        return Json(nullptr);
    }
    switch (field.kind) {
    case Kind::Str:
    case Kind::OptStr: {
        auto text = detail::lax_str(value);
        if (!text) {
            refuse(errors, loc, text.error(), value);
            return std::nullopt;
        }
        const std::size_t length = py::len(*text);
        if (length < field.min_length) {
            errors.push_back(
                {loc, "string_too_short",
                 "String should have at least " + plural(field.min_length, "character"), value});
            return std::nullopt;
        }
        if (field.max_length != 0 && length > field.max_length) {
            errors.push_back({loc, "string_too_long",
                              "String should have at most " + plural(field.max_length, "character"),
                              value});
            return std::nullopt;
        }
        if (field.record_id && !matches_record_id(*text)) {
            errors.push_back(
                {loc, "string_pattern_mismatch",
                 "String should match pattern '" + std::string(RECORD_ID_PATTERN) + "'", value});
            return std::nullopt;
        }
        return Json(*text);
    }
    case Kind::Int:
    case Kind::OptInt: {
        auto number = detail::lax_int(value);
        if (!number) {
            refuse(errors, loc, number.error(), value);
            return std::nullopt;
        }
        if (field.ge && *number < *field.ge) {
            errors.push_back(
                {loc, "greater_than_equal",
                 "Input should be greater than or equal to " + std::to_string(*field.ge), value});
            return std::nullopt;
        }
        if (field.le && *number > *field.le) {
            errors.push_back({loc, "less_than_equal",
                              "Input should be less than or equal to " + std::to_string(*field.le),
                              value});
            return std::nullopt;
        }
        return Json(*number);
    }
    case Kind::OptFloat: {
        auto number = detail::lax_float(value);
        if (!number) {
            refuse(errors, loc, number.error(), value);
            return std::nullopt;
        }
        if (field.ge && *number < static_cast<double>(*field.ge)) {
            errors.push_back(
                {loc, "greater_than_equal",
                 "Input should be greater than or equal to " + std::to_string(*field.ge), value});
            return std::nullopt;
        }
        return Json(*number);
    }
    case Kind::Bool: {
        auto truth = detail::lax_bool(value);
        if (!truth) {
            refuse(errors, loc, truth.error(), value);
            return std::nullopt;
        }
        return Json(*truth);
    }
    case Kind::Literal: {
        if (value.is_string() && !detail::has_surrogate(value.get_ref<const std::string&>())) {
            for (const std::string_view choice : field.choices) {
                if (value.get_ref<const std::string&>() == choice) {
                    return std::optional<Json>(std::in_place, value);
                }
            }
        }
        errors.push_back({loc, "literal_error", literal_message(field.choices), value});
        return std::nullopt;
    }
    case Kind::OptDate: {
        auto when = detail::lax_datetime(value);
        if (!when) {
            refuse(errors, loc, when.error(), value);
            return std::nullopt;
        }
        return Json(when->isoformat());
    }
    case Kind::OptTypes: {
        if (!value.is_array()) {
            errors.push_back({loc, "list_type", "Input should be a valid list", value});
            return std::nullopt;
        }
        const std::size_t before = errors.size();
        for (std::size_t i = 0; i < value.size(); ++i) {
            const Json& item = value[i];
            bool known = false;
            if (item.is_string()) {
                for (const std::string_view type : event_types()) {
                    known = known || item.get_ref<const std::string&>() == type;
                }
            }
            if (!known) {
                errors.push_back({loc + "." + std::to_string(i), "literal_error",
                                  literal_message(event_types()), item});
            }
        }
        if (errors.size() != before) {
            return std::nullopt;
        }
        if (value.empty()) {
            errors.push_back({loc, "too_short",
                              "List should have at least 1 item after validation, not 0", value});
            return std::nullopt;
        }
        if (value.size() > event_types().size()) {
            errors.push_back({loc, "too_long",
                              "List should have at most " + std::to_string(event_types().size()) +
                                  " items after validation, not " + std::to_string(value.size()),
                              value});
            return std::nullopt;
        }
        return std::optional<Json>(std::in_place, value);
    }
    case Kind::Changes: {
        if (!value.is_object()) {
            errors.push_back({loc, "dict_type", "Input should be a valid dictionary", value});
            return std::nullopt;
        }
        const std::size_t before = errors.size();
        for (const auto& [key, item] : value.items()) {
            if (item.is_null() || item.is_boolean() || item.is_number() || item.is_string()) {
                continue;
            }
            const std::string base = loc + "." + key;
            errors.push_back(
                {base + ".bool", "bool_type", "Input should be a valid boolean", item});
            errors.push_back({base + ".int", "int_type", "Input should be a valid integer", item});
            errors.push_back(
                {base + ".float", "float_type", "Input should be a valid number", item});
            errors.push_back(
                {base + ".str", "string_type", "Input should be a valid string", item});
        }
        if (errors.size() != before) {
            return std::nullopt;
        }
        return std::optional<Json>(std::in_place, value);
    }
    }
    return std::nullopt;
}

using Handler = std::function<Json(const Json& args)>;

struct Tool {
    std::string_view name;
    std::vector<Field> fields;
    Handler handler;
};

// ---- History tools --------------------------------------------------------------------------

// httpx's normalize_path, which every request URL goes through: "." and ".." segments dropped
// (RFC 3986 5.2.4). A record id may be "." or "..", and Python's tool then asks the History API
// for the path they lead to.
std::string normalize_path(const std::string& path) {
    std::vector<std::string_view> components;
    const std::string_view text(path);
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = text.find('/', start);
        components.push_back(text.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    if (std::ranges::find(components, ".") == components.end() &&
        std::ranges::find(components, "..") == components.end()) {
        return path;
    }
    std::vector<std::string_view> output;
    for (const std::string_view component : components) {
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            if (!output.empty() && !(output.size() == 1 && output.front().empty())) {
                output.pop_back();
            }
            continue;
        }
        output.push_back(component);
    }
    std::string out;
    for (std::size_t i = 0; i < output.size(); ++i) {
        out += (i == 0 ? "" : "/") + std::string(output[i]);
    }
    return out;
}

std::string query_text(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    return value.get<std::string>();
}

} // namespace

const McpCatalog& mcp_catalog() {
    static const McpCatalog catalog = [] {
        // tools/gen/mcp_tools.py writes one product name throughout, the server's own; it
        // becomes identity().product wherever it appears.
        const std::string text(embedded::agentapi_mcp_tools);
        const std::string written = py::loads(text).at("server").at("name").get<std::string>();
        Json document = py::loads(replace_all(text, written, std::string(identity().product)));
        McpCatalog out;
        out.server = document.at("server");
        out.instructions = document.at("instructions").get<std::string>();
        out.tools = document.at("tools");
        return out;
    }();
    return catalog;
}

Json tool_annotations(std::string_view kind) {
    struct Hints {
        std::string_view kind;
        bool destructive, idempotent, open_world, read_only;
    };
    static constexpr Hints table[] = {
        {"read", false, true, false, true},     {"write", false, true, false, false},
        {"run", true, false, true, false},      {"record", false, false, false, false},
        {"stop", false, true, false, false},    {"cancel", true, true, false, false},
        {"notice", false, false, false, false}, {"live_read", false, false, false, true},
    };
    for (const Hints& hints : table) {
        if (hints.kind == kind) {
            Json out = Json::object();
            out["destructiveHint"] = hints.destructive;
            out["idempotentHint"] = hints.idempotent;
            out["openWorldHint"] = hints.open_world;
            out["readOnlyHint"] = hints.read_only;
            return out;
        }
    }
    throw std::out_of_range("no tool annotations of kind " + std::string(kind));
}

namespace {

class Runner {
public:
    explicit Runner(const McpToolsOptions& options) : options_(options) {}

    // _control_call
    template <class Fn> Json control(Fn&& fn) const {
        try {
            return fn();
        } catch (const ControlError& error) {
            throw ToolFailure(error.what());
        } catch (const std::exception& error) {
            // Never echo the exception: it may carry SQL, dictated text or paths.
            kLog.warning("An MCP control call failed: {}", typeid(error).name());
            throw ToolFailure(
                "storage_unavailable: Could not save or read the requested data. Retry the "
                "request.");
        }
    }

    // _live_caller: the permission is read from the saved settings on every call, so turning
    // it off takes effect for the very next request.
    template <class Fn> Json live(std::optional<std::string_view> permission, Fn&& fn) const {
        try {
            if (options_.enabled && !options_.enabled()) {
                throw ToolFailure("disabled: MCP access is turned off.");
            }
            const Json saved =
                options_.settings != nullptr ? *options_.settings->snapshot() : Json::object();
            if (permission) {
                permissions::require(saved, *permission);
            }
            return fn(saved);
        } catch (const ToolFailure&) {
            throw;
        } catch (const permissions::PermissionDenied& error) {
            throw ToolFailure("permission_denied: " + error.message());
        } catch (const automation::ControlError& error) {
            throw ToolFailure(error.code() + ": " + error.message());
        } catch (const std::exception& error) {
            // Never echo the exception: it may carry dictated text or paths.
            kLog.warning("An MCP live tool failed: {}", typeid(error).name());
            throw ToolFailure("unavailable: " + std::string(identity().product) +
                              " could not do that right now. Retry the request.");
        }
    }

    // _history_getter: the History API's answer, in process, with the same token.
    Json get(std::string path, const Json& args,
             std::initializer_list<std::string_view> names) const {
        net::HttpRequest request;
        request.method = "GET";
        request.path = normalize_path(path);
        request.tls = false;
        request.peer = {"127.0.0.1", 0};
        request.headers = {{"host", "127.0.0.1"},
                           {"accept", "*/*"},
                           {"accept-encoding", "gzip, deflate"},
                           {"connection", "keep-alive"},
                           {"user-agent", "python-httpx"},
                           {"authorization", "Bearer " + options_.token}};
        for (const std::string_view name : names) {
            const Json& value = args.at(std::string(name));
            if (!value.is_null()) {
                request.query.emplace_back(std::string(name), query_text(value));
            }
        }
        const ApiResponse response = options_.history->handle(request);
        if (response.status >= 400) {
            const Json body = py::loads(response.body);
            const Json& error = body.at("error");
            throw ToolFailure(error.at("code").get<std::string>() + ": " +
                              error.at("message").get<std::string>());
        }
        return py::loads(response.body);
    }

    AgentControls& controls() const { return *options_.controls; }

private:
    const McpToolsOptions& options_;
};

Json job(std::string id) {
    Json out = Json::object();
    out["job_id"] = std::move(id);
    return out;
}

Json flag_result(std::string_view key) {
    Json out = Json::object();
    out[std::string(key)] = true;
    return out;
}

// _recent_events
Json recent_events(const Json& args) {
    automation::EventBus& bus = automation::bus();
    std::optional<std::vector<std::string>> wanted;
    if (args.at("types").is_array()) {
        wanted.emplace();
        for (const Json& type : args.at("types")) {
            wanted->push_back(type.get<std::string>());
        }
    }
    const auto limit = args.at("limit").get<std::int64_t>();
    std::vector<automation::Event> items;
    bool gap = false;
    bool more = false;
    if (args.at("since_seq").is_null()) {
        items = bus.recent(limit, wanted);
    } else {
        automation::SinceResult found = bus.since(args.at("since_seq").get<std::int64_t>(), wanted);
        items = std::move(found.events);
        gap = found.gap.has_value();
        more = static_cast<std::int64_t>(items.size()) > limit;
        if (more) {
            items.resize(static_cast<std::size_t>(limit));
        }
    }
    Json events = Json::array();
    for (const automation::Event& event : items) {
        events.push_back(event.to_dict());
    }
    Json out = Json::object();
    out["events"] = std::move(events);
    out["gap"] = gap;
    out["has_more"] = more;
    out["last_seq"] = bus.last_seq();
    return out;
}

std::vector<Tool> control_tools(const Runner& run, bool allow_live_tools) {
    namespace keys = permissions;
    std::vector<Tool> tools;

    // Control tools.
    tools.push_back({"get_capabilities", {}, [&run](const Json&) {
                         return run.control([&] { return run.controls().capabilities(); });
                     }});
    tools.push_back({"get_settings", {}, [&run](const Json&) {
                         return run.control([&] { return run.controls().get_settings(); });
                     }});
    {
        Field changes{"changes", Kind::Changes};
        changes.required = true;
        tools.push_back({"update_settings", {changes}, [&run](const Json& args) {
                             return run.control([&] {
                                 return run.controls().update_settings(args.at("changes"));
                             });
                         }});
    }
    tools.push_back({"retitle_transcription",
                     {record_id("record_id"), text("title", 1, 200)},
                     [&run](const Json& args) {
                         return run.control([&] {
                             return run.controls().retitle("transcription",
                                                           args.at("record_id").get<std::string>(),
                                                           args.at("title").get<std::string>());
                         });
                     }});
    tools.push_back({"retitle_meeting",
                     {record_id("meeting_id"), text("title", 1, 200)},
                     [&run](const Json& args) {
                         return run.control([&] {
                             return run.controls().retitle("meeting",
                                                           args.at("meeting_id").get<std::string>(),
                                                           args.at("title").get<std::string>());
                         });
                     }});

    if (!allow_live_tools)
        return tools;

    // Live tools.
    tools.push_back({"get_live_state", {}, [&run](const Json&) {
                         return run.live(std::nullopt, [](const Json& saved) {
                             Json state = automation::require_app_control()->state().to_dict();
                             state["permissions"] = permissions::describe(saved);
                             return state;
                         });
                     }});
    tools.push_back({"list_actions", {}, [&run](const Json&) {
                         return run.live(std::nullopt, [](const Json&) {
                             Json out = Json::object();
                             out["actions"] = automation::require_app_control()->list_actions();
                             return out;
                         });
                     }});
    tools.push_back({"run_action",
                     {text("action_id", 1, 200), text("text", 1, MAX_ACTION_TEXT)},
                     [&run](const Json& args) {
                         return run.live(keys::RUN_ACTIONS, [&](const Json&) {
                             return job(automation::require_app_control()->run_action(
                                 args.at("action_id").get_ref<const std::string&>(),
                                 args.at("text").get_ref<const std::string&>(), "mcp"));
                         });
                     }});

    // Recording tools.
    tools.push_back(
        {"start_dictation",
         {text("action_id", 1, 200, Kind::OptStr, false)},
         [&run](const Json& args) {
             return run.live(keys::CONTROL_RECORDING, [&](const Json&) {
                 const Json& action = args.at("action_id");
                 // action_id or "": no action is an empty id, as the live API's.
                 return job(automation::require_app_control()->start_recording(
                     action.is_string() ? action.get<std::string>() : std::string(), "mcp"));
             });
         }});
    tools.push_back({"stop_dictation", {}, [&run](const Json&) {
                         return run.live(keys::CONTROL_RECORDING, [](const Json&) {
                             automation::require_app_control()->stop_recording();
                             return flag_result("stopped");
                         });
                     }});
    tools.push_back({"cancel_dictation", {}, [&run](const Json&) {
                         return run.live(keys::CONTROL_RECORDING, [](const Json&) {
                             automation::require_app_control()->cancel();
                             return flag_result("canceled");
                         });
                     }});

    // Events and notices.
    {
        Field types{"types", Kind::OptTypes};
        Field since{"since_seq", Kind::OptInt};
        since.ge = 0;
        tools.push_back({"recent_events", {types, since, limit()}, [&run](const Json& args) {
                             return run.live(keys::LIVE_EVENTS,
                                             [&](const Json&) { return recent_events(args); });
                         }});
    }
    {
        Field title = text("title", 0, MAX_NOTICE_TITLE, Kind::Str, false);
        title.fallback = "";
        tools.push_back(
            {"notify_user", {text("text", 1, MAX_NOTICE_TEXT), title}, [&run](const Json& args) {
                 return run.live(keys::RUN_ACTIONS, [&](const Json&) {
                     automation::require_app_control()->notify(
                         args.at("text").get_ref<const std::string&>(),
                         args.at("title").get_ref<const std::string&>());
                     return flag_result("shown");
                 });
             }});
    }

    return tools;
}

std::vector<Tool> make_tools(const Runner& run, bool local_history_only, bool allow_live_tools) {
    const auto list_fields = [] {
        return std::vector<Field>{limit(),
                                  cursor(),
                                  flag("include_remote"),
                                  flag("include_clients"),
                                  record_id("device_id", false),
                                  date("since"),
                                  date("before")};
    };
    auto tools = local_history_only ? std::vector<Tool>{} : control_tools(run, allow_live_tools);
    // Status and search.
    tools.push_back({"get_status", {}, [&run, local_history_only](const Json& args) {
                         Json status = run.get("/v1/status", args, {});
                         if (local_history_only) {
                             status["history_api_read_only"] = true;
                             status["read_only"] = true;
                             status["capabilities"] = Json::object();
                             return status;
                         }
                         const Json capabilities =
                             run.control([&] { return run.controls().capabilities(); });
                         bool read_only = true;
                         for (const std::string_view key : WRITE_CAPABILITIES) {
                             const Json& value = capabilities.at(std::string(key));
                             const bool granted =
                                 value.is_boolean() ? value.get<bool>() : !value.empty();
                             read_only = read_only && !granted;
                         }
                         status["history_api_read_only"] = true;
                         status["read_only"] = read_only;
                         status["capabilities"] = capabilities;
                         return status;
                     }});
    {
        Field kind{"kind", Kind::Literal};
        kind.fallback = "all";
        kind.choices = {"all", "transcription", "meeting"};
        std::vector<Field> fields{text("q", 1, 500), kind};
        for (Field& field : list_fields()) {
            fields.push_back(std::move(field));
        }
        tools.push_back({"search_history", fields, [&run](const Json& args) {
                             return run.get("/v1/search", args,
                                            {"q", "kind", "limit", "cursor", "include_remote",
                                             "include_clients", "device_id", "since", "before"});
                         }});
    }

    // Transcriptions.
    {
        std::vector<Field> fields{text("q", 1, 500, Kind::OptStr, false)};
        for (Field& field : list_fields()) {
            fields.push_back(std::move(field));
        }
        tools.push_back({"list_transcriptions", fields, [&run](const Json& args) {
                             return run.get("/v1/transcriptions", args,
                                            {"q", "limit", "cursor", "include_remote",
                                             "include_clients", "device_id", "since", "before"});
                         }});
    }
    tools.push_back(
        {"get_transcription",
         {record_id("record_id"), flag("include_remote"), record_id("device_id", false)},
         [&run](const Json& args) {
             return run.get("/v1/transcriptions/" + args.at("record_id").get<std::string>(), args,
                            {"include_remote", "device_id"});
         }});

    // Meetings.
    {
        std::vector<Field> fields{text("q", 1, 500, Kind::OptStr, false)};
        for (Field& field : list_fields()) {
            fields.push_back(std::move(field));
        }
        tools.push_back({"list_meetings", fields, [&run](const Json& args) {
                             return run.get("/v1/meetings", args,
                                            {"q", "limit", "cursor", "include_remote",
                                             "include_clients", "device_id", "since", "before"});
                         }});
    }
    tools.push_back(
        {"get_meeting",
         {record_id("meeting_id"), flag("include_remote"), record_id("device_id", false)},
         [&run](const Json& args) {
             return run.get("/v1/meetings/" + args.at("meeting_id").get<std::string>(), args,
                            {"include_remote", "device_id"});
         }});
    tools.push_back(
        {"list_meeting_segments",
         {record_id("meeting_id"), limit(), cursor(), flag("include_remote"),
          record_id("device_id", false), seconds("start_s"), seconds("end_s")},
         [&run](const Json& args) {
             return run.get(
                 "/v1/meetings/" + args.at("meeting_id").get<std::string>() + "/segments", args,
                 {"limit", "cursor", "include_remote", "device_id", "start_s", "end_s"});
         }});
    tools.push_back({"get_meeting_segment",
                     {record_id("meeting_id"), record_id("segment_id"), flag("include_remote"),
                      record_id("device_id", false)},
                     [&run](const Json& args) {
                         return run.get("/v1/meetings/" + args.at("meeting_id").get<std::string>() +
                                            "/segments/" + args.at("segment_id").get<std::string>(),
                                        args, {"include_remote", "device_id"});
                     }});
    tools.push_back(
        {"get_meeting_insights",
         {record_id("meeting_id"), flag("include_remote"), record_id("device_id", false)},
         [&run](const Json& args) {
             return run.get("/v1/meetings/" + args.at("meeting_id").get<std::string>() +
                                "/insights",
                            args, {"include_remote", "device_id"});
         }});
    return tools;
}

struct Registry {
    explicit Registry(const McpToolsOptions& options)
        : runner(options),
          tools(make_tools(runner, options.local_history_only, options.allow_live_tools)) {}
    Runner runner;
    std::vector<Tool> tools;
};

std::string indent2(const Json& value) {
    py::DumpOptions options;
    options.indent = 2;
    options.ensure_ascii = false;
    return py::dumps(value, options);
}

} // namespace

struct McpTools::Impl {
    explicit Impl(McpToolsOptions given) : options(std::move(given)), registry(options) {}
    McpToolsOptions options;
    Registry registry;
};

McpTools::McpTools(McpToolsOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

McpTools::~McpTools() = default;

bool McpTools::has(std::string_view name) const {
    return std::ranges::any_of(impl_->registry.tools,
                               [&](const Tool& tool) { return tool.name == name; });
}

Json McpTools::listed(const Json& catalog) const {
    if (!impl_->options.local_history_only && impl_->options.allow_live_tools)
        return catalog;
    Json tools = Json::array();
    for (const auto& tool : catalog)
        if (has(tool.at("name").get_ref<const std::string&>()))
            tools.push_back(tool);
    return tools;
}

std::string McpTools::instructions() const {
    if (impl_->options.local_history_only)
        return "Read saved dictations and meetings from this computer.";
    if (!impl_->options.allow_live_tools)
        return "Read saved dictations and meetings. Use get_capabilities to see which settings "
               "and titles this session may change.";
    return mcp_catalog().instructions;
}

std::optional<ToolResult> McpTools::call(std::string_view name, const Json& arguments) {
    const Tool* tool = nullptr;
    for (const Tool& candidate : impl_->registry.tools) {
        if (candidate.name == name) {
            tool = &candidate;
        }
    }
    if (tool == nullptr) {
        return std::nullopt;
    }
    const std::string prefix = "Error executing tool " + std::string(name);
    ToolResult result;
    result.is_error = true;

    // The arguments model: pre-parse, then each field in signature order.
    Json given = arguments.is_object() ? arguments : Json::object();
    for (const Field& field : tool->fields) {
        const std::string key(field.name);
        if (given.contains(key)) {
            given[key] = pre_parsed(field, given.at(key));
        }
    }
    std::vector<LineError> errors;
    Json args = Json::object();
    for (const Field& field : tool->fields) {
        const std::string key(field.name);
        if (!given.contains(key)) {
            if (field.required) {
                errors.push_back({key, "missing", "Field required", given});
            } else {
                args[key] = field.fallback;
            }
            continue;
        }
        if (auto value = validate_field(field, given.at(key), errors)) {
            args[key] = std::move(*value);
        }
    }
    if (!errors.empty()) {
        result.text = prefix + ": " +
                      detail::format_validation_error(std::string(name) + "Arguments", errors);
        return result;
    }
    try {
        Json value = tool->handler(args);
        result.is_error = false;
        result.text = indent2(value);
        result.structured = std::move(value);
    } catch (const ToolFailure& failure) {
        result.text = prefix + ": " + failure.what();
    } catch (const std::exception& error) {
        // A crash: its own text stays here.
        kLog.warning("MCP tool {} failed: {}", name, typeid(error).name());
        result.text = prefix;
    }
    return result;
}

} // namespace fv::agentapi
