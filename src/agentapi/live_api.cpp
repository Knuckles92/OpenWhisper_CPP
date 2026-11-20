#include "agentapi/live_api.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <typeinfo>
#include <utility>

#include "agentapi/data/live_openapi.json.h"
#include "agentapi/detail/pydantic.h"
#include "agentapi/history_server.h"
#include "agentapi/mcp_runtime.h"
#include "automation/catalog.h"
#include "automation/control.h"
#include "automation/filters.h"
#include "automation/permissions.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/log.h"
#include "net/address.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/store.h"

namespace fv::agentapi {

namespace {

constexpr log::Logger kLog{"services.automation.api"};

namespace permissions = automation::permissions;

using Responder = std::shared_ptr<net::Responder>;

// ControlError.code to HTTP status; any other code is 503.
int control_status(std::string_view code) {
    if (code == automation::BUSY) {
        return 409;
    }
    if (code == automation::NOT_FOUND) {
        return 404;
    }
    if (code == automation::REFUSED) {
        return 422;
    }
    return 503;
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// Code points of UTF-8 text, a lone surrogate's three bytes counting as one, as Python's len().
std::size_t code_points(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

// "20,000": format(n, ",").
std::string with_commas(std::size_t number) {
    std::string digits = std::to_string(number);
    for (std::size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        // Surrogates too: a lone one is kept as its three bytes, as surrogatepass keeps it.
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Starlette reads header values as Latin-1.
std::string latin1_to_utf8(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        append_utf8(out, static_cast<unsigned char>(c));
    }
    return out;
}

// bytes.decode("utf-8", "surrogatepass"): strict UTF-8 that lets encoded surrogates through.
bool valid_utf8_surrogatepass(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        std::size_t length = 0;
        std::uint32_t cp = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            cp = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            cp = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            cp = lead & 0x07;
        } else {
            return false;
        }
        if (i + length > bytes.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        const std::uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (cp < minimum || cp > 0x10FFFF) {
            return false;
        }
        i += length;
    }
    return true;
}

// UTF-16 or UTF-32 code units (big or little endian) as UTF-8, surrogates passed through.
std::optional<std::string> decode_units(std::string_view bytes, std::size_t width, bool big) {
    if (bytes.size() % width != 0) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> units;
    for (std::size_t i = 0; i < bytes.size(); i += width) {
        std::uint32_t unit = 0;
        for (std::size_t k = 0; k < width; ++k) {
            const auto byte = static_cast<unsigned char>(bytes[i + (big ? k : width - 1 - k)]);
            unit = (unit << 8) | byte;
        }
        units.push_back(unit);
    }
    std::string out;
    for (std::size_t i = 0; i < units.size(); ++i) {
        std::uint32_t cp = units[i];
        if (width == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        }
        if (cp > 0x10FFFF) {
            return std::nullopt;
        }
        append_utf8(out, cp);
    }
    return out;
}

// json.loads(bytes): json.detect_encoding, then decode with "surrogatepass". nullopt where
// Python raises UnicodeDecodeError.
std::optional<std::string> decode_json_bytes(std::string_view b) {
    const auto at = [&](std::size_t i) {
        return static_cast<unsigned char>(b[i]);
    };
    const auto starts = [&](std::string_view prefix) {
        return b.starts_with(prefix);
    };
    using namespace std::string_view_literals;
    if (starts("\x00\x00\xFE\xFF"sv)) {
        return decode_units(b.substr(4), 4, true);
    }
    if (starts("\xFF\xFE\x00\x00"sv)) {
        return decode_units(b.substr(4), 4, false);
    }
    if (starts("\xFE\xFF"sv)) {
        return decode_units(b.substr(2), 2, true);
    }
    if (starts("\xFF\xFE"sv)) {
        return decode_units(b.substr(2), 2, false);
    }
    if (starts("\xEF\xBB\xBF"sv)) {
        const std::string_view rest = b.substr(3);
        return valid_utf8_surrogatepass(rest) ? std::optional(std::string(rest)) : std::nullopt;
    }
    if (b.size() >= 4) {
        if (at(0) == 0) {
            return at(1) != 0 ? decode_units(b, 2, true) : decode_units(b, 4, true);
        }
        if (at(1) == 0) {
            return at(2) != 0 || at(3) != 0 ? decode_units(b, 2, false) : decode_units(b, 4, false);
        }
    } else if (b.size() == 2) {
        if (at(0) == 0) {
            return decode_units(b, 2, true);
        }
        if (at(1) == 0) {
            return decode_units(b, 2, false);
        }
    }
    return valid_utf8_surrogatepass(b) ? std::optional(std::string(b)) : std::nullopt;
}

bool ascii_digits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool ascii_blank(std::string_view bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    });
}

std::optional<std::string> last_query(const net::Query& query, std::string_view name) {
    std::optional<std::string> value;
    for (const auto& [key, item] : query) {
        if (key == name) {
            value = item; // Starlette's QueryParams keep the last value of a repeated name
        }
    }
    return value;
}

std::optional<std::string> header_value(const net::HttpRequest& request, std::string_view name) {
    if (auto value = net::header(request.headers, name)) {
        return std::string(*value);
    }
    return std::nullopt;
}

std::string param(const net::HttpRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.params) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

ApiResponse invalid_parameters() {
    // FastAPI's RequestValidationError, as the History API answers it.
    return error_response(422, "invalid_parameters",
                          "Invalid request parameters. See /openapi.json for the contract.");
}

void send(const Responder& responder, ApiResponse response) {
    responder->send(response.status, std::move(response.headers), std::move(response.body));
}

// The failure's type for the log: Python's name for a pyport error, else the C++ class.
std::string error_type(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    std::string name = typeid(error).name();
    for (const std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
    }
    return name;
}

// str.isdigit() over the Latin-1 text a header carries: the ASCII digits and the superscripts
// ¹ ² ³, which int() then refuses. Counts code points into `length`.
bool latin1_isdigit(std::string_view utf8, std::size_t& length) {
    length = 0;
    for (std::size_t i = 0; i < utf8.size(); ++length) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead >= '0' && lead <= '9') {
            ++i;
            continue;
        }
        if (lead == 0xC2 && i + 1 < utf8.size()) {
            const auto next = static_cast<unsigned char>(utf8[i + 1]);
            if (next == 0xB2 || next == 0xB3 || next == 0xB9) {
                i += 2;
                continue;
            }
        }
        return false;
    }
    return length > 0;
}

} // namespace

