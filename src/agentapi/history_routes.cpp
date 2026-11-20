#include "agentapi/history_routes.h"

#include "agentapi/federation.h"

#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <utility>

#include "agentapi/data/openapi.json.h"
#include "agentapi/detail/query_params.h"
#include "agentapi/detail/standalone_history.h"
#include "agentapi/history_models.h"
#include "agentapi/history_store.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "sql/db.h"

namespace fv::agentapi {

namespace {

constexpr log::Logger kLog{"services.agent_api.app"};

constexpr std::size_t kTokenMinChars = 32;
constexpr std::size_t kTokenMaxChars = 512;

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// secrets.compare_digest: the time depends on the lengths only.
bool same_bytes(std::string_view a, std::string_view b) {
    unsigned char difference = a.size() == b.size() ? 0 : 1;
    const std::size_t length = a.size() > b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < length; ++i) {
        const auto x = static_cast<unsigned char>(i < a.size() ? a[i] : 0);
        const auto y = static_cast<unsigned char>(i < b.size() ? b[i] : 0);
        difference = static_cast<unsigned char>(difference | (x ^ y));
    }
    return difference == 0;
}

// urllib.parse.quote(text, safe): UTF-8 bytes, ASCII letters, digits, "_.-~" and `safe` kept.
std::string quote(std::string_view text, std::string_view safe) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool keep = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                          (byte >= '0' && byte <= '9') || c == '_' || c == '.' || c == '-' ||
                          c == '~' || (byte < 0x80 && safe.find(c) != std::string_view::npos);
        if (keep) {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0x0f];
        }
    }
    return out;
}

// urllib.parse.urlencode of the decoded pairs (quote_plus): the query string a redirect keeps.
std::string urlencode(const net::Query& query) {
    std::string out;
    for (const auto& [name, value] : query) {
        if (!out.empty()) {
            out += '&';
        }
        const auto plus = [](std::string_view text) {
            std::string quoted = quote(text, " ");
            for (char& c : quoted) {
                if (c == ' ') {
                    c = '+';
                }
            }
            return quoted;
        };
        out += plus(name) + "=" + plus(value);
    }
    return out;
}

enum class Route {
    Openapi,
    Status,
    Transcriptions,
    Transcription,
    Meetings,
    Meeting,
    Segments,
    Segment,
    Insights,
    Search,
};

struct Match {
    Route route;
    std::vector<std::string> params;
};

