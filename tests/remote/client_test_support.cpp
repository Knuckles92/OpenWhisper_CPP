#include "client_test_support.h"

#include <cmath>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <utility>

#include "base/ids.h"
#include "net/http_server.h"
#include "net/tls.h"
#include "pyport/json.h"
#include "remote/connection.h"
#include "remote/protocol.h"
#include "remote/wire.h"

namespace fv::remote::test {

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

std::vector<float> tone(std::size_t samples) {
    std::vector<float> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<float>(0.1 * std::sin(static_cast<float>(i) / 8.0f));
    }
    return out;
}

namespace {

// message[key] when it is a string, else "".
std::string text_of(const Json& message, const char* key) {
    if (!message.is_object()) {
        return {};
    }
    const auto found = message.find(key);
    return found != message.end() && found->is_string() ? found->get<std::string>() : std::string();
}

Json fake_engine() {
    return Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Fake Parakeet"},
                {"device", "cuda"},     {"streaming", false},     {"available", true},
                {"status", "ready"}};
}

} // namespace

struct MiniHost::State {
    Options options;
    net::Reactor* reactor = nullptr;
    net::TlsIdentity identity;
    std::string fingerprint;
    std::unique_ptr<net::HttpServer> server;
    std::uint16_t port = 0;
    std::string error;

    mutable std::mutex mutex;
    std::size_t connections = 0;
    std::size_t busy_replies = 0;
    std::vector<Json> hellos;
    std::vector<Json> requests;
    std::vector<std::string> tokens;
    std::map<const net::WsConnection*, std::size_t> delivered;
    // "first", then "commit" and "revealed" for a pairing request, or "ready".
    std::map<const net::WsConnection*, std::string> stage;
    std::vector<std::weak_ptr<net::WsConnection>> sockets;
    // Replies sent later (a slow transcribe), joined at stop.
    std::vector<std::thread> workers;

