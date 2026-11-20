#include "remote/detail/client_socket.h"

#include <filesystem>
#include <format>
#include <stdexcept>
#include <utility>

#include "base/identity.h"
#include "base/log.h"
#include "credentials/store.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "remote/client.h"
#include "remote/detail/pyvalue.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "remote/wire.h"

namespace fv::remote::detail {

namespace {

constexpr log::Logger kLog("services.remote_asr.client");

using Kind = net::WsConnectFailure::Kind;

// Past CLOSING_TIMEOUT, how much longer close() waits for the connection to report its end.
constexpr std::chrono::milliseconds kCloseGrace{1'000};
// How much longer than the connect timeout open() waits for the connection to say how it went,
// should the reactor stop before it does.
constexpr std::chrono::milliseconds kOpenGrace{5'000};

bool is_ascii(std::string_view text) {
    for (const char ch : text) {
        if (static_cast<unsigned char>(ch) >= 0x80) {
            return false;
        }
    }
    return true;
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

// str.isspace's ASCII members, which bytes.fromhex skips.
bool ascii_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

} // namespace

struct ClientSocket::State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Message> messages;
    bool opened = false;
    bool closed = false;
    std::optional<net::WsConnectFailure> failure;
    std::optional<std::uint16_t> code;
    std::function<void(std::uint16_t)> close_observer;
};

std::expected<std::shared_ptr<ClientSocket>, net::WsConnectFailure>
ClientSocket::open(net::Reactor& reactor, const std::string& url,
                   std::chrono::milliseconds timeout) {
    std::shared_ptr<ClientSocket> socket(new ClientSocket());
    auto state = std::make_shared<State>();
    socket->state_ = state;

    net::WsClientOptions options;
    options.url = url;
    options.connect_timeout = timeout;
    options.max_message_bytes = MAX_REPLY_BYTES;
    options.close_timeout = CLOSING_TIMEOUT;
    options.on_open = [state] {
        {
            std::lock_guard lock(state->mutex);
            state->opened = true;
        }
        state->changed.notify_all();
    };
    options.on_text = [state](std::string_view text) {
        {
            std::lock_guard lock(state->mutex);
            state->messages.push_back(Message{false, std::string(text)});
        }
        state->changed.notify_all();
    };
    options.on_binary = [state](std::span<const std::byte> data) {
        {
            std::lock_guard lock(state->mutex);
            state->messages.push_back(Message{
                true, std::string(reinterpret_cast<const char*>(data.data()), data.size())});
        }
        state->changed.notify_all();
    };
    options.on_close = [state](std::uint16_t code, std::string_view) {
        std::function<void(std::uint16_t)> observer;
        {
            std::lock_guard lock(state->mutex);
            state->closed = true;
            state->code = code;
            observer = std::move(state->close_observer);
        }
        state->changed.notify_all();
        if (observer) {
            observer(code);
        }
    };
    options.on_connect_failed = [state](const net::WsConnectFailure& failure) {
        {
            std::lock_guard lock(state->mutex);
            state->failure = failure;
            state->closed = true;
        }
        state->changed.notify_all();
    };
    socket->connection_ = net::ws_connect(reactor, std::move(options));

    std::optional<net::WsConnectFailure> failure;
    {
        std::unique_lock lock(state->mutex);
        const bool settled = state->changed.wait_for(lock, timeout + kOpenGrace, [&] {
            return state->opened || state->failure.has_value() || state->closed;
        });
        if (!settled) {
            failure = net::WsConnectFailure{Kind::Timeout, 0, "timed out during opening handshake"};
        } else if (state->failure) {
            failure = state->failure;
        } else if (!state->opened) {
            failure = net::WsConnectFailure{Kind::Unreachable, 0, "the connection closed"};
        }
    }
    if (failure) {
        socket->connection_.reset();
        return std::unexpected(std::move(*failure));
    }
    return socket;
}

ClientSocket::~ClientSocket() {
    connection_.reset();
}

std::string ClientSocket::fingerprint() const {
    return connection_->peer_certificate_sha256().value_or(std::string());
}

std::string ClientSocket::peer_address() const {
    return connection_->remote_endpoint().address;
}

bool ClientSocket::send_text(std::string text) {
    if (!is_open()) {
        return false;
    }
    connection_->send_text(std::make_shared<const std::string>(std::move(text)));
    return true;
}

bool ClientSocket::send_binary(std::vector<std::byte> bytes) {
    if (!is_open()) {
        return false;
    }
    connection_->send_binary(std::move(bytes));
    return true;
}

std::expected<Message, ReceiveError> ClientSocket::receive(std::chrono::milliseconds timeout,
                                                           const CancelToken& cancel) {
    const auto wake = cancel.on_cancel([state = state_] {
        // Synchronize with the predicate check so cancellation cannot miss waking a waiter.
        { std::lock_guard lock(state->mutex); }
        state->changed.notify_all();
    });
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock, timeout, [&] {
        return cancel.cancelled() || !state_->messages.empty() || state_->closed;
    });
    if (cancel.cancelled()) {
        return std::unexpected(ReceiveError::canceled);
    }
    if (!state_->messages.empty()) {
        Message message = std::move(state_->messages.front());
        state_->messages.pop_front();
        return message;
    }
    if (state_->closed) {
        return std::unexpected(ReceiveError::closed);
    }
    return std::unexpected(ReceiveError::timeout);
}

bool ClientSocket::is_open() const {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return false;
        }
    }
    return connection_->is_open();
}

std::optional<std::uint16_t> ClientSocket::close_code() const {
    return connection_->close_code();
}