// Starlette's path matching: each "{param}" is one non-empty segment ([^/]+).
std::optional<Match> match_route(std::string_view path) {
    if (path == "/openapi.json") {
        return Match{Route::Openapi, {}};
    }
    if (!path.starts_with("/v1/")) {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    std::string_view rest = path.substr(4);
    while (true) {
        const std::size_t slash = rest.find('/');
        parts.emplace_back(rest.substr(0, slash));
        if (slash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    for (const auto& part : parts) {
        if (part.empty()) {
            return std::nullopt;
        }
    }
    const auto& head = parts[0];
    if (parts.size() == 1) {
        if (head == "status") {
            return Match{Route::Status, {}};
        }
        if (head == "transcriptions") {
            return Match{Route::Transcriptions, {}};
        }
        if (head == "meetings") {
            return Match{Route::Meetings, {}};
        }
        if (head == "search") {
            return Match{Route::Search, {}};
        }
        return std::nullopt;
    }
    if (head == "transcriptions" && parts.size() == 2) {
        return Match{Route::Transcription, {parts[1]}};
    }
    if (head != "meetings") {
        return std::nullopt;
    }
    if (parts.size() == 2) {
        return Match{Route::Meeting, {parts[1]}};
    }
    if (parts.size() == 3 && parts[2] == "segments") {
        return Match{Route::Segments, {parts[1]}};
    }
    if (parts.size() == 3 && parts[2] == "insights") {
        return Match{Route::Insights, {parts[1]}};
    }
    if (parts.size() == 4 && parts[2] == "segments") {
        return Match{Route::Segment, {parts[1], parts[3]}};
    }
    return std::nullopt;
}

// FastAPI's validation of one request's query parameters: every failure is remembered, and
// the request is refused with 422 once all were read.
class Params {
public:
    explicit Params(const net::Query& query) : query_(query) {}

    int errors() const { return errors_; }

    // str with min_length and max_length in characters; absent is nullopt.
    std::optional<std::string> text(std::string_view name, std::size_t min_length = 0,
                                    std::size_t max_length = static_cast<std::size_t>(-1)) {
        std::optional<std::string> value = detail::last_value(query_, name);
        if (value) {
            const std::size_t length = py::len(*value);
            if (length < min_length || length > max_length) {
                ++errors_;
                return std::nullopt;
            }
        }
        return value;
    }

    // A Literal: one of `options` exactly, the first when absent.
    std::string choice(std::string_view name, std::initializer_list<std::string_view> options) {
        const std::optional<std::string> value = detail::last_value(query_, name);
        if (!value) {
            return std::string(*options.begin());
        }
        for (const std::string_view option : options) {
            if (*value == option) {
                return *value;
            }
        }
        ++errors_;
        return std::string(*options.begin());
    }

    bool flag(std::string_view name) {
        const std::optional<std::string> value = detail::last_value(query_, name);
        if (!value) {
            return false;
        }
        const std::optional<bool> parsed = detail::parse_bool(*value);
        if (!parsed) {
            ++errors_;
            return false;
        }
        return *parsed;
    }

    // Limit: int 1..100, 20 when absent.
    std::int64_t limit() {
        const std::optional<std::string> value = detail::last_value(query_, "limit");
        if (!value) {
            return 20;
        }
        const std::optional<std::int64_t> parsed = detail::parse_int(*value);
        if (!parsed || *parsed < 1 || *parsed > 100) {
            ++errors_;
            return 20;
        }
        return *parsed;
    }

    // float >= 0, finite.
    std::optional<double> seconds(std::string_view name) {
        const std::optional<std::string> value = detail::last_value(query_, name);
        if (!value) {
            return std::nullopt;
        }
        const std::optional<double> parsed = detail::parse_float(*value);
        if (!parsed || !std::isfinite(*parsed) || !(*parsed >= 0.0)) {
            ++errors_;
            return std::nullopt;
        }
        return parsed;
    }

    std::optional<py::DateTime> datetime(std::string_view name) {
        const std::optional<std::string> value = detail::last_value(query_, name);
        if (!value) {
            return std::nullopt;
        }
        std::optional<py::DateTime> parsed = detail::parse_datetime(*value);
        if (!parsed) {
            ++errors_;
        }
        return parsed;
    }

private:
    const net::Query& query_;
    int errors_ = 0;
};

// app.py _filters, the dependency of the list routes.
struct Filters {
    bool include_remote = false;
    bool include_clients = false;
    std::optional<std::string> device_id;
    std::optional<std::string> since;
    std::optional<std::string> before;
};

// The dependency's parameters, then its body when they are valid (FastAPI runs it before it
// reads the route's own parameters, so its 400s come before their 422s).
std::optional<Filters> read_filters(Params& params) {
    const int errors = params.errors();
    Filters filters;
    filters.include_remote = params.flag("include_remote");
    filters.include_clients = params.flag("include_clients");
    filters.device_id = params.text("device_id", 1, 64);
    const std::optional<py::DateTime> since = params.datetime("since");
    const std::optional<py::DateTime> before = params.datetime("before");
    if (params.errors() != errors) {
        return std::nullopt;
    }
    const auto utc = [](const std::optional<py::DateTime>& value) -> std::optional<std::string> {
        if (!value) {
            return std::nullopt;
        }
        if (!value->utc_offset_us) {
            throw InvalidQuery(
                "Date filters must include a timezone, for example 2026-01-01T00:00:00Z.");
        }
        return detail::to_utc(*value).isoformat();
    };
    filters.since = utc(since);
    filters.before = utc(before);
    if (filters.since && filters.before && *filters.since >= *filters.before) {
        throw InvalidQuery("since must be earlier than before.");
    }
    return filters;
}

Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

ApiResponse invalid_parameters() {
    return error_response(422, "invalid_parameters",
                          "Invalid request parameters. See /openapi.json for the contract.");
}

} // namespace

const std::string& openapi_document() {
    static const std::string document = [] {
        Json schema = py::loads(embedded::agentapi_openapi);
        schema["info"]["title"] = std::string(identity().product) + " History API";
        return render_json(schema);
    }();
    return document;
}

void check_token(std::string_view token) {
    bool valid = token.size() >= kTokenMinChars && token.size() <= kTokenMaxChars;
    for (const char c : token) {
        // ASCII, printable and not whitespace: '!' to '~'.
        valid = valid && c >= '!' && c <= '~';
    }
    if (!valid) {
        throw std::invalid_argument(
            std::string(identity().env_prefix) +
            "API_TOKEN must be 32–512 printable ASCII characters without whitespace.");
    }
}

HistoryApi::HistoryApi(std::filesystem::path database, std::string token,
                       std::vector<std::string> allowed_hosts, const Clock& clock,
                       std::shared_ptr<remote::ClientHistorySource> clients, bool local_only)
    : token_(std::move(token)), local_only_(local_only) {
    check_token(token_);
    store_ = std::make_unique<HistoryStore>(std::move(database));
    history_ = std::make_unique<detail::StandaloneHistory>(clock);
    if (clients && !local_only_) {
        federation_ = std::make_unique<FederatedHistory>(std::move(clients), clock);
    }
    trusted_hosts_ = {"127.0.0.1", "localhost"};
    for (auto& host : allowed_hosts) {
        trusted_hosts_.push_back(std::move(host));
    }
}

HistoryApi::~HistoryApi() = default;

HistoryStore& HistoryApi::store() {
    return *store_;
}

std::optional<ApiResponse> HistoryApi::refusal(const net::HttpRequest& request) const {
    // Native agent clients send no browser Origin. A Host outside the trusted names is refused
    // so that a web page cannot reach this service through DNS rebinding.
    const std::string_view host_header = net::header(request.headers, "host").value_or("");
    const std::string host = lower_ascii(host_header.substr(0, host_header.find(':')));
    bool trusted = false;
    for (const auto& name : trusted_hosts_) {
        trusted = trusted || host == name;
    }
    if (!trusted || net::header(request.headers, "origin")) {
        return error_response(403, "forbidden",
                              "Only configured, non-browser clients are supported.");
    }
    const std::string_view authorization =
        net::header(request.headers, "authorization").value_or("");
    const std::size_t space = authorization.find(' ');
    const std::string_view scheme = authorization.substr(0, space);
    const std::string_view supplied =
        space == std::string_view::npos ? std::string_view() : authorization.substr(space + 1);
    if (lower_ascii(scheme) != "bearer" || !same_bytes(supplied, token_)) {
        return error_response(401, "unauthorized", "A valid bearer token is required.",
                              {{"WWW-Authenticate", "Bearer"}});
    }
    return std::nullopt;
}

ApiResponse HistoryApi::handle(const net::HttpRequest& request) {
    ApiResponse response;
    try {
        std::optional<ApiResponse> refused = refusal(request);
        response = refused ? std::move(*refused) : route(request);
    } catch (const std::exception& error) {
        kLog.error_with(error, "History API request failed");
        return internal_error_response();
    }
    response.headers.emplace_back("cache-control", "no-store");
    response.headers.emplace_back("x-content-type-options", "nosniff");
    return response;
}

ApiResponse HistoryApi::route(const net::HttpRequest& request) {
    const std::optional<Match> match = match_route(request.path);
    if (!match) {
        // Starlette's redirect_slashes: the same path with its trailing slashes removed, or one
        // added, when that is a route.
        if (request.path != "/") {
            std::string other = request.path;
            if (other.ends_with('/')) {
                while (other.ends_with('/')) {
                    other.pop_back();
                }
            } else {
                other += '/';
            }
            if (match_route(other)) {
                std::string location =
                    std::string(request.tls ? "https" : "http") + "://" +
                    std::string(net::header(request.headers, "host").value_or("")) + other;
                const std::string query = urlencode(request.query);
                if (!query.empty()) {
                    location += "?" + query;
                }
                ApiResponse redirect;
                redirect.status = 307;
                redirect.headers.emplace_back("location", quote(location, ":/%#?=@[]!$&'()*+,;"));
                return redirect;
            }
        }
        return error_response(404, "not_found", "Not Found");
    }
    if (request.method != "GET") {
        return error_response(405, "method_not_allowed", "Method Not Allowed", {{"Allow", "GET"}});
    }

    try {
        Params params(request.query);
        if (local_only_ && (params.flag("include_remote") || params.flag("include_clients") ||
                            params.text("device_id", 0, 256))) {
            return error_response(403, "forbidden", "This session only shares local history.");
        }
        HistoryStore& store = *store_;
        switch (match->route) {
        case Route::Openapi: {
            ApiResponse response;
            response.headers.emplace_back("content-type", "application/json");
            response.body = openapi_document();
            return response;
        }
        case Route::Status:
            store.check();
            return json_response(
                200, ApiStatus{federation_ ? federation_->clients() : std::vector<ClientStatus>{}}
                         .to_json());
        case Route::Transcriptions:
        case Route::Meetings: {
            const std::optional<Filters> filters = read_filters(params);
            const std::optional<std::string> q = params.text("q", 1, 500);
            const std::int64_t limit = params.limit();
            const std::optional<std::string> cursor = params.text("cursor", 0, 2048);
            if (!filters || params.errors() > 0) {
                return invalid_parameters();
            }
            const bool meetings = match->route == Route::Meetings;
            Json scope = Json::object();
            scope["q"] = optional_json(q);
            scope["include_remote"] = filters->include_remote;
            scope["since"] = optional_json(filters->since);
            scope["before"] = optional_json(filters->before);
            const auto read = [&](const std::optional<std::string>& page_cursor,
                                  std::int64_t page_limit,
                                  const std::optional<std::string>& origin) {
                const ListQuery query{q,
                                      page_limit,
                                      page_cursor,
                                      filters->include_remote,
                                      filters->since,
                                      filters->before,
                                      origin};
                return meetings ? store.meetings(query) : store.transcriptions(query);
            };
            if (federation_ && (filters->include_clients || filters->device_id)) {
                return json_response(
                    200, federation_->page(meetings ? "meetings" : "transcriptions", scope,
                                           filters->device_id, filters->include_remote, cursor,
                                           limit, read));
            }
            return json_response(200,
                                 history_->page(meetings ? "meetings" : "transcriptions", scope,
                                                filters->include_clients, filters->device_id,
                                                filters->include_remote, cursor, limit, read));
        }
        case Route::Search: {
            const std::optional<Filters> filters = read_filters(params);
            // q is required here.
            const std::optional<std::string> q = params.text("q", 1, 500);
            const std::string kind = params.choice("kind", {"all", "transcription", "meeting"});
            const std::int64_t limit = params.limit();
            const std::optional<std::string> cursor = params.text("cursor", 0, 2048);
            if (!filters || params.errors() > 0 || !q) {
                return invalid_parameters();
            }
            const std::string& chosen_kind = kind;
            Json scope = Json::object();
            scope["q"] = *q;
            scope["kind"] = chosen_kind;
            scope["include_remote"] = filters->include_remote;
            scope["since"] = optional_json(filters->since);
            scope["before"] = optional_json(filters->before);
            const auto read = [&](const std::optional<std::string>& page_cursor,
                                  std::int64_t page_limit,
                                  const std::optional<std::string>& origin) {
                return store.search(SearchQuery{*q, chosen_kind, page_limit, page_cursor,
                                                filters->include_remote, filters->since,
                                                filters->before, origin});
            };
            if (federation_ && (filters->include_clients || filters->device_id)) {
                return json_response(200, federation_->page("search", scope, filters->device_id,
                                                            filters->include_remote, cursor, limit,
                                                            read));
            }
            return json_response(200, history_->page("search", scope, filters->include_clients,
                                                     filters->device_id, filters->include_remote,
                                                     cursor, limit, read));
        }
        case Route::Transcription:
        case Route::Meeting:
        case Route::Insights: {
            const bool include_remote = params.flag("include_remote");
            const std::optional<std::string> device_id = params.text("device_id");
            if (params.errors() > 0) {
                return invalid_parameters();
            }
            const std::string& id = match->params[0];
            const auto read = [&](bool remote) {
                if (match->route == Route::Transcription) {
                    return store.transcription(id, remote);
                }
                if (match->route == Route::Meeting) {
                    return store.meeting(id, remote);
                }
                return store.insights(id, remote);
            };
            if (!device_id) {
                return json_response(200, read(include_remote));
            }
            std::exception_ptr unavailable;
            if (federation_) {
                try {
                    if (match->route == Route::Transcription) {
                        return json_response(200, federation_->query(*device_id, "transcription",
                                                                     Json{{"record_id", id}}));
                    }
                    const auto op = match->route == Route::Meeting ? "meeting" : "insights";
                    return json_response(
                        200, federation_->query(*device_id, op, Json{{"meeting_id", id}}));
                } catch (const remote::ClientUnavailable&) {
                    unavailable = std::current_exception();
                }
            }
            if (include_remote) {
                try {
                    if (match->route == Route::Insights) {
                        if (store.meeting(id, true).value("origin_device_id", Json()) ==
                            Json(*device_id)) {
                            return json_response(200, read(true));
                        }
                    } else {
                        Json result = read(true);
                        if (result.value("origin_device_id", Json()) == Json(*device_id)) {
                            return json_response(200, result);
                        }
                    }
                } catch (const NotFound&) {
                }
            }
            if (unavailable) {
                std::rethrow_exception(unavailable);
            }
            throw detail::ClientUnavailable();
        }
        case Route::Segments:
        case Route::Segment: {
            const bool segments = match->route == Route::Segments;
            std::int64_t limit = 20;
            std::optional<std::string> cursor;
            if (segments) {
                limit = params.limit();
                cursor = params.text("cursor", 0, 2048);
            }
            const bool include_remote = params.flag("include_remote");
            const std::optional<std::string> device_id = params.text("device_id");
            std::optional<double> start_s;
            std::optional<double> end_s;
            if (segments) {
                start_s = params.seconds("start_s");
                end_s = params.seconds("end_s");
            }
            if (params.errors() > 0) {
                return invalid_parameters();
            }
            if (start_s && end_s && *start_s >= *end_s) {
                throw InvalidQuery("start_s must be less than end_s.");
            }
            const std::string& meeting_id = match->params[0];
            const auto read = [&](bool remote) {
                if (segments) {
                    return store.segments(
                        SegmentQuery{meeting_id, start_s, end_s, limit, cursor, remote});
                }
                return store.segment(meeting_id, match->params[1], remote);
            };
            if (!device_id) {
                return json_response(200, read(include_remote));
            }
            std::exception_ptr unavailable;
            if (federation_) {
                Json query{{"meeting_id", meeting_id}};
                if (segments) {
                    query["limit"] = limit;
                    if (cursor) {
                        query["cursor"] = *cursor;
                    }
                    if (start_s) {
                        query["start_s"] = *start_s;
                    }
                    if (end_s) {
                        query["end_s"] = *end_s;
                    }
                } else {
                    query["segment_id"] = match->params[1];
                }
                try {
                    return json_response(
                        200,
                        federation_->query(*device_id, segments ? "segments" : "segment", query));
                } catch (const remote::ClientUnavailable&) {
                    unavailable = std::current_exception();
                }
            }
            if (include_remote) {
                try {
                    if (store.meeting(meeting_id, true).value("origin_device_id", Json()) ==
                        Json(*device_id)) {
                        return json_response(200, read(true));
                    }
                } catch (const NotFound&) {
                }
            }
            if (unavailable) {
                std::rethrow_exception(unavailable);
            }
            throw detail::ClientUnavailable();
        }
        }
        return error_response(404, "not_found", "Not Found");
    } catch (const InvalidQuery& error) {
        return error_response(400, "invalid_query", error.what());
    } catch (const NotFound&) {
        return error_response(404, "not_found", "Record not found.");
    } catch (const remote::ClientUnavailable& error) {
        return error_response(error.code() == "not_found" ? 404 : 503, error.code(), error.what());
    } catch (const detail::ClientUnavailable& error) {
        return error_response(error.status(), error.code(), error.what());
    } catch (const SnapshotUnavailable& error) {
        return error_response(503, "snapshot_unavailable", error.what());
    } catch (const sql::Error& error) {
        // Never return database paths, SQL, or bound values in error messages.
        kLog.warning("History API could not read the database (SQLite code {})", error.code());
        return error_response(503, "database_unavailable",
                              "History is temporarily unavailable. Retry the request.");
    }
}

} // namespace fv::agentapi
