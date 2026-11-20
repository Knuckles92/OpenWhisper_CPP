#include "automation/channel_link.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <tuple>
#include <utility>

#include "automation/catalog.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/sse_parser.h"
#include "automation/detail/thread_name.h"
#include "automation/detail/url.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/paths.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "net/address.h"
#include "net/tcp_knock.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::automation {
namespace {

constexpr log::Logger kLog{"services.automation.channel_bridge"};

// A refused stream's body is read this far, as http.client's response.read(65536).
constexpr std::size_t kRefusalBodyLimit = 65536;

std::string product() {
    return std::string(identity().product);
}

std::string env_or_legacy(const EnvReader& env, const std::string& name,
                          const std::string& legacy) {
    if (auto value = env.get(name); value && !value->empty()) {
        return *value;
    }
    return env.get(legacy).value_or("");
}

// The port of a netloc as urllib's SplitResult.port reads it: nullopt when there is none,
// py::ValueError when it is not ASCII digits or outside 0 to 65535.
std::optional<int> url_port(std::string_view netloc) {
    const std::size_t at = netloc.rfind('@');
    std::string_view hostinfo = at == std::string_view::npos ? netloc : netloc.substr(at + 1);
    std::string_view port;
    if (const std::size_t open = hostinfo.find('['); open != std::string_view::npos) {
        const std::string_view bracketed = hostinfo.substr(open + 1);
        const std::size_t close = bracketed.find(']');
        const std::string_view after =
            close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
        const std::size_t colon = after.find(':');
        port = colon == std::string_view::npos ? std::string_view() : after.substr(colon + 1);
    } else {
        const std::size_t colon = hostinfo.find(':');
        port = colon == std::string_view::npos ? std::string_view() : hostinfo.substr(colon + 1);
    }
    if (port.empty()) {
        return std::nullopt;
    }
    if (!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        throw py::ValueError("Port could not be cast to integer value");
    }
    std::size_t start = port.find_first_not_of('0');
    if (start == std::string_view::npos) {
        return 0;
    }
    port.remove_prefix(start);
    if (port.size() > 5) {
        throw py::ValueError("Port out of range 0-65535");
    }
    const int value = std::stoi(std::string(port));
    if (value > 65535) {
        throw py::ValueError("Port out of range 0-65535");
    }
    return value;
}

bool is_ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// [A-Za-z_][A-Za-z0-9_]*\Z
bool is_meta_key(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    const auto word = [](char c, bool first) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
               (!first && c >= '0' && c <= '9');
    };
    if (!word(key.front(), true)) {
        return false;
    }
    return std::all_of(key.begin() + 1, key.end(), [&](char c) { return word(c, false); });
}

const Json& field(const Json& object, std::string_view key) {
    return detail::get(object, key);
}

} // namespace

std::string channel_client_header() {
    return "X-" + product() + "-Client";
}

std::string url_env() {
    return std::string(identity().env_prefix) + "URL";
}
std::string token_env() {
    return std::string(identity().env_prefix) + "TOKEN";
}
std::string legacy_url_env() {
    return std::string(legacy_identity().env_prefix) + "URL";
}
std::string legacy_token_env() {
    return std::string(legacy_identity().env_prefix) + "TOKEN";
}

std::string Target::label() const {
    return host + ":" + std::to_string(port);
}

bool Target::on_this_computer() const {
    if (py::lower(host) == "localhost") {
        return true;
    }
    const std::optional<net::IpAddress> address = net::parse_address(host);
    return address && net::is_loopback(net::unmap_v4(*address));
}

int configured_port(const Json& settings) {
    return static_cast<int>(keys::mcp_port.read(settings.is_object() ? settings : Json::object()));
}

Target parse_url(std::string_view url) {
    const detail::UrlParts parts = detail::urlsplit(py::strip(url));
    bool valid = false;
    std::optional<int> port;
    std::optional<std::string> hostname;
    try {
        port = url_port(parts.netloc);
        hostname = parts.hostname();
        valid = (parts.scheme == "http" || parts.scheme == "https") && hostname.has_value();
    } catch (const py::ValueError&) {
        valid = false;
    }
    if (!valid) {
        throw LinkError("bad_url",
                        url_env() + " must be an http:// address such as http://127.0.0.1:8767.");
    }
    Target target;
    target.https = parts.scheme == "https";
    target.host = *hostname;
    target.port = port.value_or(0) != 0 ? *port : (target.https ? 443 : 80);
    target.base_path = py::rstrip(parts.path, "/");
    return target;
}

