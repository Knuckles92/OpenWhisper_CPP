#pragma once

// A SpeechHost on 127.0.0.1 with fake engines and devices, and a blocking client that speaks
// the remote engine's wire with raw frames (remote/protocol.h): OpenWhisper's tests/fakes/remote.py
// host fixture, _pair and _connect, for a host whose C++ client lives in another package.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "net/reactor.h"
#include "net/testing/ws_test_client.h"
#include "pyport/json.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_host/identity.h"
#include "remote_host/registry.h"
#include "remote_host/speech_host.h"
#include "remote_host/testing/fake_host_engine.h"
#include "remote_host/testing/memory_device_store.h"
#include "testkit/testkit.h"

namespace fv::remote_host::rig {

using namespace std::chrono_literals;
using fv::Json;

// Executes a signal's subscriber on the emitting thread, as Python's on_event runs.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

// The host's identity for the whole test binary: one key is enough. It lives in the test's own
// data folder (CTest gives each test one), else in a temporary one.
inline const HostIdentity& test_identity() {
    static const HostIdentity identity = [] {
        if (const auto data = testkit::get_env("OPENWHISPER_CPP_DATA_DIR");
            data && !data->empty()) {
            return ensure_host_identity(std::filesystem::path(*data) / "host", "studio");
        }
        static testkit::TempDataRoot root;
        return ensure_host_identity(root.path() / "host", "studio");
    }();
    return identity;
}

inline bool wait_for(const std::function<bool()>& predicate,
                     std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

// tests/fakes/remote.py _tone: audible audio.
inline std::vector<float> tone(std::size_t samples) {
    std::vector<float> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<float>(0.1 * std::sin(static_cast<float>(i) / 8.0f));
    }
    return out;
}

// One WebSocket to the host, frames sent and read by hand.
class RawClient {
public:
    explicit RawClient(std::uint16_t port, std::string path = std::string(remote::PATH)) {
        net::testing::WsTestClient::Options options;
        options.tls = true;
        options.timeout = 10s;
        connected_ = client_.connect(port, path, options);
    }

    bool connected() const { return connected_; }
    const std::string& error() const { return client_.error(); }
    std::string fingerprint() const { return client_.peer_certificate_sha256(); }

    // A client message, written as OpenWhisper's client writes them (default separators).
    void send(const Json& message) { client_.send_text(remote::dumps_default(message)); }
    void send_text(std::string_view text) { client_.send_text(text); }
    void send_binary(std::span<const std::byte> bytes) { client_.send_binary(bytes); }

    // The next text frame as JSON; nullopt on timeout or once closed.
    std::optional<Json> receive(std::chrono::milliseconds timeout = 10s) {
        auto message = client_.receive(timeout);
        if (!message) {
            return std::nullopt;
        }
        last_text_ = message->data;
        return py::loads(message->data);
    }
    // The raw text of the last frame received.
    const std::string& last_text() const { return last_text_; }

    bool wait_closed(std::chrono::milliseconds timeout = 10s) {
        return client_.wait_closed(timeout);
    }
    std::optional<std::uint16_t> close_code() const { return client_.close_code(); }
    std::string close_reason() const { return client_.close_reason(); }
    void close(std::uint16_t code = 1000) { client_.close(code); }
    void abort() { client_.abort(); }

    // One request frame; the reply. `fields` are the header's besides id and op.
    Json request(std::string_view op, Json fields = Json::object(),
                 std::optional<std::vector<float>> audio = std::nullopt,
                 std::chrono::milliseconds timeout = 10s) {
        Json header = Json::object();
        header["id"] = ++serial_;
        header["op"] = std::string(op);
        for (auto& [key, value] : fields.items()) {
            header[key] = value;
        }
        if (audio) {
            send_binary(remote::pack_request(header, *audio));
        } else {
            send_binary(remote::pack_request(header));
        }
        auto reply = receive(timeout);
        if (!reply) {
            throw std::runtime_error("no reply to " + std::string(op));
        }
        return *reply;
    }
    Json request_payload(std::string_view op, Json fields, std::span<const std::byte> payload) {
        Json header = Json::object();
        header["id"] = ++serial_;
        header["op"] = std::string(op);
        for (auto& [key, value] : fields.items()) {
            header[key] = value;
        }
        send_binary(remote::pack_request(header, payload));
        auto reply = receive();
        if (!reply) {
            throw std::runtime_error("no reply to " + std::string(op));
        }
        return *reply;
    }