    std::string advertised() const {
        if (!options.advertised_fingerprint.empty()) {
            return options.advertised_fingerprint;
        }
        std::string out = fingerprint;
        if (options.lowercase_fingerprint) {
            for (char& ch : out) {
                if (ch >= 'A' && ch <= 'F') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
        }
        return out;
    }

    Json host_info() const {
        return Json{{"name", options.name},
                    {"fingerprint", advertised()},
                    {"addresses", options.addresses}};
    }

    static void send(const std::shared_ptr<net::WsConnection>& socket, const Json& message) {
        socket->send_text(std::make_shared<const std::string>(dumps_compact(message)));
    }

    static void refuse(const std::shared_ptr<net::WsConnection>& socket, std::string code,
                       std::string message, int close_code) {
        send(socket, Json{{"type", "error"}, {"code", code}, {"message", std::move(message)}});
        socket->close(static_cast<std::uint16_t>(close_code), std::move(code));
    }

    // True when this message is one the host still reads and answers.
    bool admit(const std::shared_ptr<net::WsConnection>& socket) {
        std::lock_guard lock(mutex);
        const std::size_t count = ++delivered[socket.get()];
        if (options.stop_reading_after != 0 && count > options.stop_reading_after) {
            return false;
        }
        socket->release_message();
        return true;
    }

    void on_text(const std::shared_ptr<net::WsConnection>& socket, std::string_view text) {
        if (!admit(socket)) {
            return;
        }
        Json message;
        try {
            message = py::loads(text);
        } catch (const std::exception&) {
            message = nullptr;
        }
        if (!message.is_object()) {
            socket->close(CLOSE_BAD_REQUEST, "expected hello");
            return;
        }
        std::string at;
        {
            std::lock_guard lock(mutex);
            at = stage[socket.get()];
        }
        const std::string type = text_of(message, "type");
        if (at == "commit") {
            // The reveal breaks the commitment: another nonce than the one committed to.
            const std::vector<std::uint8_t> revealed(PAIRING_NONCE_BYTES, 'b');
            std::string hex;
            for (const auto value : revealed) {
                hex += std::format("{:02x}", value);
            }
            send(socket, Json{{"type", "pair_reveal"}, {"nonce", hex}});
            std::lock_guard lock(mutex);
            stage[socket.get()] = "revealed";
            return;
        }
        if (at == "revealed" || at == "ready") {
            return;
        }
        if (type == "probe") {
            send(socket, Json{{"type", "probe"},
                              {"protocol", PROTOCOL_VERSION},
                              {"host", Json{{"name", options.name}}},
                              {"engine", Json{{"label", "Fake Parakeet"},
                                              {"device", "cuda"},
                                              {"available", true},
                                              {"family", "parakeet"}}},
                              {"tailscale_pairing", options.tailscale_pairing},
                              {"approval", options.approval}});
            socket->close(1000, "");
            return;
        }
        if (type == "pair") {
            if (text_of(message, "code") != options.code) {
                refuse(socket, "bad_code", "That code doesn't match the one on the host.",
                       CLOSE_UNAUTHORIZED);
                return;
            }
            const std::string token = token_urlsafe(32);
            {
                std::lock_guard lock(mutex);
                tokens.push_back(token);
            }
            send(socket, Json{{"type", "paired"},
                              {"token", token},
                              {"device_id", uuid4_hex()},
                              {"host", host_info()}});
            socket->close(1000, "");
            return;
        }
        if (type == "pair_request" && options.broken_reveal) {
            const std::vector<std::uint8_t> committed(PAIRING_NONCE_BYTES, 'a');
            send(socket, Json{{"type", "pair_commit"},
                              {"commit", pairing_commitment(committed)},
                              {"timeout_s", 120}});
            std::lock_guard lock(mutex);
            stage[socket.get()] = "commit";
            return;
        }
        if (type == "hello") {
            bool known = false;
            {
                std::lock_guard lock(mutex);
                hellos.push_back(message);
                for (const auto& token : tokens) {
                    known = known || text_of(message, "token") == token;
                }
            }
            if (!known) {
                refuse(socket, "unauthorized",
                       "This computer isn't paired with the host anymore. Pair it again.",
                       CLOSE_UNAUTHORIZED);
                return;
            }
            Json ready = Json{{"type", "ready"},
                              {"host", host_info()},
                              {"device", Json{{"id", "d"}, {"name", "laptop"}}},
                              {"engine", fake_engine()},
                              {"models", Json::array()},
                              {"runtime", Json::object()},
                              {"capabilities", Json{{"model_management", false},
                                                    {"runtime_installation", false},
                                                    {"engine_controls", false},
                                                    {"recognition_hints", false},
                                                    {"recording_activity", true}}}};
            if (!options.list_models) {
                ready.erase("models");
            }
            send(socket, ready);
            std::lock_guard lock(mutex);
            stage[socket.get()] = "ready";
            return;
        }
        socket->close(CLOSE_BAD_REQUEST, "expected hello");
    }

    void on_binary(const std::shared_ptr<net::WsConnection>& socket,
                   std::span<const std::byte> data) {
        // Read (and recorded) even past stop_reading_after: only the answer and the release are
        // held back.
        Frame frame;
        try {
            frame = unpack_frame(data);
        } catch (const ProtocolError& error) {
            send(socket, Json{{"id", nullptr}, {"error", error.what()}, {"code", "bad_request"}});
            (void)admit(socket);
            return;
        }
        {
            std::lock_guard lock(mutex);
            requests.push_back(frame.header);
        }
        if (!admit(socket)) {
            return;
        }
        const Json id = frame.header.value("id", Json());
        const std::string op = text_of(frame.header, "op");
        if (op == "transcribe") {
            bool busy = false;
            {
                std::lock_guard lock(mutex);
                if (options.transcribe_busy_count > 0) {
                    --options.transcribe_busy_count;
                    busy = true;
                }
            }
            if (busy) {
                send(socket, Json{{"id", id}, {"error", "The host is busy."}, {"code", "busy"}});
                {
                    std::lock_guard lock(mutex);
                    ++busy_replies;
                }
                return;
            }
            const std::size_t samples = frame.payload.size() / 2;
            Json reply = {
                {"id", id},
                {"result",
                 Json{{"text", std::format("heard {}", samples)},
                      {"segments",
                       Json::array({Json{{"text", std::format("heard {}", samples)},
                                         {"start", 0.0},
                                         {"end", static_cast<double>(samples) / SAMPLE_RATE}}})}}},
                {"host_ms", 1.5}};
            if (!options.timed) {
                reply.erase("host_ms");
            }
            if (options.transcribe_delay.count() > 0) {
                std::lock_guard lock(mutex);
                workers.emplace_back([socket, reply, delay = options.transcribe_delay] {
                    std::this_thread::sleep_for(delay);
                    send(socket, reply);
                });
            } else {
                send(socket, reply);
            }
            return;
        }
        if (op == "describe") {
            send(socket, Json{{"id", id}, {"result", fake_engine()}, {"host_ms", 0.1}});
            return;
        }
        if (op == "recording_begin" || op == "recording_end") {
            send(socket, Json{{"id", id}, {"result", Json::object()}, {"host_ms", 0.1}});
            return;
        }
        send(socket, Json{{"id", id}, {"error", std::format("Unknown operation: '{}'", op)}});
    }
};

MiniHost::MiniHost(net::Reactor& reactor, Options options) : state_(std::make_shared<State>()) {
    auto& state = *state_;
    state.options = std::move(options);
    state.reactor = &reactor;
    state.identity = net::generate_self_signed_p256("MiniHost host test");
    state.identity.disable_tickets = true;
    state.fingerprint = net::certificate_sha256_from_pem(state.identity.cert_pem);
    restart();
}

void MiniHost::restart() {
    auto& state = *state_;
    const auto port = state.port ? state.port : state.options.port;
    stop();
    state.port = 0;
    state.error.clear();
    state.server = net::HttpServer::create(*state.reactor);

    std::weak_ptr<State> weak = state_;
    net::WsHandlers handlers;
    handlers.permessage_deflate = false;
    handlers.max_message_bytes = 8u << 20;
    handlers.ping_interval = std::chrono::milliseconds(20'000);
    handlers.ping_timeout = std::chrono::milliseconds(20'000);
    handlers.max_queued_messages = 1;
    handlers.on_open = [weak](std::shared_ptr<net::WsConnection> socket, const net::HttpRequest&) {
        if (auto self = weak.lock()) {
            std::lock_guard lock(self->mutex);
            ++self->connections;
            self->stage[socket.get()] = "first";
            self->sockets.push_back(socket);
        }
    };
    handlers.on_text = [weak](const std::shared_ptr<net::WsConnection>& socket,
                              std::string_view text) {
        if (auto self = weak.lock()) {
            self->on_text(socket, text);
        }
    };
    handlers.on_binary = [weak](const std::shared_ptr<net::WsConnection>& socket,
                                std::span<const std::byte> data) {
        if (auto self = weak.lock()) {
            self->on_binary(socket, data);
        }
    };
    state.server->websocket(std::string(PATH), std::move(handlers));
    auto endpoint = state.server->listen(
        net::Listen{.address = state.options.bind, .port = port, .tls = state.identity});
    if (endpoint) {
        state.port = endpoint->port;
    } else {
        state.error = endpoint.error();
    }
}

MiniHost::~MiniHost() {
    stop();
}

bool MiniHost::listening() const {
    return state_->port != 0;
}

std::string MiniHost::error() const {
    return state_->error;
}

std::uint16_t MiniHost::port() const {
    return state_->port;
}

std::string MiniHost::fingerprint() const {
    return state_->fingerprint;
}

std::size_t MiniHost::connections() const {
    std::lock_guard lock(state_->mutex);
    return state_->connections;
}

std::vector<Json> MiniHost::hellos() const {
    std::lock_guard lock(state_->mutex);
    return state_->hellos;
}

std::vector<Json> MiniHost::requests() const {
    std::lock_guard lock(state_->mutex);
    return state_->requests;
}

std::vector<std::string> MiniHost::tokens() const {
    std::lock_guard lock(state_->mutex);
    return state_->tokens;
}

void MiniHost::close_all(std::uint16_t code, std::string reason) {
    std::vector<std::shared_ptr<net::WsConnection>> open;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& socket : state_->sockets) {
            if (auto live = socket.lock()) {
                open.push_back(std::move(live));
            }
        }
    }
    for (const auto& socket : open) {
        socket->close(code, reason);
    }
}

std::size_t MiniHost::busy_replies() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy_replies;
}