std::string error_message(std::string_view body) {
    if (!detail::encodes_as_utf8(body)) {
        return "";
    }
    Json document;
    try {
        document = py::loads(body);
    } catch (const std::exception&) {
        return "";
    }
    if (!document.is_object()) {
        return "";
    }
    const Json& error = field(document, "error");
    if (!error.is_object()) {
        return "";
    }
    const Json& message = field(error, "message");
    return message.is_string() ? message.get<std::string>() : "";
}

std::string reply_refusal(int status, std::string_view body) {
    const std::string name = product();
    if (status == 401) {
        return name + " rejected the channel's access token, so the reply wasn't shown.";
    }
    if (status == 403) {
        return name + " didn't show the reply: allow running actions in " + name +
               " Settings > API & MCP.";
    }
    if (status == 404) {
        return "This version of " + name + " can't show replies. Update " + name + ".";
    }
    std::string detail = error_message(body);
    if (detail.empty()) {
        detail = "HTTP " + std::to_string(status);
    }
    return name + " couldn't show the reply (" + detail + ").";
}

std::pair<std::string, std::string> stream_refusal(int status, std::string_view body) {
    const std::string name = product();
    if (status == 200) {
        return {"unsupported", name + " didn't answer with an event stream. Update " + name + "."};
    }
    if (status == 401) {
        return {"unauthorized", name + " rejected the access token; retrying."};
    }
    if (status == 403) {
        std::string detail = error_message(body);
        if (detail.empty()) {
            detail = "Allow live events in " + name + " Settings > API & MCP.";
        }
        return {"forbidden", name + " refused the event stream. " + detail};
    }
    if (status == 404) {
        return {"unsupported",
                "This version of " + name + " has no live event stream. Update " + name + "."};
    }
    return {"http_" + std::to_string(status), name + " answered the event stream with HTTP " +
                                                  std::to_string(status) + "; retrying."};
}

Json saved_settings() {
    const AppIdentity& id = identity();
    const Paths paths = Paths::resolve(id, system_env());
    std::ifstream in(paths.user_file(id.settings_file), std::ios::binary);
    if (!in) {
        return Json::object();
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        Json settings = py::loads(text);
        return settings.is_object() ? settings : Json::object();
    } catch (const std::exception&) {
        return Json::object();
    }
}

std::optional<std::string> saved_token() {
    CredentialStore store(make_platform_backend(), identity());
    return store.get(std::string(identity().env_prefix) + "MCP_TOKEN");
}

// --- AppLink ---------------------------------------------------------------------------------

AppLink::AppLink(AppLinkOptions options)
    : url_(py::strip(options.url)), fixed_token_(py::strip(options.token)),
      read_settings_(std::move(options.read_settings)), read_token_(std::move(options.read_token)),
      user_agent_(product() + "-channel/" + (options.version.empty() ? "0" : options.version)),
      client_(options.client ? std::move(options.client) : http::make_curl_client()) {}

AppLink::~AppLink() = default;

AppLinkOptions AppLink::from_environment(const EnvReader& env, std::string version) {
    AppLinkOptions options;
    options.url = env_or_legacy(env, url_env(), legacy_url_env());
    options.token = env_or_legacy(env, token_env(), legacy_token_env());
    options.version = std::move(version);
    return options;
}

Target AppLink::target() const {
    if (!url_.empty()) {
        Target target = parse_url(url_);
        if (!target.on_this_computer()) {
            // The saved token is this computer's: another computer gets only a token the user
            // gave for it, and only over https.
            if (!target.https) {
                throw LinkError("bad_url", url_env() + " must use https:// to reach " + product() +
                                               " on another computer.");
            }
            if (fixed_token_.empty()) {
                throw LinkError("no_token", "Set " + token_env() + " to reach " + product() +
                                                " on another computer; the saved access token "
                                                "is only sent to " +
                                                product() + " on this one.");
            }
        }
        return target;
    }
    Json settings = Json::object();
    try {
        if (read_settings_) {
            settings = read_settings_();
        }
    } catch (const std::exception& error) {
        kLog.debug("Reading settings failed: {}", detail::error_type(error));
        settings = Json::object();
    }
    return Target{"127.0.0.1", configured_port(settings), false, ""};
}