    std::int64_t serial() const { return serial_; }

private:
    net::testing::WsTestClient client_;
    bool connected_ = false;
    std::int64_t serial_ = 0;
    std::string last_text_;
};

// What pairing gave the client, or the host's refusal.
struct Paired {
    std::string token;
    std::string device_id;
    Json host;
    std::string fingerprint; // the certificate the socket saw
    std::optional<Json> error;
    std::optional<std::uint16_t> close_code;
    std::string close_reason;
};

inline Paired pair_message(std::uint16_t port, const Json& message) {
    RawClient client(port);
    REQUIRE(client.connected());
    client.send(message);
    Paired out;
    out.fingerprint = client.fingerprint();
    const auto reply = client.receive();
    REQUIRE(reply.has_value());
    if (reply->value("type", "") == "paired") {
        out.token = reply->at("token").get<std::string>();
        out.device_id = reply->at("device_id").get<std::string>();
        out.host = reply->at("host");
    } else {
        out.error = *reply;
    }
    client.wait_closed();
    out.close_code = client.close_code();
    out.close_reason = client.close_reason();
    return out;
}

inline Paired pair_with_code(std::uint16_t port, std::string code,
                             std::string device_name = "laptop") {
    Json message = Json::object();
    message["type"] = "pair";
    message["protocol"] = 1;
    message["device_name"] = device_name;
    message["code"] = std::move(code);
    return pair_message(port, message);
}

inline Paired pair_with_tailscale(std::uint16_t port, std::string device_name = "laptop") {
    Json message = Json::object();
    message["type"] = "pair";
    message["protocol"] = 1;
    message["device_name"] = device_name;
    message["tailscale"] = true;
    return pair_message(port, message);
}

inline Json hello_message(const std::string& token, bool reports_recording = false) {
    Json message = Json::object();
    message["type"] = "hello";
    message["protocol"] = 1;
    message["token"] = token;
    if (reports_recording) {
        message["capabilities"] = Json{{"recording_activity", true}};
    }
    return message;
}

// A paired computer's speech connection after ready.
struct Connected {
    std::unique_ptr<RawClient> client;
    Json ready; // or the refusal
};

inline Connected connect(std::uint16_t port, const std::string& token,
                         bool reports_recording = false) {
    Connected out;
    out.client = std::make_unique<RawClient>(port);
    REQUIRE(out.client->connected());
    out.client->send(hello_message(token, reports_recording));
    const auto reply = out.client->receive();
    REQUIRE(reply.has_value());
    out.ready = *reply;
    return out;
}

// A started host over a FakeHostEngine and a device store in memory (tests/fakes/remote.py's
// host fixture, named "devbox").
struct HostRig {
    explicit HostRig(std::function<void(SpeechHostDeps&)> configure = {},
                     fv::Clock& clock = fv::system_clock(), std::string host_name = "devbox")
        : registry(store.loader(), store.saver(), clock) {
        SpeechHostDeps deps{
            .reactor = reactor,
            .registry = registry,
            .identity = test_identity(),
            .host_name = std::move(host_name),
            .clock = clock,
        };
        current_engine = engine;
        deps.engine_provider = [this] {
            std::lock_guard lock(engine_mutex);
            return current_engine;
        };
        if (configure) {
            configure(deps);
        }
        host = std::make_unique<SpeechHost>(std::move(deps));
        const auto started = host->start(0, "127.0.0.1", std::nullopt);
        REQUIRE(started.has_value());
        port = *started;
    }
    ~HostRig() {
        host->stop();
        host.reset();
    }

    // What the host serves from now on.
    void serve(std::shared_ptr<HostEngine> next) {
        std::lock_guard lock(engine_mutex);
        current_engine = std::move(next);
    }

    Paired pair() { return pair_with_code(port, host->open_pairing()); }
    Connected connected(bool reports_recording = false) {
        const auto paired = pair();
        REQUIRE(!paired.token.empty());
        token = paired.token;
        device_id = paired.device_id;
        auto out = connect(port, token, reports_recording);
        REQUIRE(out.ready.value("type", "") == "ready");
        return out;
    }

    net::Reactor reactor{2};
    testing::MemoryDeviceStore store;
    DeviceRegistry registry;
    std::shared_ptr<testing::FakeHostEngine> engine = std::make_shared<testing::FakeHostEngine>();
    std::mutex engine_mutex;
    std::shared_ptr<HostEngine> current_engine;
    std::unique_ptr<SpeechHost> host;
    std::uint16_t port = 0;
    std::string token;
    std::string device_id;
};

} // namespace fv::remote_host::rig
