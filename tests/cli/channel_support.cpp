#include "channel_support.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "base/identity.h"
#include "cli_process.h"
#include "process/process_env.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::clitest {

#ifndef _WIN32
std::unique_ptr<Process> start_posix_cli_fixture(const ProcessSpec& spec);
#endif
namespace {

std::map<std::string, std::string> lower_headers(const net::Headers& headers) {
    std::map<std::string, std::string> out;
    for (const auto& [name, value] : headers) {
        out.emplace(py::lower(name), value);
    }
    return out;
}

net::Headers json_headers() {
    return {{"content-type", "application/json"}};
}

std::string error_body(const std::string& code, const std::string& message) {
    return py::dumps(
        Json::object({{"error", Json::object({{"code", code}, {"message", message}})}}));
}

std::string event_time() {
    return "2026-10-10T12:00:00.000Z";
}

} // namespace

// --- FakeApp ---------------------------------------------------------------------------------

FakeApp::FakeApp(std::string token) : token_(std::move(token)) {
    pinger_ = std::thread([this] { ping_loop(); });
}

FakeApp::~FakeApp() {
    stop();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    pinger_.join();
    reactor_.stop(std::chrono::milliseconds(0));
}

std::string FakeApp::url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

FakeApp& FakeApp::start() {
    auto server = net::HttpServer::create(reactor_);
    server->set_fallback(
        [this](const net::HttpRequest& request, bool, std::shared_ptr<net::Responder> responder) {
            serve(request, std::move(responder));
        });
    // The port of a listener just closed may take a moment to be free again.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto listening = server->listen({"127.0.0.1", port_, std::nullopt, false});
        if (listening) {
            port_ = listening->port;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("the fake app could not listen: " + listening.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::lock_guard lock(mutex_);
    server_ = std::move(server);
    return *this;
}

void FakeApp::stop() {
    std::unique_ptr<net::HttpServer> server;
    std::vector<std::shared_ptr<net::EventStream>> streams;
    {
        std::lock_guard lock(mutex_);
        server = std::move(server_);
        streams.swap(streams_);
    }
    changed_.notify_all();
    for (const auto& stream : streams) {
        stream->close();
    }
    if (server) {
        server->stop(std::chrono::milliseconds(200));
    }
}

void FakeApp::serve(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder) {
    SeenRequest seen{request.method, request.path, request.query, lower_headers(request.headers),
                     Json()};
    const auto authorization = net::header(request.headers, "Authorization");
    const bool authorized = authorization && *authorization == "Bearer " + token_;
    if (request.method == "GET") {
        {
            std::lock_guard lock(mutex_);
            stream_requests_.push_back(seen);
        }
        changed_.notify_all();
        if (request.path != "/v1/events") {
            responder->send(404, json_headers(), error_body("not_found", "No such route."));
            return;
        }
        if (!authorized) {
            responder->send(401, json_headers(),
                            error_body("unauthorized", "A valid bearer token is required."));
            return;
        }
        if (events_status.load() != 200) {
            responder->send(events_status.load(), json_headers(),
                            error_body("forbidden", "Allow live events."));
            return;
        }
        std::shared_ptr<net::EventStream> stream = responder->start_sse(
            {{"content-type", "text/event-stream; charset=utf-8"}, {"cache-control", "no-store"}});
        stream->write("retry: 2000\r\n\r\n"); // the live API's first frame
        const auto last = net::header(request.headers, "Last-Event-ID");
        {
            std::lock_guard lock(mutex_);
            if (last) {
                const std::int64_t after = std::stoll(std::string(*last));
                for (const auto& [seq, frame] : buffer_) {
                    if (seq > after) {
                        stream->write(frame);
                    }
                }
            }
            streams_.push_back(stream);
        }
        changed_.notify_all();
        return;
    }
    if (request.method == "POST") {
        try {
            seen.body = request.body.empty() ? Json::object() : py::loads(request.body);
        } catch (const std::exception&) {
            seen.body = Json();
        }
        std::string message;
        {
            std::lock_guard lock(mutex_);
            notifications_.push_back(seen);
            message = notify_message_;
        }
        changed_.notify_all();
        if (request.path != "/v1/notify") {
            responder->send(404, json_headers(), error_body("not_found", "No such route."));
        } else if (!authorized) {
            responder->send(401, json_headers(),
                            error_body("unauthorized", "A valid bearer token is required."));
        } else if (notify_status.load() == 202) {
            responder->send(202, json_headers(), R"({"shown": true})");
        } else {
            responder->send(notify_status.load(), json_headers(), error_body("refused", message));
        }
        return;
    }
    responder->send(405, json_headers(), error_body("method_not_allowed", "No."));
}

void FakeApp::ping_loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        changed_.wait_for(lock, std::chrono::milliseconds(200));
        const auto before = streams_.size();
        std::erase_if(streams_, [](const std::shared_ptr<net::EventStream>& stream) {
            return !stream->write(": ping\r\n\r\n");
        });
        if (streams_.size() != before) {
            changed_.notify_all();
        }
    }
}