std::string AppLink::unreachable_message() const {
    if (url_.empty()) {
        std::optional<bool> enabled;
        try {
            const Json settings = read_settings_ ? read_settings_() : Json::object();
            enabled = keys::mcp_enabled.read(settings.is_object() ? settings : Json::object());
        } catch (const std::exception&) {
            enabled.reset(); // unknown: the message names both causes
        }
        if (enabled == false) {
            return "API & MCP is off in " + product() + " Settings.";
        }
    }
    return product() + " isn't running, or API & MCP is off in its Settings.";
}

std::string AppLink::token() {
    if (!fixed_token_.empty()) {
        return fixed_token_;
    }
    std::lock_guard lock(mutex_);
    if (!token_) {
        try {
            std::optional<std::string> read = read_token_ ? read_token_() : std::nullopt;
            if (read && !read->empty()) {
                token_ = std::move(read);
            }
        } catch (const std::exception& error) {
            kLog.debug("Reading the token failed: {}", detail::error_type(error));
            token_.reset();
        }
    }
    if (!token_) {
        throw LinkError("no_token", "Couldn't read " + product() +
                                        "'s access token from the system credential store. "
                                        "Turn on API & MCP in " +
                                        product() + " Settings.");
    }
    return *token_;
}

void AppLink::forget_token() {
    std::lock_guard lock(mutex_);
    token_.reset();
}

http::Headers AppLink::headers(const std::string& token, http::Headers extra) const {
    http::Headers headers{{"Authorization", "Bearer " + token},
                          {channel_client_header(), std::string(CHANNEL_CLIENT_NAME)},
                          {"User-Agent", user_agent_}};
    for (auto& header : extra) {
        headers.push_back(std::move(header));
    }
    return headers;
}

std::string AppLink::url(const Target& target, std::string_view path) {
    const std::string host =
        target.host.find(':') != std::string::npos ? "[" + target.host + "]" : target.host;
    return std::string(target.https ? "https" : "http") + "://" + host + ":" +
           std::to_string(target.port) + target.base_path + std::string(path);
}

void AppLink::cancel_requests() {
    requests_cancel_.cancel();
}

bool AppLink::reachable(const Target& target) const {
    return net::tcp_knock(target.host, static_cast<std::uint16_t>(target.port),
                          CHANNEL_CONNECT_TIMEOUT);
}

void AppLink::notify(const std::string& text, const std::string& title) {
    // json.dumps(ensure_ascii=False).encode("utf-8") fails on a lone surrogate, which Claude
    // then reads as the reply having failed.
    if (!detail::encodes_as_utf8(std::string_view(text)) ||
        !detail::encodes_as_utf8(std::string_view(title))) {
        throw std::runtime_error("the reply is not valid Unicode text");
    }
    Json body = Json::object();
    body["text"] = text;
    if (!title.empty()) {
        body["title"] = title;
    }
    py::DumpOptions options;
    options.ensure_ascii = false;
    const std::string payload = py::dumps(body, options);
    auto [status, reply] = post(NOTIFY_PATH, payload);
    if (status == 401 && fixed_token_.empty()) {
        forget_token(); // replaced since it was read; once more with the saved one
        std::tie(status, reply) = post(NOTIFY_PATH, payload);
    }
    if (status < 200 || status >= 300) {
        throw LinkError("http_" + std::to_string(status), reply_refusal(status, reply));
    }
}