void ClientSocket::watch_closed(std::function<void(std::uint16_t)> observer) {
    std::optional<std::uint16_t> code;
    {
        std::lock_guard lock(state_->mutex);
        code = state_->code;
        if (!code) {
            state_->close_observer = std::move(observer);
            return;
        }
    }
    observer(*code);
}

void ClientSocket::ping(std::chrono::milliseconds timeout,
                        std::function<void(std::optional<std::chrono::nanoseconds>)> done) {
    connection_->ping(timeout, std::move(done));
}

void ClientSocket::close() {
    connection_->close(1000, "");
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock, CLOSING_TIMEOUT + kCloseGrace, [&] { return state_->closed; });
}

std::string a_product() {
    const std::string_view product = identity().product;
    const bool vowel = !product.empty() && std::string_view("AEIOUaeiou").find(product.front()) !=
                                               std::string_view::npos;
    return std::format("{} {}", vowel ? "an" : "a", product);
}

[[noreturn]] void throw_open_failure(const net::WsConnectFailure& failure, std::string_view host,
                                     int port) {
    const std::string where = format_address(host, port);
    switch (failure.kind) {
    case Kind::HttpStatus:
        throw RemoteEngineError(std::format("{} answered, but it isn't {} remote engine (HTTP {}).",
                                            where, a_product(), failure.http_status));
    case Kind::Handshake:
        throw RemoteEngineError(std::format("{} isn't {} remote engine.", where, a_product()));
    case Kind::Dns:
        throw RemoteUnreachable(std::format("Couldn't find a computer named {}.", host));
    case Kind::Refused:
        throw RemoteUnreachable(
            std::format("Couldn't reach {}: nothing there is accepting connections on that "
                        "port. Check that the host is sharing its engine, on that port.",
                        where),
            true);
    case Kind::Unreachable:
    case Kind::Timeout:
    case Kind::Tls:
    case Kind::Cancelled:
        break;
    }
    throw RemoteUnreachable(std::format("Couldn't reach {}. Check that the host is on, sharing its "
                                        "engine, and allowed through its firewall.",
                                        where));
}

std::shared_ptr<ClientSocket> open_engine_socket(net::Reactor& reactor, std::string_view host,
                                                 int port, std::chrono::milliseconds timeout) {
    auto opened = ClientSocket::open(reactor, engine_uri(host, port), timeout);
    if (!opened) {
        throw_open_failure(opened.error(), host, port);
    }
    return std::move(*opened);
}

void quiet_close(ClientSocket& socket) noexcept {
    try {
        socket.close();
    } catch (const std::exception& error) {
        kLog.debug("Remote engine socket close raised: {}", exception_class(error));
    }
}

std::optional<Json> loads_message(const Message& message) {
    try {
        if (message.binary) {
            const auto text = json_bytes_to_text(as_bytes(message.data));
            if (!text) {
                return std::nullopt; // UnicodeDecodeError, a ValueError
            }
            return std::optional<Json>(std::in_place, py::loads(*text));
        }
        return std::optional<Json>(std::in_place, py::loads(message.data));
    } catch (const py::ValueError&) {
        return std::nullopt;
    } catch (const py::TypeError&) {
        return std::nullopt;
    }
}

Json read_json(ClientSocket& socket, std::chrono::milliseconds timeout, const CancelToken& cancel) {
    auto received = socket.receive(timeout, cancel);
    if (!received) {
        if (received.error() == ReceiveError::canceled) {
            throw std::runtime_error("Transcription canceled");
        }
        if (received.error() == ReceiveError::timeout) {
            throw RemoteEngineError("The host didn't answer in time.");
        }
        throw RemoteEngineError("The host closed the connection.");
    }
    auto message = loads_message(*received);
    if (!message || !message->is_object()) {
        throw RemoteEngineError("The host sent a reply this version can't read.");
    }
    return std::move(*message);
}

bool compare_digest(std::string_view a, std::string_view b) {
    if (!is_ascii(a) || !is_ascii(b)) {
        throw py::TypeError("comparing strings with non-ASCII characters is not supported");
    }
    // Constant time over `b` (the expected value), as hmac.compare_digest is for equal lengths.
    unsigned char difference = a.size() == b.size() ? 0 : 1;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const char left = i < a.size() ? a[i] : '\0';
        difference |= static_cast<unsigned char>(left ^ b[i]);
    }
    return difference == 0;
}

std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    while (true) {
        while (at < text.size() && ascii_space(text[at])) {
            ++at;
        }
        if (at >= text.size()) {
            return out;
        }
        if (at + 1 >= text.size()) {
            return std::nullopt;
        }
        const int high = hex_digit(text[at]);
        const int low = hex_digit(text[at + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>(high * 16 + low));
        at += 2;
    }
}

std::string exception_class(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    if (dynamic_cast<const CredentialStoreTimeout*>(&error) != nullptr) {
        return "CredentialStoreTimeout";
    }
    if (dynamic_cast<const CredentialStoreError*>(&error) != nullptr) {
        return "CredentialStoreError";
    }
    if (dynamic_cast<const std::filesystem::filesystem_error*>(&error) != nullptr) {
        return "OSError";
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return "ValueError";
    }
    if (dynamic_cast<const RemoteUnreachable*>(&error) != nullptr) {
        return "RemoteUnreachable";
    }
    if (dynamic_cast<const RemoteHostBusy*>(&error) != nullptr) {
        return "RemoteHostBusy";
    }
    if (dynamic_cast<const RemoteConnectionLost*>(&error) != nullptr) {
        return "RemoteConnectionLost";
    }
    if (dynamic_cast<const RemoteEngineError*>(&error) != nullptr) {
        return "RemoteEngineError";
    }
    if (dynamic_cast<const RemoteRequestError*>(&error) != nullptr) {
        return "RemoteRequestError";
    }
    return "Exception";
}

} // namespace fv::remote::detail