std::int64_t FakeApp::publish(const Json& data, const std::string& type) {
    std::lock_guard lock(mutex_);
    ++seq_;
    char id[32];
    std::snprintf(id, sizeof(id), "evt_%024llx", static_cast<unsigned long long>(seq_));
    Json event = Json::object();
    event["id"] = id;
    event["seq"] = seq_;
    event["type"] = type;
    event["time"] = event_time();
    event["v"] = 1;
    event["data"] = data;
    py::DumpOptions options;
    options.ensure_ascii = false;
    const std::string frame = "id: " + std::to_string(seq_) + "\r\nevent: " + type +
                              "\r\ndata: " + py::dumps(event, options) + "\r\n\r\n";
    buffer_.emplace_back(seq_, frame);
    for (const auto& stream : streams_) {
        stream->write(frame);
    }
    changed_.notify_all();
    return seq_;
}

void FakeApp::send_gap(std::int64_t missed_from_seq, const std::string& reason) {
    Json event = Json::object();
    event["id"] = "";
    event["seq"] = 0;
    event["type"] = "events.gap";
    event["time"] = event_time();
    event["v"] = 1;
    event["data"] = Json::object({{"missed_from_seq", missed_from_seq}, {"reason", reason}});
    const std::string frame = "event: events.gap\r\ndata: " + py::dumps(event) + "\r\n\r\n";
    std::lock_guard lock(mutex_);
    for (const auto& stream : streams_) {
        stream->write(frame);
    }
}

std::vector<SeenRequest> FakeApp::stream_requests() const {
    std::lock_guard lock(mutex_);
    return stream_requests_;
}

std::vector<SeenRequest> FakeApp::notifications() const {
    std::lock_guard lock(mutex_);
    return notifications_;
}

std::size_t FakeApp::open_streams() const {
    std::lock_guard lock(mutex_);
    return streams_.size();
}