std::pair<int, std::string> AppLink::post(std::string_view path, const std::string& payload) {
    const Target where = target();
    const auto unreachable = [&] {
        return LinkError("unreachable", unreachable_message() +
                                            " The reply wasn't shown; tell the user here instead.");
    };
    std::string bearer;
    try {
        bearer = token();
    } catch (const LinkError&) {
        if (!reachable(where)) {
            throw unreachable();
        }
        throw;
    }
    http::Request request;
    request.method = "POST";
    request.url = url(where, path);
    request.headers = headers(bearer, {{"Content-Type", "application/json"}});
    request.body = payload;
    request.connect_timeout = CHANNEL_CONNECT_TIMEOUT;
    request.idle_timeout = CHANNEL_NOTIFY_TIMEOUT;
    request.bypass_proxy = true; // http.client never goes through a proxy
    request.log_request = false;
    if (requests_cancel_.token().cancelled()) {
        throw unreachable();
    }
    const auto answer = client_->send(request, requests_cancel_.token());
    if (!answer) {
        const http::Error& error = answer.error();
        if (error.kind == http::ErrorKind::Cancelled) {
            throw unreachable();
        }
        const bool connecting = !error.after_request;
        if (connecting &&
            (error.kind == http::ErrorKind::Connect || error.kind == http::ErrorKind::Dns ||
             error.kind == http::ErrorKind::Tls || error.kind == http::ErrorKind::Timeout)) {
            throw unreachable();
        }
        if (error.kind == http::ErrorKind::Timeout) {
            throw LinkError("timeout", product() + " didn't answer in time, so the reply may not "
                                                   "have been shown.");
        }
        throw LinkError("unreachable",
                        "The connection to " + product() + " dropped, so the reply wasn't shown.");
    }
    return {answer->status, answer->body};
}

// --- what reaches Claude ---------------------------------------------------------------------

std::string origin_of(const Json& data) {
    const Json& origin = field(data, "origin");
    return origin.is_string() && origin.get<std::string>() == catalog::ORIGIN_DICTATED
               ? std::string(catalog::ORIGIN_DICTATED)
               : std::string(catalog::ORIGIN_PROGRAM);
}

Json clean_meta(const Json& meta) {
    Json cleaned = Json::object();
    if (!meta.is_object()) {
        return cleaned;
    }
    for (const auto& [key, value] : meta.items()) {
        if (!is_meta_key(key) || !value.is_string()) {
            continue;
        }
        std::string joined;
        for (const std::string& word : py::split(value.get<std::string>())) {
            if (!joined.empty()) {
                joined += ' ';
            }
            joined += word;
        }
        joined = py::slice(joined, 0, static_cast<std::int64_t>(MAX_META_CHARS));
        if (!joined.empty()) {
            cleaned[key] = joined;
        }
    }
    return cleaned;
}

std::optional<Json> channel_params(const Json& event) {
    if (!event.is_object()) {
        return std::nullopt;
    }
    const Json& data = field(event, "data");
    if (!data.is_object()) {
        return std::nullopt;
    }
    const Json& text = field(data, "text");
    if (!text.is_string() || py::strip(text.get<std::string>()).empty()) {
        return std::nullopt;
    }
    Json app = field(data, "app");
    if (app.is_object()) {
        app = Json(field(app, "name"));
    }
    Json meta = Json::object();
    meta["origin"] = origin_of(data);
    meta["job_id"] = field(data, "job_id");
    meta["action"] = field(data, "action_id");
    meta["app"] = app;
    Json params = Json::object();
    params["content"] = text;
    params["meta"] = clean_meta(meta);
    return params;
}

std::string gap_reason(const SseEvent& event) {
    Json payload;
    try {
        payload = py::loads(event.data);
    } catch (const std::exception&) {
        return "";
    }
    const Json& data = field(payload, "data");
    const Json& reason = field(data, "reason");
    return reason.is_string() ? reason.get<std::string>() : "";
}

std::string channel_notice_origin() {
    return py::lower(identity().product);
}

std::string gap_notice() {
    return "Some messages sent from " + product() +
           " while this session was disconnected may have been missed. If the user seems to be "
           "waiting on something, ask them to send it again.";
}

// --- EventStream -----------------------------------------------------------------------------

EventStream::EventStream(AppLink& link, std::function<void(const Json&)> deliver,
                         EventStreamOptions options)
    : link_(link), deliver_(std::move(deliver)), options_(std::move(options)) {}

EventStream::~EventStream() {
    stop();
}