std::string client_header() {
    return "x-" + lower_ascii(identity().product) + "-client";
}

std::string legacy_client_header() {
    return "x-" + lower_ascii(legacy_identity().product) + "-client";
}

Invalid::Invalid(const std::string& message, int status, std::string code)
    : std::runtime_error(message), status_(status), code_(std::move(code)) {}

std::optional<std::vector<std::string>> parse_types(std::optional<std::string_view> raw) {
    if (!raw) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (const std::string& part : py::split_on(*raw, ",")) {
        std::string name = py::strip(part);
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    if (names.empty()) {
        return std::nullopt;
    }
    for (const std::string& name : names) {
        if (!automation::catalog::describe_type(name)) {
            throw Invalid("types names an event type " + std::string(identity().product) +
                          " doesn't publish.");
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::optional<std::int64_t> parse_last_event_id(std::optional<std::string_view> raw) {
    if (!raw) {
        return std::nullopt;
    }
    const std::string value = py::strip(*raw);
    if (value.empty()) {
        return std::nullopt;
    }
    std::size_t length = 0;
    if (!latin1_isdigit(value, length) || length > 18) {
        throw Invalid("Last-Event-ID must be the id of an event from this stream.");
    }
    if (!ascii_digits(value)) {
        // "²" passes isdigit() and fails int(): an unexpected failure in OpenWhisper too.
        throw py::ValueError("invalid literal for int() with base 10");
    }
    return std::stoll(value);
}

std::string sse_frame(const automation::Event& event, bool with_id) {
    std::string frame;
    if (with_id) {
        frame += "id: " + std::to_string(event.seq) + "\n";
    }
    frame += "event: " + event.type + "\n";
    frame += "data: " + event.to_json(true) + "\n\n";
    return frame;
}

automation::Event gap_event(std::int64_t missed_from_seq, std::string_view reason,
                            const Clock& clock) {
    using namespace std::chrono;
    const auto now = floor<milliseconds>(clock.utc());
    const auto seconds = floor<std::chrono::seconds>(now);
    automation::Event event;
    event.id = "evt_" + random_hex(24);
    event.seq = 0;
    event.type = std::string(automation::catalog::EVENTS_GAP);
    event.time = std::format("{:%Y-%m-%dT%H:%M:%S}.{:03d}Z", seconds, (now - seconds).count());
    event.data = automation::catalog::events_gap(missed_from_seq, reason);
    return event;
}

Json read_json_body(const net::HttpRequest& request, std::size_t limit) {
    const Invalid too_large("The request body is too large.", 413, "request_too_large");
    if (const auto declared = net::header(request.headers, "content-length");
        declared && ascii_digits(*declared)) {
        const std::string_view digits = *declared;
        const auto first = digits.find_first_not_of('0');
        const std::string_view significant =
            first == std::string_view::npos ? std::string_view("0") : digits.substr(first);
        if (significant.size() > 19 ||
            std::stoull(std::string(significant)) > static_cast<unsigned long long>(limit)) {
            throw too_large;
        }
    }
    if (request.body_too_large || request.body.size() > limit) {
        throw too_large;
    }
    if (ascii_blank(request.body)) {
        return Json::object();
    }
    const Invalid not_object("The request body must be a JSON object.");
    const std::optional<std::string> text = decode_json_bytes(request.body);
    if (!text) {
        throw not_object;
    }
    Json value;
    try {
        value = py::loads(*text);
    } catch (const py::ValueError&) {
        throw not_object;
    }
    if (!value.is_object()) {
        throw not_object;
    }
    return value;
}

bool peer_is_local(std::string_view host) {
    const auto address = net::parse_address(net::strip_scope(host));
    return address && net::is_loopback(net::unmap_v4(*address));
}

bool is_unicode_text(std::string_view utf8) {
    return !detail::has_surrogate(utf8);
}

std::string string_field(const Json& body, std::string_view key, bool required,
                         std::size_t max_chars, std::string_view label) {
    const std::string name(label.empty() ? key : label);
    const auto found = body.is_object() ? body.find(std::string(key)) : body.end();
    const bool missing = !body.is_object() || found == body.end() || found->is_null();
    if (missing && !required) {
        return {};
    }
    if (missing || !found->is_string() ||
        (required && found->get_ref<const std::string&>().empty()) ||
        code_points(found->get_ref<const std::string&>()) > max_chars) {
        if (required) {
            throw Invalid(name + " must be text of 1 to " + with_commas(max_chars) +
                          " characters.");
        }
        throw Invalid(name + " must be text of at most " + with_commas(max_chars) + " characters.");
    }
    const std::string& value = found->get_ref<const std::string&>();
    if (!is_unicode_text(value)) {
        throw Invalid(name + " must be valid Unicode text.");
    }
    return value;
}

const Json& live_openapi_paths() {
    static const Json paths = py::loads(embedded::agentapi_live_openapi).at("paths");
    return paths;
}

// --- LiveApi -----------------------------------------------------------------------------------

struct LiveApi::Context {
    Context(SettingsStore& settings, std::function<bool()> enabled, automation::EventBus& bus,
            automation::FilterLeases& leases, Executor& io, const Clock& clock)
        : settings(settings), enabled(std::move(enabled)), bus(bus), leases(leases), io(io),
          clock(clock) {}

    Json saved() const { return *settings.snapshot(); }

    // _LiveRoutes.failure: a refusal as its error, anything else logged by type alone (a message
    // can quote the request's text or a URL) and answered 503.
    ApiResponse failure(std::string_view route, std::exception_ptr error) const {
        try {
            std::rethrow_exception(error);
        } catch (const permissions::PermissionDenied& denied) {
            return error_response(403, "permission_denied", denied.message());
        } catch (const automation::ControlError& refused) {
            return error_response(control_status(refused.code()), refused.code(),
                                  refused.message());
        } catch (const Invalid& invalid) {
            return error_response(invalid.status(), invalid.code(), invalid.message());
        } catch (const std::exception& unexpected) {
            kLog.error("Live API {} failed: {}", route, error_type(unexpected));
        } catch (...) {
            kLog.error("Live API {} failed: {}", route, "unknown");
        }
        return error_response(503, "unavailable",
                              std::string(identity().product) +
                                  " couldn't do that right now. Try again.");
    }

    // _LiveRoutes.respond: `work`'s JSON answered with `status`; nullopt is an empty answer.
    void respond(const Responder& responder, std::string_view route, int status,
                 const std::function<std::optional<Json>()>& work) const {
        std::optional<Json> result;
        try {
            result = work();
        } catch (...) {
            send(responder, failure(route, std::current_exception()));
            return;
        }
        if (!result) {
            responder->send(status, {}, {});
            return;
        }
        send(responder, json_response(status, *result));
    }

    // Body reading that fails answers at once, before the work.
    std::optional<Json> body_or_answer(const net::HttpRequest& request, std::size_t limit,
                                       const Responder& responder, std::string_view route) const {
        try {
            return read_json_body(request, limit);
        } catch (...) {
            send(responder, failure(route, std::current_exception()));
            return std::nullopt;
        }
    }

    // The route's path id, as FastAPI checks it (1 to 64 code points) before the handler runs.
    static std::optional<std::string> path_id(const net::HttpRequest& request,
                                              std::string_view name, const Responder& responder) {
        std::string id = param(request, name);
        if (id.empty() || code_points(id) > MAX_ID_CHARS) {
            send(responder, invalid_parameters());
            return std::nullopt;
        }
        return id;
    }

    void track(const std::shared_ptr<EventStream>& stream) {
        std::lock_guard lock(mutex);
        std::erase_if(streams, [](const std::weak_ptr<EventStream>& each) {
            const auto alive = each.lock();
            return !alive || alive->ended();
        });
        streams.push_back(stream);
    }

    SettingsStore& settings;
    const std::function<bool()> enabled;
    automation::EventBus& bus;
    automation::FilterLeases& leases;
    Executor& io;
    const Clock& clock;

    std::mutex mutex;
    StreamOptions stream_options;
    std::vector<std::weak_ptr<EventStream>> streams;
};

LiveApi::LiveApi(SettingsStore& settings, std::function<bool()> enabled, automation::EventBus& bus,
                 automation::FilterLeases& leases, Executor& io, const Clock& clock)
    : context_(std::make_shared<Context>(settings, std::move(enabled), bus, leases, io, clock)) {}

LiveApi::~LiveApi() {
    close_streams();
}

void LiveApi::set_stream_options(StreamOptions options) {
    std::lock_guard lock(context_->mutex);
    context_->stream_options = options;
}

void LiveApi::close_streams() {
    std::vector<std::weak_ptr<EventStream>> streams;
    {
        std::lock_guard lock(context_->mutex);
        streams.swap(context_->streams);
    }
    for (const auto& weak : streams) {
        if (const auto stream = weak.lock()) {
            stream->close();
        }
    }
}

void LiveApi::drop_leases() {
    context_->leases.drop_all_leases();
}

void LiveApi::install(LiveRoutes& routes) {
    const std::shared_ptr<Context> live = context_;
    const std::function<bool()> listener = routes.enabled();
    // A stream runs while the live API and the listener it came through are both on.
    const auto streaming = [live, listener] {
        return (!live->enabled || live->enabled()) && (!listener || listener());
    };

    // --- state and events ---
    routes.route("GET", "/v1/live/state", [live](const net::HttpRequest&, Responder responder) {
        live->respond(responder, "state", 200, [&]() -> std::optional<Json> {
            Json state = automation::require_app_control()->state().to_dict();
            state["permissions"] = permissions::describe(live->saved());
            Json listeners = Json::object();
            listeners["events"] = live->bus.subscribers(SSE_LABEL);
            listeners["claude_channel"] = live->bus.subscribers(CHANNEL_LABEL);
            state["listeners"] = std::move(listeners);
            return state;
        });
    });

    routes.route(
        "GET", "/v1/events",
        [live, streaming](const net::HttpRequest& request, Responder responder) {
            // FastAPI's checks of the parameters, before the handler: types up to 2,000 characters,
            // since a whole number from 0, the headers up to 64.
            const std::optional<std::string> types = last_query(request.query, "types");
            const std::optional<std::string> since_text = last_query(request.query, "since");
            const std::optional<std::string> last_event_id = header_value(request, "last-event-id");
            const std::optional<std::string> client = header_value(request, client_header());
            const std::optional<std::string> legacy_client =
                header_value(request, legacy_client_header());
            std::optional<std::int64_t> since;
            bool valid = !types || code_points(*types) <= MAX_TYPES_CHARS;
            if (since_text) {
                const auto parsed = detail::lax_int(Json(*since_text));
                if (parsed) {
                    valid = valid && *parsed >= 0;
                    since = *parsed;
                } else if (parsed.error().type == "int_parsing_size" &&
                           py::strip(*since_text).find('-') == std::string::npos) {
                    // Python keeps any whole number; past int64 it is past every position.
                    since = std::numeric_limits<std::int64_t>::max();
                } else {
                    valid = false;
                }
            }
            for (const auto& header : {last_event_id, client, legacy_client}) {
                valid = valid && (!header || header->size() <= MAX_HEADER_CHARS);
            }
            if (!valid) {
                send(responder, invalid_parameters());
                return;
            }
            std::optional<std::vector<std::string>> wanted;
            std::optional<std::int64_t> resume;
            try {
                wanted = parse_types(types);
                resume = parse_last_event_id(
                    last_event_id ? std::optional(latin1_to_utf8(*last_event_id)) : std::nullopt);
                permissions::require(live->saved(), permissions::LIVE_EVENTS);
            } catch (...) {
                send(responder, live->failure("events", std::current_exception()));
                return;
            }
            // EventSource reconnects to the same URL, so the header is the newer position. Without
            // either, the stream starts at this request.
            if (!resume) {
                resume = since ? *since : live->bus.last_seq();
            }
            const std::string who =
                py::strip(latin1_to_utf8(client.value_or(legacy_client.value_or(""))));
            const std::string label(who == CHANNEL_CLIENT ? CHANNEL_LABEL : SSE_LABEL);
            std::shared_ptr<net::EventStream> out =
                responder->start_sse({{"x-accel-buffering", "no"},
                                      {"content-type", "text/event-stream; charset=utf-8"}});
            if (!out) {
                return;
            }
            StreamOptions options;
            {
                std::lock_guard lock(live->mutex);
                options = live->stream_options;
            }
            auto stream = std::make_shared<EventStream>(
                wanted, *resume, label, streaming,
                [live] { return permissions::granted(live->saved(), permissions::LIVE_EVENTS); },
                live->bus, live->io, live->clock, options);
            live->track(stream);
            stream->start(std::move(out));
        });

    // --- actions ---
    routes.route("GET", "/v1/actions", [live](const net::HttpRequest&, Responder responder) {
        live->respond(responder, "actions", 200, [&]() -> std::optional<Json> {
            Json out = Json::object();
            out["actions"] = automation::require_app_control()->list_actions();
            return out;
        });
    });

    routes.route("POST", "/v1/actions/{action_id}/run",
                 [live](const net::HttpRequest& request, Responder responder) {
                     const auto action_id = Context::path_id(request, "action_id", responder);
                     if (!action_id) {
                         return;
                     }
                     const auto body =
                         live->body_or_answer(request, MAX_RUN_BODY, responder, "run");
                     if (!body) {
                         return;
                     }
                     live->respond(responder, "run", 202, [&]() -> std::optional<Json> {
                         permissions::require(live->saved(), permissions::RUN_ACTIONS);
                         const std::string text =
                             string_field(*body, "text", true, MAX_ACTION_TEXT);
                         Json out = Json::object();
                         out["job_id"] =
                             automation::require_app_control()->run_action(*action_id, text, "api");
                         return out;
                     });
                 });

    // --- recording ---
    routes.route("POST", "/v1/recording/start",
                 [live](const net::HttpRequest& request, Responder responder) {
                     const auto body =
                         live->body_or_answer(request, MAX_SMALL_BODY, responder, "start");
                     if (!body) {
                         return;
                     }
                     live->respond(responder, "start", 202, [&]() -> std::optional<Json> {
                         permissions::require(live->saved(), permissions::CONTROL_RECORDING);
                         const std::string action_id =
                             string_field(*body, "action_id", false, MAX_ID_CHARS);
                         Json out = Json::object();
                         out["job_id"] = automation::require_app_control()->start_recording(
                             action_id.empty() ? std::nullopt : std::optional(action_id), "api");
                         return out;
                     });
                 });

    routes.route("POST", "/v1/recording/stop",
                 [live](const net::HttpRequest&, Responder responder) {
                     live->respond(responder, "stop", 202, [&]() -> std::optional<Json> {
                         permissions::require(live->saved(), permissions::CONTROL_RECORDING);
                         automation::require_app_control()->stop_recording();
                         return Json::object();
                     });
                 });

    routes.route("POST", "/v1/recording/cancel",
                 [live](const net::HttpRequest&, Responder responder) {
                     live->respond(responder, "cancel", 202, [&]() -> std::optional<Json> {
                         permissions::require(live->saved(), permissions::CONTROL_RECORDING);
                         automation::require_app_control()->cancel();
                         return Json::object();
                     });
                 });

    // --- filter leases ---
    routes.route(
        "POST", "/v1/filters", [live](const net::HttpRequest& request, Responder responder) {
            if (!peer_is_local(request.peer.address)) {
                // A filter's address is on this computer, which a Tailscale peer must not point
                // this computer's dictations at.
                send(responder,
                     live->failure("filters",
                                   std::make_exception_ptr(Invalid(
                                       "Only a program on this computer can lease a text filter.",
                                       403, "forbidden"))));
                return;
            }
            const auto body = live->body_or_answer(request, MAX_SMALL_BODY, responder, "filters");
            if (!body) {
                return;
            }
            live->respond(responder, "filters", 201, [&]() -> std::optional<Json> {
                permissions::require(live->saved(), permissions::TEXT_FILTERS);
                const std::string url = string_field(*body, "url", true, MAX_URL_CHARS);
                const std::string stage = string_field(*body, "stage", true, MAX_ID_CHARS);
                // _filter_options: only timeout_ms is taken over HTTP, and only a whole number.
                std::optional<Json> timeout_ms;
                if (const auto found = body->find("timeout_ms");
                    found != body->end() && !found->is_null()) {
                    if (!found->is_number_integer()) {
                        throw Invalid("timeout_ms must be a whole number of milliseconds.");
                    }
                    timeout_ms = *found;
                }
                try {
                    return live->leases.register_lease(url, stage, timeout_ms).to_json();
                } catch (const py::ValueError& refused) {
                    throw Invalid(refused.what());
                }
            });
        });

    routes.route("POST", "/v1/filters/{lease_id}/renew",
                 [live](const net::HttpRequest& request, Responder responder) {
                     const auto lease_id = Context::path_id(request, "lease_id", responder);
                     if (!lease_id) {
                         return;
                     }
                     live->respond(responder, "filters", 200, [&]() -> std::optional<Json> {
                         permissions::require(live->saved(), permissions::TEXT_FILTERS);
                         const auto renewed = live->leases.renew_lease(*lease_id);
                         if (!renewed) {
                             throw automation::ControlError(
                                 std::string(automation::NOT_FOUND),
                                 "That filter lease has expired. Create a new one.");
                         }
                         return renewed->to_json();
                     });
                 });

    routes.route("DELETE", "/v1/filters/{lease_id}",
                 [live](const net::HttpRequest& request, Responder responder) {
                     const auto lease_id = Context::path_id(request, "lease_id", responder);
                     if (!lease_id) {
                         return;
                     }
                     live->respond(responder, "filters", 204, [&]() -> std::optional<Json> {
                         permissions::require(live->saved(), permissions::TEXT_FILTERS);
                         if (!live->leases.drop_lease(*lease_id)) {
                             throw automation::ControlError(std::string(automation::NOT_FOUND),
                                                            "That filter lease has expired.");
                         }
                         return std::nullopt;
                     });
                 });

    // --- notices ---
    routes.route(
        "POST", "/v1/notify", [live](const net::HttpRequest& request, Responder responder) {
            const auto body = live->body_or_answer(request, MAX_SMALL_BODY, responder, "notify");
            if (!body) {
                return;
            }
            live->respond(responder, "notify", 202, [&]() -> std::optional<Json> {
                permissions::require(live->saved(), permissions::RUN_ACTIONS);
                const std::string text = string_field(*body, "text", true, MAX_NOTICE_TEXT);
                const std::string title = string_field(*body, "title", false, MAX_NOTICE_TITLE);
                if (py::strip(text).empty()) {
                    throw Invalid("text must be text of 1 to " + with_commas(MAX_NOTICE_TEXT) +
                                  " characters.");
                }
                automation::require_app_control()->notify(text, title);
                return Json::object();
            });
        });

    routes.add_openapi_paths(live_openapi_paths());
}

void mount_live_api(McpRuntime& runtime, LiveApi& api) {
    runtime.add_routes([&api](LiveRoutes& routes) { api.install(routes); });
    runtime.on_stopped([&api] { api.drop_leases(); });
}

} // namespace fv::agentapi