void MiniHost::stop() {
    std::unique_ptr<net::HttpServer> server;
    std::vector<std::thread> workers;
    {
        std::lock_guard lock(state_->mutex);
        server = std::move(state_->server);
        workers = std::move(state_->workers);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (server) {
        server->stop(std::chrono::milliseconds(0));
    }
}

BusResponder::BusResponder(testing::FakeDiscoveryBus& bus, std::string address, Json info)
    : socket_(bus.transport(std::move(address))), info_(std::move(info)) {
    if (!socket_->bind("0.0.0.0", static_cast<std::uint16_t>(DISCOVERY_PORT))) {
        throw std::runtime_error("the responder could not bind");
    }
    thread_ = std::thread([this] {
        while (!stop_.load()) {
            const auto datagram = socket_->receive(std::chrono::milliseconds(20));
            if (!datagram) {
                continue;
            }
            const auto nonce = parse_query(datagram->bytes);
            if (!nonce) {
                continue;
            }
            const auto reply = encode_reply(*nonce, info_);
            if (!reply.empty() && socket_->send_to(reply, datagram->sender, datagram->port)) {
                ++answered_;
            }
        }
    });
}

BusResponder::~BusResponder() {
    stop_ = true;
    socket_->close();
    thread_.join();
}

ClientRig::ClientRig()
    : env{.reactor = reactor,
          .discovery = bus.factory("10.0.0.2"),
          .lan_cards = [] { return std::vector<LanCard>{}; },
          .remember_addresses = {}} {}

ClientRig::~ClientRig() {
    RemoteConnection::clear_route_memory();
}

} // namespace fv::remote::test