void EventStream::start() {
    std::lock_guard lock(mutex_);
    if (thread_.joinable() || stop_.load()) {
        return;
    }
    running_.store(true);
    thread_ = std::thread([this] {
        detail::set_current_thread_name("channel-events");
        try {
            run();
        } catch (const std::exception& error) {
            // Python's thread ended the same way: nothing more is followed.
            kLog.error("The event stream stopped: {}", detail::error_type(error));
        }
        running_.store(false);
    });
}

void EventStream::stop() {
    stop_.store(true);
    cancel_.cancel(); // ends a request in progress, as shutting the socket did
    std::thread thread;
    {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable() || thread_.get_id() == std::this_thread::get_id()) {
            return;
        }
        thread = std::move(thread_);
    }
    thread.join();
}

std::string EventStream::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::string EventStream::last_id() const {
    std::lock_guard lock(mutex_);
    return last_id_;
}

bool EventStream::running() const {
    return running_.load();
}

void EventStream::report(const std::string& state, const std::string& message, bool warning) {
    {
        std::lock_guard lock(mutex_);
        if (state == state_) {
            return;
        }
        state_ = state;
    }
    if (warning) {
        kLog.warning("{}", message);
    } else {
        kLog.info("{}", message);
    }
}

void EventStream::run() {
    std::chrono::milliseconds delay = options_.backoff_first;
    while (!stopping()) {
        const auto [opened, state] = follow_once();
        if (stopping()) {
            return;
        }
        if (state == "unreachable" && options_.on_update && options_.update_check &&
            options_.update_check()) {
            kLog.info("{} is updating; closing the channel so the update can replace its files. "
                      "Reconnect the {} server with /mcp afterwards.",
                      product(), channel_notice_origin());
            options_.on_update();
            return;
        }
        if (opened) {
            delay = options_.backoff_first;
        }
        if (cancel_.token().wait_for(delay)) {
            return;
        }
        if (!opened) {
            delay = std::min(delay * 2, options_.backoff_max);
        }
    }
}

namespace {

// What one GET /v1/events gave: its head, a refused answer's body, or the event stream itself.
class StreamSink final : public http::BodySink {
public:
    StreamSink(EventStream& stream, std::string last_id, const std::atomic<bool>& stop,
               std::function<void(const http::Response&)> on_open,
               std::function<void(std::string)> on_resume)
        : stream_(stream), parser_(std::move(last_id)), stop_(stop), on_open_(std::move(on_open)),
          on_resume_(std::move(on_resume)) {}

    bool on_head(const http::Response& head) override {
        status = head.status;
        const std::string type = http::find_header(head.headers, "Content-Type").value_or("");
        if (head.status != 200 || !type.starts_with("text/event-stream")) {
            refused = true;
            return true;
        }
        opened = true;
        on_open_(head);
        return !stop_.load();
    }

    bool write(std::span<const char> bytes) override {
        if (refused) {
            const std::size_t room = kRefusalBodyLimit - body.size();
            body.append(bytes.data(), std::min(room, bytes.size()));
            return body.size() < kRefusalBodyLimit;
        }
        buffer_.append(bytes.data(), bytes.size());
        std::size_t start = 0;
        for (;;) {
            if (stop_.load()) {
                return false;
            }
            const std::size_t end = buffer_.find('\n', start);
            if (end == std::string::npos) {
                break;
            }
            const std::optional<SseEvent> event = parser_.feed(
                detail::sse_line(std::string_view(buffer_).substr(start, end - start)));
            start = end + 1;
            if (event) {
                stream_.handle(*event);
            }
            on_resume_(parser_.last_id());
        }
        buffer_.erase(0, start);
        return !stop_.load();
    }

    int status = 0;
    bool opened = false;
    bool refused = false;
    std::string body;

private:
    EventStream& stream_;
    SseParser parser_;
    const std::atomic<bool>& stop_;
    std::function<void(const http::Response&)> on_open_;
    std::function<void(std::string)> on_resume_;
    std::string buffer_;
};

} // namespace