bool FakeApp::wait_for(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void FakeApp::set_notify_message(std::string message) {
    std::lock_guard lock(mutex_);
    notify_message_ = std::move(message);
}

// --- the child ---------------------------------------------------------------------------------

std::map<std::string, std::string> child_env(const std::map<std::string, std::string>& values) {
    std::map<std::string, std::string> env;
    for (const auto& [name, value] : system_mutable_env().items()) {
        env[name] = value;
    }
    for (const AppIdentity* id : {&identity(), &legacy_identity()}) {
        for (const char* suffix : {"URL", "TOKEN", "LOG_LEVEL", "API_TOKEN"}) {
            std::string name = std::string(id->env_prefix) + suffix;
            std::erase_if(
                env, [&](const auto& entry) { return py::upper(entry.first) == py::upper(name); });
        }
    }
    env["QT_QPA_PLATFORM"] = "offscreen";
    for (const auto& [name, value] : values) {
        env[name] = value;
    }
    return env;
}

ChannelProcess::ChannelProcess(const std::filesystem::path& program, std::vector<std::string> args,
                               std::map<std::string, std::string> env) {
    ProcessSpec spec;
    spec.program = program;
    spec.args = std::move(args);
    spec.env = std::move(env);
    spec.pipes = true;
#ifdef _WIN32
    auto started = make_process_launcher()->start(spec);
    if (!started) {
        throw std::runtime_error("could not start the channel: " + started.error());
    }
    process_ = std::move(*started);
#else
    process_ = start_posix_cli_fixture(spec);
#endif
    process_->on_bytes(
        [this](std::span<const char> bytes) {
            std::lock_guard lock(mutex_);
            if (bytes.empty()) {
                out_closed_ = true;
                changed_.notify_all();
                return;
            }
            out_buffer_.append(bytes.data(), bytes.size());
            for (std::size_t end = out_buffer_.find('\n'); end != std::string::npos;
                 end = out_buffer_.find('\n')) {
                const std::string line = out_buffer_.substr(0, end + 1);
                out_buffer_.erase(0, end + 1);
                raw_lines_.push_back(line);
                try {
                    lines_.push_back(py::loads(line));
                } catch (const std::exception&) {
                    lines_.push_back(Json::object({{"unparsed", line}}));
                }
            }
            changed_.notify_all();
        },
        [this](std::span<const char> bytes) {
            std::lock_guard lock(mutex_);
            if (!bytes.empty()) {
                err_.append(bytes.data(), bytes.size());
            }
        });
}

ChannelProcess::~ChannelProcess() {
    if (!process_->exit_code()) {
        process_->kill_tree();
        process_->wait(std::chrono::seconds(10));
    }
    process_->release_pipes(std::chrono::seconds(2));
}

void ChannelProcess::send(const Json& message) {
    process_->write_stdin(py::dumps(message) + "\n");
}

Json ChannelProcess::take(const std::function<bool(const Json&)>& predicate,
                          std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    for (auto it = unclaimed_.begin(); it != unclaimed_.end(); ++it) {
        if (predicate(*it)) {
            Json found = std::move(*it);
            unclaimed_.erase(it);
            return found;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!changed_.wait_until(lock, deadline, [&] { return !lines_.empty() || out_closed_; })) {
            throw std::runtime_error("no matching message; stderr:\n" + err_);
        }
        if (lines_.empty()) {
            throw std::runtime_error("the channel closed stdout; stderr:\n" + err_);
        }
        Json message = std::move(lines_.front());
        lines_.pop_front();
        if (predicate(message)) {
            return message;
        }
        unclaimed_.push_back(std::move(message));
    }
}

Json ChannelProcess::request(const std::string& method, std::optional<Json> params,
                             std::optional<Json> id, std::chrono::milliseconds timeout) {
    const Json request_id = id ? *id : Json(++next_id_);
    Json message = Json::object({{"jsonrpc", "2.0"}, {"id", request_id}, {"method", method}});
    if (params) {
        message["params"] = *params;
    }
    send(message);
    return take(
        [&](const Json& m) {
            return m.contains("id") && m["id"] == request_id && !m.contains("method");
        },
        timeout);
}

Json ChannelProcess::notification(const std::string& method, std::chrono::milliseconds timeout) {
    return take(
        [&](const Json& m) {
            return m.contains("method") && m["method"] == method && !m.contains("id");
        },
        timeout);
}

Json ChannelProcess::handshake() {
    Json response = request("initialize", Json::parse(R"({"protocolVersion": "2025-11-25",
        "capabilities": {}, "clientInfo": {"name": "claude-code", "version": "2.1.296"}})"));
    send(Json::object({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    return response;
}

std::optional<int> ChannelProcess::close_stdin(std::chrono::milliseconds timeout) {
    process_->close_stdin();
    const std::optional<int> code = process_->wait(timeout);
    if (code) {
        // What it wrote last reaches the readers before the pipes close.
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(5), [&] { return out_closed_; });
    }
    return code;
}

std::string ChannelProcess::stderr_text() const {
    std::lock_guard lock(mutex_);
    return err_;
}

std::vector<std::string> ChannelProcess::raw_lines() const {
    std::lock_guard lock(mutex_);
    return raw_lines_;
}

std::filesystem::path app_executable() {
#ifdef FV_APP_EXECUTABLE
    return std::filesystem::path(FV_APP_EXECUTABLE);
#else
    return {};
#endif
}

std::uint16_t closed_port() {
    net::Reactor reactor(1);
    auto server = net::HttpServer::create(reactor);
    const auto listening = server->listen({"127.0.0.1", 0, std::nullopt, true});
    if (!listening) {
        throw std::runtime_error("no free port: " + listening.error());
    }
    const std::uint16_t port = listening->port;
    server->stop(std::chrono::milliseconds(0));
    server.reset();
    reactor.stop(std::chrono::milliseconds(0));
    return port;
}

} // namespace fv::clitest
