#include "agentapi/mcp_events.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <system_error>
#include <typeinfo>

#include "agentapi/data/mcp_event_schemas.json.h"
#include "agentapi/detail/pydantic.h"
#include "agentapi/mcp_events_delivery.h"
#include "agentapi/mcp_events_registry.h"
#include "agentapi/mcp_server.h"
#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "automation/hooks_model.h"
#include "automation/permissions.h"
#include "automation/signing.h"
#include "base/base64.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/subscription.h"
#include "credentials/store.h"
#include "http/client.h"
#include "net/http_server.h"
#include "net/tailscale.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::agentapi {

namespace events_ext {

namespace {

const Json& schema_document() {
    static const Json document = py::loads(std::string(embedded::agentapi_mcp_event_schemas));
    return document;
}

bool is_public_type(std::string_view type) {
    const auto& types = automation::catalog::PUBLIC_TYPES();
    return std::ranges::find(types, type) != types.end();
}

bool is_unicode_text(std::string_view text) {
    return !detail::has_surrogate(text);
}

// What urlsplit gives that the checks here read: the scheme and the netloc.
struct UrlParts {
    std::string scheme;
    std::string netloc;
};

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

// urllib.parse.urlsplit's scheme and netloc, for a URL validate_url accepted (so its brackets
// are balanced and valid).
UrlParts split_url(std::string_view input) {
    while (!input.empty() && static_cast<unsigned char>(input.front()) <= 0x20) {
        input.remove_prefix(1);
    }
    std::string url;
    for (const char c : input) {
        if (c != '\t' && c != '\r' && c != '\n') {
            url += c;
        }
    }
    UrlParts parts;
    const auto colon = url.find(':');
    const auto scheme_char = [](char c) {
        return is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '-' || c == '.';
    };
    if (colon != std::string::npos && colon > 0 && is_ascii_alpha(url[0]) &&
        std::all_of(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(colon), scheme_char)) {
        parts.scheme = py::lower(url.substr(0, colon));
        url = url.substr(colon + 1);
    }
    if (url.starts_with("//")) {
        auto end = url.find_first_of("/?#", 2);
        if (end == std::string::npos) {
            end = url.size();
        }
        parts.netloc = url.substr(2, end - 2);
    }
    return parts;
}

// SplitResult._hostinfo: (host, port text), the port nullopt when empty.
std::pair<std::string_view, std::optional<std::string_view>> hostinfo(std::string_view netloc) {
    const auto at = netloc.rfind('@');
    const auto info = at == std::string_view::npos ? netloc : netloc.substr(at + 1);
    std::string_view host;
    std::string_view port;
    if (const auto open = info.find('['); open != std::string_view::npos) {
        const auto bracketed = info.substr(open + 1);
        const auto close = bracketed.find(']');
        host = bracketed.substr(0, close);
        const auto rest =
            close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
        const auto colon = rest.find(':');
        port = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
    } else {
        const auto colon = info.find(':');
        host = info.substr(0, colon);
        port = colon == std::string_view::npos ? std::string_view() : info.substr(colon + 1);
    }
    if (port.empty()) {
        return {host, std::nullopt};
    }
    return {host, port};
}

// SplitResult.hostname: lower case, the IPv6 zone keeping its case; nullopt when empty.
std::optional<std::string> hostname(std::string_view netloc) {
    const auto host = hostinfo(netloc).first;
    if (host.empty()) {
        return std::nullopt;
    }
    const auto percent = host.find('%');
    return py::lower(host.substr(0, percent)) + std::string(percent == std::string_view::npos
                                                                ? std::string_view()
                                                                : host.substr(percent));
}

// SplitResult.port: throws py::ValueError with urllib's message.
std::optional<int> port(std::string_view netloc) {
    const auto text = hostinfo(netloc).second;
    if (!text) {
        return std::nullopt;
    }
    if (!std::ranges::all_of(*text, is_ascii_digit)) {
        throw py::ValueError("Port could not be cast to integer value as " + py::repr(*text));
    }
    const auto digits = text->substr(std::min(text->find_first_not_of('0'), text->size()));
    if (digits.size() > 5) {
        throw py::ValueError("Port out of range 0-65535");
    }
    int value = 0;
    for (const char c : digits) {
        value = value * 10 + (c - '0');
    }
    if (value > 65535) {
        throw py::ValueError("Port out of range 0-65535");
    }
    return value;
}

const std::vector<net::IpAddress>& tailscale_service() {
    static const std::vector<net::IpAddress> addresses = [] {
        std::vector<net::IpAddress> out;
        for (const char* text : {"100.100.100.100", "fd7a:115c:a1e0::53"}) {
            out.push_back(*net::parse_address(text));
        }
        return out;
    }();
    return addresses;
}

// Python's == of two JSON values found in event data.
bool same(const Json* a, const Json& b) {
    return a != nullptr ? py::py_equal(*a, b) : b.is_null();
}

const Json* member(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &*found;
}

Json get(const Json& object, std::string_view key) {
    const Json* found = member(object, key);
    return found != nullptr ? *found : Json(nullptr);
}

// days_from_civil: the days from 1970-01-01 to a proleptic Gregorian date.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool is_leap(std::int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

} // namespace

bool decodes_as_utf8(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            length = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            length = 4;
            cp = c & 0x07;
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
        if ((length == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (length == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        i += length;
    }
    return true;
}

std::string secret_name_prefix() {
    return std::string(identity().env_prefix) + "MCP_EVENT_SECRET_";
}

std::string principal() {
    return py::lower(identity().data_dir_name) + "-mcp-token";
}

std::string legacy_principal() {
    return py::lower(legacy_identity().data_dir_name) + "-mcp-token";
}

std::string off_message() {
    return std::format("MCP Events is turned off in {} Settings > API & MCP.", identity().product);
}

EventsError too_many(std::size_t max) {
    Json data = Json::object();
    data["limit"] = "subscriptions";
    data["max"] = max;
    return EventsError(RESOURCE_EXHAUSTED,
                       std::format("{} keeps at most {} event subscriptions. Unsubscribe from "
                                   "one first.",
                                   identity().product, max),
                       std::move(data));
}

EventsError callback_failed(std::string_view reason) {
    Json data = Json::object();
    data["reason"] = std::string(reason);
    return EventsError(CALLBACK_ENDPOINT_ERROR,
                       std::format("The callback URL did not pass verification ({}).", reason),
                       std::move(data));
}

std::string error_name(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    if (dynamic_cast<const CredentialStoreError*>(&error) != nullptr) {
        return "CredentialStoreError";
    }
    if (dynamic_cast<const std::system_error*>(&error) != nullptr) {
        return "OSError";
    }
    return "Exception";
}

const Json& payload_schemas() {
    return schema_document().at("payload_schemas");
}

Json input_schema(std::string_view name) {
    Json properties = Json::object();
    const Json& by_type = schema_document().at("filters_by_type");
    if (const Json* keys = member(by_type, name)) {
        for (const Json& key : *keys) {
            properties[key.get<std::string>()] =
                schema_document().at("filter_schemas").at(key.get<std::string>());
        }
    }
    Json out = Json::object();
    out["type"] = "object";
    out["properties"] = std::move(properties);
    out["additionalProperties"] = false;
    return out;
}

Json descriptors() {
    Json out = Json::array();
    for (const std::string_view name : automation::catalog::PUBLIC_TYPES()) {
        Json item = Json::object();
        item["name"] = std::string(name);
        item["description"] = std::string(automation::catalog::describe_type(name).value_or(""));
        item["delivery"] = Json::array({"webhook"});
        item["inputSchema"] = input_schema(name);
        const Json* schema = member(payload_schemas(), name);
        item["payloadSchema"] = schema != nullptr ? *schema : Json{{"type", "object"}};
        out.push_back(std::move(item));
    }
    return out;
}

Json normalize_arguments(std::string_view name, const Json& arguments) {
    if (arguments.is_null()) {
        return Json::object();
    }
    if (!arguments.is_object()) {
        throw EventsError(INVALID_PARAMS, "arguments must be an object.");
    }
    std::vector<std::string> allowed;
    if (const Json* keys = member(schema_document().at("filters_by_type"), name)) {
        for (const Json& key : *keys) {
            allowed.push_back(key.get<std::string>());
        }
    }
    for (const auto& [key, value] : arguments.items()) {
        if (std::ranges::find(allowed, key) == allowed.end()) {
            const std::string shown = py::slice(key, 0, 40);
            Json data = Json::object();
            data["argument"] = shown;
            throw EventsError(
                INVALID_PARAMS,
                std::format("{} does not accept the argument {}.", name, py::repr(shown)),
                std::move(data));
        }
    }
    Json result = Json::object();
    if (const Json* value = member(arguments, "action_id")) {
        if (!value->is_string() || py::len(value->get_ref<const std::string&>()) < 1 ||
            py::len(value->get_ref<const std::string&>()) > 200 ||
            !is_unicode_text(value->get_ref<const std::string&>())) {
            throw EventsError(INVALID_PARAMS, "action_id must be an action id (1–200 characters).");
        }
        result["action_id"] = *value;
    }
    if (const Json* value = member(arguments, "modes")) {
        bool valid = value->is_array() && !value->empty();
        std::vector<std::string> modes;
        if (valid) {
            for (const Json& mode : *value) {
                if (!mode.is_string() ||
                    std::ranges::find(MODES, mode.get_ref<const std::string&>()) == MODES.end()) {
                    valid = false;
                    break;
                }
                modes.push_back(mode.get<std::string>());
            }
        }
        if (valid) {
            std::vector<std::string> distinct = modes;
            std::ranges::sort(distinct);
            valid = std::ranges::adjacent_find(distinct) == distinct.end();
            modes = std::move(distinct);
        }
        if (!valid) {
            std::string listed;
            for (const std::string_view mode : MODES) {
                listed += (listed.empty() ? "" : ", ") + std::string(mode);
            }
            throw EventsError(INVALID_PARAMS,
                              "modes must be a nonempty list of distinct values of " + listed +
                                  ".");
        }
        // Order carries no meaning, so it must not make a second subscription.
        Json sorted = Json::array();
        for (const std::string& mode : modes) {
            sorted.push_back(mode);
        }
        result["modes"] = std::move(sorted);
    }
    return result;
}

bool matches(const Json& arguments, const Json& data) {
    const Json action_id = get(arguments, "action_id");
    if (!action_id.is_null() && !same(member(data, "action_id"), action_id)) {
        return false;
    }
    const Json modes = get(arguments, "modes");
    if (!modes.is_null()) {
        const Json mode = get(data, "mode");
        bool found = false;
        for (const Json& wanted : modes) {
            found = found || py::py_equal(wanted, mode);
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

std::string event_name(const Json& value) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        throw EventsError(INVALID_PARAMS, "name must be an event name from events/list.");
    }
    const std::string& name = value.get_ref<const std::string&>();
    if (!is_public_type(name)) {
        Json data = Json::object();
        data["kind"] = "event";
        throw EventsError(NOT_FOUND,
                          std::format("{} has no event with that name.", identity().product),
                          std::move(data));
    }
    return name;
}

bool is_loopback_host(std::string_view host) {
    if (py::lower(host) == "localhost") {
        return true;
    }
    const auto address = net::parse_address(host);
    return address && net::is_loopback(*address);
}

std::string validate_callback_url(const Json& url) {
    if (!url.is_string() || !is_unicode_text(url.get_ref<const std::string&>())) {
        throw EventsError(INVALID_PARAMS, "delivery.url must be a URL.");
    }
    const std::string& text = url.get_ref<const std::string&>();
    UrlParts parts;
    try {
        automation::hooks::validate_url(text);
        parts = split_url(text);
        static_cast<void>(port(parts.netloc));
    } catch (const py::ValueError& error) {
        throw EventsError(INVALID_PARAMS, std::string("delivery.url: ") + error.what());
    }
    if (parts.netloc.find('@') != std::string::npos) {
        throw EventsError(INVALID_PARAMS, "delivery.url can't contain a user name or password.");
    }
    if (parts.scheme == "https" || is_loopback_host(hostname(parts.netloc).value_or(""))) {
        return text;
    }
    throw EventsError(INVALID_PARAMS,
                      "delivery.url must use https://; http:// is allowed only to this computer "
                      "(localhost or a loopback address).");
}

bool is_this_computer(const net::IpAddress& address) {
    if (std::ranges::find(tailscale_service(), address) != tailscale_service().end()) {
        return true;
    }
    return net::can_bind(address);
}

Network system_network(CancelToken cancel) {
    Network network;
    network.resolve = [cancel = std::move(cancel)](std::string_view host) {
        return net::resolve_host(host, cancel);
    };
    network.is_this_computer = [](const net::IpAddress& address) {
        return is_this_computer(address);
    };
    return network;
}

DestinationResult destination(std::string_view url, const Network& network) {
    const std::string host = hostname(split_url(url).netloc).value_or("");
    if (host.empty() || automation::hooks::url_is_private(url)) {
        return {Destination::Private, {}};
    }
    std::vector<net::IpAddress> found;
    if (const auto literal = net::parse_address(host)) {
        found.push_back(*literal);
    } else {
        auto resolved = network.resolve(host);
        if (!resolved) {
            return {Destination::Unresolved, {}};
        }
        found = std::move(*resolved);
    }
    std::vector<net::IpAddress> addresses;
    for (const auto& address : found) {
        addresses.push_back(net::unmap_v4(address));
    }
    if (addresses.empty()) {
        return {Destination::Unresolved, {}};
    }
    for (const auto& address : addresses) {
        if (network.is_this_computer(address) ||
            !(net::is_global(address) || net::is_tailscale_address(address))) {
            return {Destination::Private, {}};
        }
    }
    return {Destination::Public, addresses.front().to_string()};
}

std::string validate_secret(const Json& secret) {
    const EventsError refused(INVALID_PARAMS, "delivery.secret must be whsec_ followed by the "
                                              "base64 of 24 to 64 random bytes.");
    if (!secret.is_string()) {
        throw refused;
    }
    const std::string& text = secret.get_ref<const std::string&>();
    if (!text.starts_with(automation::signing::PREFIX)) {
        throw refused;
    }
    const auto key = b64decode(std::string_view(text).substr(automation::signing::PREFIX.size()));
    if (!key || key->size() < SECRET_MIN_BYTES || key->size() > SECRET_MAX_BYTES) {
        throw refused;
    }
    return text;
}

std::pair<std::string, std::string> delivery(const Json& value, bool subscribing) {
    if (!value.is_object()) {
        throw EventsError(INVALID_PARAMS, "delivery must be an object with mode and url.");
    }
    const Json mode = get(value, "mode");
    if (!(mode.is_string() && mode.get_ref<const std::string&>() == "webhook")) {
        Json data = Json::object();
        data["feature"] = "delivery.mode";
        data["value"] = mode.is_string() ? mode : Json(nullptr);
        throw EventsError(UNSUPPORTED,
                          std::format("{} delivers events by webhook only; use delivery.mode "
                                      "\"webhook\".",
                                      identity().product),
                          std::move(data));
    }
    if (!subscribing) {
        const Json url = get(value, "url");
        if (!url.is_string() || url.get_ref<const std::string&>().empty() ||
            !is_unicode_text(url.get_ref<const std::string&>())) {
            throw EventsError(INVALID_PARAMS, "delivery.url must be the subscribed URL.");
        }
        return {url.get<std::string>(), {}};
    }
    std::string url = validate_callback_url(get(value, "url"));
    return {std::move(url), validate_secret(get(value, "secret"))};
}

std::int64_t granted_ttl_ms(const Json& params) {
    const Json* value = member(params, "ttlMs");
    if (value == nullptr) {
        return DEFAULT_TTL_MS;
    }
    if (value->is_null()) {
        return MAX_TTL_MS;
    }
    const EventsError refused(INVALID_PARAMS,
                              "ttlMs must be a positive number of milliseconds or null.");
    if (value->is_boolean() || !value->is_number()) {
        throw refused;
    }
    if (value->is_number_unsigned()) {
        const auto ms = value->get<std::uint64_t>();
        if (ms == 0) {
            throw refused;
        }
        return static_cast<std::int64_t>(std::clamp<std::uint64_t>(
            ms, static_cast<std::uint64_t>(MIN_TTL_MS), static_cast<std::uint64_t>(MAX_TTL_MS)));
    }
    if (value->is_number_integer()) {
        const auto ms = value->get<std::int64_t>();
        if (ms <= 0) {
            throw refused;
        }
        return std::clamp(ms, MIN_TTL_MS, MAX_TTL_MS);
    }
    const double ms = value->get<double>();
    if (std::isnan(ms) || ms <= 0 || std::isinf(ms)) {
        throw refused;
    }
    return static_cast<std::int64_t>(
        std::min(std::max(ms, static_cast<double>(MIN_TTL_MS)), static_cast<double>(MAX_TTL_MS)));
}

std::string secret_name(std::string_view subscription) {
    return secret_name_prefix() + std::string(subscription);
}

std::string iso(double unix_seconds) {
    const auto whole = static_cast<std::int64_t>(std::trunc(unix_seconds));
    const std::chrono::sys_seconds at{std::chrono::seconds(whole)};
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", at);
}

std::optional<std::int64_t> parse_iso(const Json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    // _strptime's pattern for "%Y-%m-%dT%H:%M:%SZ", matched with IGNORECASE and to the end.
    static const py::Regex pattern(
        R"((?P<Y>\d\d\d\d)-(?P<m>1[0-2]|0[1-9]|[1-9])-(?P<d>3[01]|[12]\d|0[1-9]|[1-9]| [1-9]))"
        R"(T(?P<H>2[0-3]|[0-1]\d|\d):(?P<M>[0-5]\d|\d):(?P<S>6[0-1]|[0-5]\d|\d)Z)",
        py::IGNORECASE);
    const auto found = pattern.fullmatch(value.get_ref<const std::string&>());
    if (!found) {
        return std::nullopt;
    }
    const auto number = [&](std::string_view group) {
        return py::py_int(found->group(group)).value_or(-1);
    };
    const std::int64_t year = number("Y");
    const std::int64_t month = number("m");
    const std::int64_t day = number("d");
    const std::int64_t hour = number("H");
    const std::int64_t minute = number("M");
    const std::int64_t second = number("S");
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || second > 59 || hour < 0 ||
        minute < 0 || second < 0) {
        return std::nullopt;
    }
    const int limit = kDays[month - 1] + (month == 2 && is_leap(year) ? 1 : 0);
    if (day > limit) {
        return std::nullopt;
    }
    return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
           hour * 3600 + minute * 60 + second;
}

} // namespace events_ext

using namespace events_ext;

namespace {

constexpr log::Logger kLog{"services.agent_mcp.events_ext"};

double unix_seconds(const Clock& clock) {
    return std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
}

// automation.api.peer_is_local: the request came from a loopback address. Without one, the peer
// cannot be shown to be this computer.
bool peer_is_local(const net::HttpRequest* request) {
    if (request == nullptr) {
        return false;
    }
    const auto address = net::parse_address(net::strip_scope(request->peer.address));
    return address && net::is_loopback(net::unmap_v4(*address));
}

// RequestParams' _meta: absent or null, else an object whose progressToken, when present, is a
// string or an integer (as pydantic reads one).
void check_meta(const Json& params) {
    const auto found = params.find("_meta");
    if (found == params.end() || found->is_null()) {
        return;
    }
    const auto invalid = [] {
        throw RpcError(INVALID_PARAMS, "Invalid request parameters", Json(""));
    };
    if (!found->is_object()) {
        invalid();
    }
    const auto token = found->find("progressToken");
    if (token != found->end() && !token->is_string() && !detail::lax_int(*token)) {
        invalid();
    }
}

} // namespace

std::string subscription_id(std::string_view principal, std::string_view url, std::string_view name,
                            const Json& arguments) {
    Json key = Json::object();
    key["principal"] = std::string(principal);
    key["url"] = std::string(url);
    key["name"] = std::string(name);
    key["arguments"] = arguments;
    py::DumpOptions options;
    options.sort_keys = true;
    options.ensure_ascii = false;
    options.item_sep = ",";
    options.key_sep = ":";
    return "sub_" + sha256_hex(py::dumps(key, options)).substr(0, 32);
}

McpEvents::McpEvents(SettingsStore& settings, CredentialStore& credentials,
                     automation::EventBus& bus, std::filesystem::path registry_file,
                     std::shared_ptr<http::Client> client, const Clock& clock,
                     McpEventsOptions options)
    : settings_(settings), credentials_(credentials), bus_(bus), client_(std::move(client)),
      clock_(clock), options_(std::move(options)),
      registry_(std::make_unique<Registry>(std::move(registry_file), credentials, clock,
                                           options_.max_subscriptions)),
      verifier_(std::make_unique<CallbackVerifier>(*client_, clock)) {}

McpEvents::~McpEvents() {
    stop_delivery();
}

Json McpEvents::load_settings() const {
    try {
        const auto saved = settings_.snapshot();
        return saved ? *saved : Json::object();
    } catch (const std::exception& error) {
        kLog.warning("Could not read settings for MCP Events: {}", error_name(error));
        return Json::object();
    }
}

bool McpEvents::delivery_allowed(const Json& saved, std::string_view origin) const {
    // Checked when an event is queued and again before every try; a Tailscale peer's
    // subscription needs Tailscale access on too, so it never outlives the access that made it.
    return keys::mcp_enabled.read(saved) && keys::mcp_events_enabled.read(saved) &&
           automation::permissions::granted(saved, automation::permissions::LIVE_EVENTS) &&
           (origin == ORIGIN_LOCAL || keys::mcp_tailscale_enabled.read(saved));
}

bool McpEvents::delivering() const {
    std::lock_guard lock(state_mutex_);
    return bus_subscription_ && !bus_subscription_->closed();
}

Registry& McpEvents::registry() const {
    return *registry_;
}

std::size_t McpEvents::pending() const {
    std::shared_ptr<Deliverer> deliverer;
    {
        std::lock_guard lock(state_mutex_);
        deliverer = deliverer_;
    }
    return deliverer ? deliverer->pending() : 0;
}

void McpEvents::sync() {
    bool started = false;
    {
        std::lock_guard lock(state_mutex_);
        started = started_;
    }
    const bool want = started && keys::mcp_events_enabled.read(load_settings());
    if (started) {
        // Forgets what expired meanwhile, with the credential store's I/O on a worker.
        registry_->prune_later();
    }
    std::shared_ptr<automation::EventSubscription> old_subscription;
    std::shared_ptr<CallbackGuard> old_guard;
    std::shared_ptr<Deliverer> old_deliverer;
    {
        std::lock_guard lock(state_mutex_);
        const bool delivering = bus_subscription_ && !bus_subscription_->closed();
        if (want && !delivering) {
            if (!deliverer_) {
                deliverer_ = std::make_shared<Deliverer>(
                    [this](const Job& job, const CancelToken& cancel) {
                        return deliver(job, cancel);
                    },
                    clock_);
            }
            old_subscription = std::move(bus_subscription_);
            old_guard = std::move(bus_guard_);
            auto guard = std::make_shared<CallbackGuard>();
            bus_guard_ = guard;
            bus_subscription_ = bus_.subscribe(
                [this, guard](const automation::Event& event) {
                    guard->run([&] { on_event(event); });
                },
                std::nullopt, std::string(BUS_LABEL));
        } else if (!want) {
            old_subscription = std::move(bus_subscription_);
            old_guard = std::move(bus_guard_);
            old_deliverer = std::move(deliverer_);
        }
    }
    if (old_subscription) {
        old_subscription->close();
    }
    if (old_guard) {
        old_guard->close();
    }
    if (old_deliverer) {
        old_deliverer->close();
    }
}

void McpEvents::start_delivery() {
    {
        std::lock_guard lock(state_mutex_);
        started_ = true;
    }
    sync();
}

void McpEvents::stop_delivery() {
    {
        std::lock_guard lock(state_mutex_);
        started_ = false;
    }
    sync();
}

void McpEvents::reload() {
    bool started = false;
    {
        std::lock_guard lock(state_mutex_);
        started = started_;
    }
    if (started) {
        sync();
    }
}

std::size_t McpEvents::subscription_count() const {
    registry_->prune_later();
    return registry_->active().size();
}

std::size_t McpEvents::clear_subscriptions() {
    const std::size_t count = registry_->clear();
    if (count != 0) {
        kLog.info("Cleared {} MCP Events subscription(s)", count);
    }
    return count;
}

void McpEvents::on_event(const automation::Event& event) {
    // Who gets the event is decided here, as it is handed out: one published while a
    // subscription's access was off never goes to it, even when access returns before a worker
    // would send it.
    if (!payload_schemas().contains(event.type)) {
        return;
    }
    auto targets = registry_->matching(event.type, event.data);
    if (targets.empty()) {
        return;
    }
    std::shared_ptr<Deliverer> deliverer;
    {
        std::lock_guard lock(state_mutex_);
        deliverer = deliverer_;
    }
    if (!deliverer) {
        return;
    }
    const Json saved = load_settings();
    std::erase_if(targets, [&](const EventSubscription& subscription) {
        return !delivery_allowed(saved, subscription.origin);
    });
    if (targets.empty()) {
        return;
    }
    const auto body = std::make_shared<const std::string>(event_body(event));
    for (const EventSubscription& subscription : targets) {
        if (body->size() > MAX_PAYLOAD_BYTES) {
            registry_->record(subscription.id, "payload_too_large");
        } else if (!deliverer->push(subscription.id, event.id, event.type, body)) {
            registry_->record(subscription.id, "throttled");
        }
    }
}

DestinationResult McpEvents::peer_destination(std::string_view url) const {
    return options_.destination ? options_.destination(url) : destination(url, options_.network);
}

std::string McpEvents::require_reachable_by_peer(std::string_view url) const {
    const DestinationResult result = peer_destination(url);
    if (result.destination == Destination::Private) {
        throw EventsError(INVALID_PARAMS,
                          "Subscribing from another computer, delivery.url can't be an address "
                          "on this computer or its local network.");
    }
    if (result.destination == Destination::Unresolved) {
        throw callback_failed(PEER_VERIFICATION_FAILED);
    }
    return result.address;
}

std::optional<double> McpEvents::deliver(const Job& job, const CancelToken& cancel) {
    if (options_.before_try) {
        options_.before_try();
    }
    struct After {
        const std::function<void()>& hook;
        ~After() {
            if (hook) {
                hook();
            }
        }
    } after{options_.after_try};
    const auto subscription = registry_->get(job.subscription_id);
    if (!subscription || !delivery_allowed(load_settings(), subscription->origin)) {
        return std::nullopt;
    }
    const bool peer = subscription->origin != ORIGIN_LOCAL;
    std::optional<std::string> address;
    if (peer) {
        // Checked again for each try (the name may resolve here by now), and the try goes to
        // the address checked, never to a second lookup's.
        const DestinationResult result = peer_destination(subscription->url);
        if (result.destination != Destination::Public) {
            registry_->record(subscription->id, result.destination == Destination::Private
                                                    ? std::string_view("address_refused")
                                                    : PEER_DELIVERY_FAILED);
            return std::nullopt;
        }
        address = result.address;
    }
    const auto keys = registry_->signing_secrets(subscription->id);
    if (keys.empty()) {
        registry_->record(subscription->id, "secret_missing");
        return std::nullopt;
    }
    const auto timestamp = static_cast<std::int64_t>(unix_seconds(clock_));
    const Reply reply =
        post(*client_, subscription->url, *job.body,
             signed_headers(keys, job.event_id, *job.body, subscription->id, timestamp),
             REQUEST_TIMEOUT_S, address, cancel);
    if (cancel.cancelled()) {
        return std::nullopt;
    }
    kLog.debug("MCP Events {} to {}: {}", job.event_type, subscription->id,
               reply.status ? std::to_string(*reply.status) : reply.category);
    if (reply.ok()) {
        registry_->record(subscription->id);
        return std::nullopt;
    }
    // Any other answer, 410 Gone included, fails this delivery only: the subscription ends when
    // it is unsubscribed, expires or is cleared.
    registry_->record(subscription->id, peer ? PEER_DELIVERY_FAILED : reply.category);
    if (!reply.retryable() || job.attempt > static_cast<int>(options_.retry_delays_s.size())) {
        return std::nullopt;
    }
    double delay = options_.retry_delays_s[static_cast<std::size_t>(job.attempt - 1)];
    if (reply.retry_after) {
        delay = std::max(delay, std::min(*reply.retry_after, MAX_RETRY_AFTER_S));
    }
    return delay;
}

namespace {

void require_access(const Json& saved) {
    if (!keys::mcp_events_enabled.read(saved)) {
        Json data = Json::object();
        data["reason"] = "disabled";
        throw EventsError(FORBIDDEN, off_message(), std::move(data));
    }
    if (!automation::permissions::granted(saved, automation::permissions::LIVE_EVENTS)) {
        Json data = Json::object();
        data["reason"] = "permission_denied";
        throw EventsError(FORBIDDEN,
                          automation::permissions::PermissionDenied(
                              std::string(automation::permissions::LIVE_EVENTS))
                              .message(),
                          std::move(data));
    }
}

const Json& field(const Json& params, std::string_view key) {
    static const Json null = nullptr;
    if (!params.is_object()) {
        return null;
    }
    const auto found = params.find(std::string(key));
    return found == params.end() ? null : *found;
}

} // namespace

Json McpEvents::list_events(const Json& /*params*/, const Json& saved,
                            std::string_view /*origin*/) {
    // One page holds every event, so there is no nextCursor.
    require_access(saved);
    Json out = Json::object();
    out["events"] = descriptors();
    return out;
}

Json McpEvents::subscribe(const Json& params, const Json& saved, std::string_view origin) {
    require_access(saved);
    const std::string name = event_name(field(params, "name"));
    const Json arguments = normalize_arguments(name, field(params, "arguments"));
    const auto [url, secret] = delivery(field(params, "delivery"), true);
    std::optional<std::string> peer_address;
    if (origin != ORIGIN_LOCAL) {
        peer_address = require_reachable_by_peer(url);
    }
    const std::int64_t ttl_ms = granted_ttl_ms(params);
    const std::string id = subscription_id(options_.principal, url, name, arguments);
    registry_->prune();
    const auto existing = registry_->get(id);
    if (!existing && registry_->active().size() >= options_.max_subscriptions) {
        throw too_many(options_.max_subscriptions);
    }
    verifier_->verify(id, url, secret, peer_address);
    const double now = unix_seconds(clock_);
    EventSubscription subscription;
    subscription.id = id;
    subscription.name = name;
    subscription.arguments = arguments;
    subscription.url = url;
    subscription.refresh_before =
        static_cast<std::int64_t>(now + static_cast<double>(ttl_ms) / 1000.0);
    subscription.created_at = existing ? existing->created_at : iso(now);
    subscription.origin = std::string(origin);
    bool created = false;
    try {
        created = registry_->upsert(subscription, secret);
    } catch (const CredentialStoreError&) {
        throw EventsError(INTERNAL_ERROR,
                          std::format("{} could not save the subscription's secret. Unlock the "
                                      "system credential store, then retry.",
                                      identity().product));
    } catch (const std::system_error&) {
        throw EventsError(INTERNAL_ERROR,
                          std::format("{} could not save the subscription.", identity().product));
    }
    kLog.info("MCP Events subscription {} {} for {} from {}", id, created ? "created" : "refreshed",
              name, origin);
    Json out = Json::object();
    out["id"] = id;
    out["refreshBefore"] = iso(static_cast<double>(subscription.refresh_before));
    // Not replayable: events live only in a short in-memory buffer.
    out["cursor"] = nullptr;
    out["truncated"] = false;
    out["deliveryStatus"] =
        registry_->delivery_status(id, delivering() && delivery_allowed(saved, origin));
    return out;
}

Json McpEvents::unsubscribe(const Json& params, const Json& saved, std::string_view /*origin*/) {
    // Idempotent, and keyed like subscribe, never by id.
    require_access(saved);
    const std::string name = event_name(field(params, "name"));
    const Json arguments = normalize_arguments(name, field(params, "arguments"));
    const auto [url, unused] = delivery(field(params, "delivery"), false);
    const std::string id = subscription_id(options_.principal, url, name, arguments);
    if (registry_->remove(id)) {
        kLog.info("MCP Events subscription {} removed", id);
    }
    return Json::object();
}

Json McpEvents::handle(const McpRequest& request, Method method) {
    Json params = Json::object();
    if (request.params.is_object()) {
        check_meta(request.params);
        for (const auto& [key, value] : request.params.items()) {
            if (key != "_meta") {
                params[key] = value;
            }
        }
    }
    const std::string_view origin = peer_is_local(request.http) ? ORIGIN_LOCAL : ORIGIN_TAILSCALE;
    try {
        return (this->*method)(params, load_settings(), origin);
    } catch (const EventsError& error) {
        throw RpcError(error.code(), error.what(), error.data());
    } catch (const std::exception& error) {
        kLog.warning("An MCP Events request failed: {}", error_name(error));
        throw RpcError(INTERNAL_ERROR,
                       std::format("{} could not complete the request.", identity().product));
    }
}

void McpEvents::install(McpServer& server) {
    // The capability goes in through a hook because the SDK's ServerCapabilities has no events
    // field; it is read per request, so turning MCP Events on or off shows on the next
    // discovery.
    const std::pair<const char*, Method> methods[] = {
        {"events/list", &McpEvents::list_events},
        {"events/subscribe", &McpEvents::subscribe},
        {"events/unsubscribe", &McpEvents::unsubscribe},
    };
    for (const auto& [name, method] : methods) {
        server.register_method(
            name, [this, method](const McpRequest& request) { return handle(request, method); });
    }
    server.add_capability_hook([this](Json& capabilities) {
        if (keys::mcp_events_enabled.read(load_settings())) {
            capabilities["events"] = Json::object();
        }
    });
}

} // namespace fv::agentapi