std::pair<bool, std::string> EventStream::follow_once() {
    Target target;
    try {
        target = link_.target();
    } catch (const LinkError& error) {
        report(error.state(), error.message(), error.state() == "bad_url");
        return {false, error.state()};
    }
    std::string token;
    try {
        token = link_.token();
    } catch (const LinkError& error) {
        // Python connected first: with the app away, a missing token reads as unreachable.
        if (!link_.reachable(target)) {
            report("unreachable", link_.unreachable_message());
            return {false, "unreachable"};
        }
        report(error.state(), error.message());
        return {false, error.state()};
    }
    if (stopping()) {
        return {false, "stopped"};
    }

    http::Request request;
    request.method = "GET";
    request.url = AppLink::url(target, EVENTS_PATH);
    request.headers =
        link_.headers(token, {{"Accept", "text/event-stream"}, {"Cache-Control", "no-cache"}});
    const std::string resume = last_id();
    if (!resume.empty() && is_ascii(resume) && py::isprintable(resume)) {
        request.headers.emplace_back("Last-Event-ID", resume);
    }
    request.connect_timeout = CHANNEL_CONNECT_TIMEOUT;
    request.idle_timeout = options_.read_timeout;
    request.bypass_proxy = true;
    request.log_request = false;

    StreamSink sink(
        *this, resume, stop_,
        [this, &target](const http::Response&) {
            report("connected", "Connected to " + product() + " at " + target.label() +
                                    "; waiting for dictated messages.");
        },
        [this](std::string id) {
            std::lock_guard lock(mutex_);
            last_id_ = std::move(id);
        });
    const auto answer = link_.client().send(request, cancel_.token(), &sink);

    if (sink.refused) {
        auto [state, message] = stream_refusal(sink.status, sink.body);
        if (sink.status == 401) {
            link_.forget_token();
        }
        report(state, message);
        return {false, state};
    }
    if (answer) {
        if (!sink.opened) {
            return connection_lost(false);
        }
        if (!stopping()) {
            report("dropped", product() + " closed the event stream; reconnecting.");
        }
        return {true, "dropped"};
    }
    const http::Error& error = answer.error();
    if (error.kind == http::ErrorKind::Cancelled || stopping()) {
        return {sink.opened, "stopped"};
    }
    const bool connecting = !error.after_request && !sink.opened;
    if (connecting &&
        (error.kind == http::ErrorKind::Connect || error.kind == http::ErrorKind::Dns ||
         error.kind == http::ErrorKind::Tls || error.kind == http::ErrorKind::Timeout)) {
        report("unreachable", link_.unreachable_message());
        return {false, "unreachable"};
    }
    if (error.kind == http::ErrorKind::Timeout) {
        report("silent", product() + " stopped sending events; reconnecting.");
        return {sink.opened, "silent"};
    }
    return connection_lost(sink.opened);
}

std::pair<bool, std::string> EventStream::connection_lost(bool opened) {
    if (stopping()) {
        return {opened, "stopped"};
    }
    if (opened) {
        report("dropped", "The connection to " + product() + " dropped; reconnecting.");
        return {true, "dropped"};
    }
    report("unreachable", link_.unreachable_message());
    return {false, "unreachable"};
}

void EventStream::handle(const SseEvent& event) {
    if (event.type == catalog::EVENTS_GAP) {
        announce_gap(event);
        return;
    }
    if (event.type != catalog::CHANNEL_MESSAGE) {
        return;
    }
    Json payload;
    try {
        payload = py::loads(event.data);
    } catch (const std::exception&) {
        kLog.warning("Skipped a channel event that wasn't JSON.");
        return;
    }
    std::optional<Json> params = channel_params(payload);
    if (!params) {
        kLog.info("Skipped a channel event with no text.");
        return;
    }
    deliver_(*params);
}

void EventStream::announce_gap(const SseEvent& event) {
    // Only messages the app no longer holds can be lost: after an app restart every message the
    // new run sent is replayed, and a stream that fell behind replays the rest when it
    // reconnects. events.gap has no id line, so the resume point stays put; until a new message
    // moves it, every reconnect brings the same gap again.
    if (gap_reason(event) != catalog::GAP_EVICTED) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (gap_announced_at_ == event.last_id) {
            return;
        }
        gap_announced_at_ = event.last_id;
    }
    kLog.warning("Some messages may have been missed while disconnected from {}.", product());
    Json params = Json::object();
    params["content"] = gap_notice();
    params["meta"] = Json::object({{"origin", channel_notice_origin()}});
    deliver_(params);
}

} // namespace fv::automation
