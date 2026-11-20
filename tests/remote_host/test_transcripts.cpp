// OpenWhisper's own clients against the C++ host: the client half of each recorded exchange
// (tests/fixtures/transcripts/remote_<scenario>.json, tools/parity/transcripts/) is played to a
// SpeechHost set up as the recorder set up OpenWhisper's, and every frame the host sends must
// equal the recorded one after normalization. Steps where two connections overlap, or where the
// recorder changed the host between two frames, are driven by hooks at those frames.
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "host_rig.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/dispatch.h"
#include "remote_wire/transcript.h"

namespace wire = fv::testing::remote_wire;
using fv::Json;
using fv::remote_host::HostEngine;
using fv::remote_host::SpeechHost;
using fv::remote_host::SpeechHostDeps;
using namespace fv::remote_host::rig;
using namespace std::chrono_literals;

namespace {

const std::vector<std::string> kAddresses = {"192.168.1.20", "100.101.102.103"};
constexpr std::string_view kCode = "246810";

// The recorder's FakeEngine (tests/fakes/remote.py), optionally without a live stream (a
// Parakeet host's) and with a gate that holds the first transcribe.
class RecordedEngine final : public HostEngine {
public:
    explicit RecordedEngine(std::string model = "parakeet-v3", bool streams = true)
        : model_(std::move(model)), streams_(streams) {}

    std::vector<std::string> identity() const override { return {"fake", "parakeet", model_}; }
    Json describe() const override {
        return Json{{"family", "parakeet"}, {"model", model_},    {"label", "Fake Parakeet"},
                    {"device", "cuda"},     {"streaming", false}, {"available", true},
                    {"status", "ready"}};
    }
    Json transcribe(std::span<const float> audio, std::optional<std::string>,
                    std::span<const std::string>) override {
        {
            std::unique_lock lock(mutex_);
            if (gated_) {
                gated_ = false;
                entered_ = true;
                changed_.notify_all();
                changed_.wait_for(lock, 10s, [&] { return released_; });
            }
        }
        const std::string text = "heard " + std::to_string(audio.size());
        const double seconds = static_cast<double>(audio.size()) / 16000.0;
        return Json{
            {"text", text},
            {"segments", Json::array({Json{{"text", text}, {"start", 0.0}, {"end", seconds}}})}};
    }
    Json stream(std::string_view session, std::span<const float> audio,
                std::optional<std::string> language, bool finish) override {
        if (!streams_) {
            return HostEngine::stream(session, audio, std::move(language), finish);
        }
        return Json{
            {"events", Json::array({Json{{"text", "partial " + std::to_string(audio.size())},
                                         {"final", finish}}})}};
    }

    void gate() {
        std::lock_guard lock(mutex_);
        gated_ = true;
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 10s, [&] { return entered_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    const std::string model_;
    const bool streams_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool gated_ = false;
    bool entered_ = false;
    bool released_ = false;
};

// The host side of a recorded scenario: one identity, one device store, any number of hosts.
struct Scenario {
    explicit Scenario(std::string_view name, fv::Clock& clock = fv::system_clock())
        : transcript(wire::load_transcript(name)), clock(clock),
          registry(store.loader(), store.saver(), clock) {
        bindings["<fingerprint#1>"] = test_identity().fingerprint;
    }

    SpeechHost& host(std::function<void(SpeechHostDeps&)> configure = {},
                     std::shared_ptr<HostEngine> engine = std::make_shared<RecordedEngine>()) {
        SpeechHostDeps deps{.reactor = reactor,
                            .registry = registry,
                            .identity = test_identity(),
                            .host_name = "studio",
                            .clock = clock};
        deps.engine_provider = [engine] {
            return engine;
        };
        deps.addresses = [] {
            return kAddresses;
        };
        if (configure) {
            configure(deps);
        }
        hosts.push_back(std::make_unique<SpeechHost>(std::move(deps)));
        auto started = hosts.back()->start(0, "127.0.0.1", std::nullopt);
        REQUIRE(started.has_value());
        return *hosts.back();
    }

    // The first connection of `step`.
    int first_connection(std::string_view step) const {
        for (const auto& frame : transcript.frames) {
            if (frame.step == step) {
                return frame.conn;
            }
        }
        FAIL("no frames in step " << step);
        return -1;
    }
    // The index of connection `conn`'s n-th frame (0-based).
    std::size_t frame(int conn, std::size_t n) const {
        return wire::frames_of(transcript, conn).at(n);
    }

    std::string bound(const std::string& name) {
        std::lock_guard lock(mutex);
        return bindings.value(name, "");
    }

    ~Scenario() {
        for (auto& host : hosts) {
            host->stop();
        }
        hosts.clear();
    }

    wire::Transcript transcript;
    fv::Clock& clock;
    fv::net::Reactor reactor{2};
    fv::remote_host::testing::MemoryDeviceStore store;
    fv::remote_host::DeviceRegistry registry;
    std::vector<std::unique_ptr<SpeechHost>> hosts;
    std::mutex mutex;
    Json bindings = Json::object();
};

struct HttpAnswer {
    int status = 0;
    std::string body;
};

// Plays the client half of one recorded connection and compares what the host sends; `hooks`
// run right before the frame of that index is sent or awaited.
std::vector<std::string> play(Scenario& scenario, std::uint16_t port, int conn,
                              std::map<std::size_t, std::function<void()>> hooks = {}) {
    const wire::Transcript& t = scenario.transcript;
    std::vector<std::string> out;
    const auto indexes = wire::frames_of(t, conn);
    const auto compare = [&](std::size_t index, const wire::WireFrame& actual) {
        std::lock_guard lock(scenario.mutex);
        if (auto diff = wire::compare(t, index, actual, scenario.bindings); !diff.empty()) {
            out.push_back(std::move(diff));
        }
    };
    const wire::WireFrame& first = t.frames.at(indexes.front());
    if (first.kind == "http") {
        RawClient refused(port, first.text);
        if (refused.connected()) {
            out.push_back("the upgrade to " + first.text + " was accepted");
            return out;
        }
        HttpAnswer answer;
        {
            namespace asio = boost::asio;
            asio::io_context io;
            asio::ssl::context context(asio::ssl::context::tls_client);
            context.set_verify_mode(asio::ssl::verify_none);
            asio::ssl::stream<asio::ip::tcp::socket> stream(io, context);
            asio::ip::tcp::resolver resolver(io);
            asio::connect(stream.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
            stream.handshake(asio::ssl::stream_base::client);
            const std::string request =
                "GET " + first.text +
                " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
            asio::write(stream, asio::buffer(request));
            std::string text;
            boost::system::error_code error;
            std::array<char, 4096> buffer{};
            while (!error) {
                const std::size_t n = stream.read_some(asio::buffer(buffer), error);
                text.append(buffer.data(), n);
                const auto head = text.find("\r\n\r\n");
                if (head != std::string::npos &&
                    text.size() >= head + 4 + fv::remote::NOT_AN_ENDPOINT_BODY.size()) {
                    break;
                }
            }
            answer.status = text.starts_with("HTTP/1.1 ") ? std::stoi(text.substr(9, 3)) : 0;
            const auto head = text.find("\r\n\r\n");
            answer.body = head == std::string::npos ? "" : text.substr(head + 4);
        }
        for (std::size_t index : indexes) {
            if (t.frames[index].dir == "s2c") {
                compare(index, wire::http_frame("s2c", answer.status, answer.body));
            }
        }
        return out;
    }

    fv::net::testing::WsTestClient client;
    fv::net::testing::WsTestClient::Options options;
    options.tls = true;
    if (!client.connect(port, fv::remote::PATH, options)) {
        out.push_back("connection " + std::to_string(conn) + " failed: " + client.error());
        return out;
    }
    bool closed = false;
    bool closing = false;
    for (std::size_t index : indexes) {
        if (const auto hook = hooks.find(index); hook != hooks.end()) {
            hook->second();
        }
        const wire::WireFrame& recorded = t.frames[index];
        if (recorded.dir == "c2s") {
            if (recorded.kind == "close") {
                if (!recorded.echo && !closed) {
                    client.close(static_cast<std::uint16_t>(recorded.code), recorded.reason);
                    closing = true;
                }
                continue;
            }
            if (closed) {
                out.push_back("closed before frame " + std::to_string(index) + " could be sent");
                break;
            }
            wire::WireFrame frame;
            {
                std::lock_guard lock(scenario.mutex);
                frame = wire::render(t, index, scenario.bindings);
            }
            if (frame.kind == "text") {
                client.send_text(frame.text);
            } else {
                const auto bytes = wire::wire_bytes(frame);
                client.send_binary(bytes);
            }
            continue;
        }
        if (recorded.kind == "close") {
            if (!closing) {
                client.wait_closed(10s);
            }
            closed = true;
            compare(index, wire::close_frame("s2c", client.close_code().value_or(1006),
                                             client.close_reason()));
            continue;
        }
        const auto message = client.receive(10s);
        if (!message) {
            out.push_back("frame " + std::to_string(index) + ": nothing arrived (close " +
                          std::to_string(client.close_code().value_or(0)) + " " +
                          client.close_reason() + ")");
            break;
        }
        if (message->binary) {
            const auto* bytes = reinterpret_cast<const std::byte*>(message->data.data());
            compare(index, wire::binary_frame("s2c", std::span(bytes, message->data.size())));
        } else {
            compare(index, wire::text_frame("s2c", message->data));
        }
    }
    if (!closed && !closing) {
        client.close();
    }
    return out;
}

void check(const std::vector<std::string>& differences) {
    for (const auto& difference : differences) {
        UNSCOPED_INFO(difference);
    }
    CHECK(differences.empty());
}

// Each connection of `step` in order, on `host`.
void play_step(Scenario& scenario, std::string_view step, SpeechHost& host,
               std::map<std::size_t, std::function<void()>> hooks = {}) {
    for (int conn : wire::connections(scenario.transcript)) {
        const auto frames = wire::frames_of(scenario.transcript, conn);
        if (scenario.transcript.frames[frames.front()].step == step) {
            check(play(scenario, *host.port(), conn, hooks));
        }
    }
}

const Json kModels = Json::array(
    {Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Parakeet TDT 0.6B v3"}},
     Json{{"family", "parakeet"}, {"model", "parakeet-v2"}, {"label", "Parakeet TDT 0.6B v2"}}});
const Json kRuntime = Json{{"family", "parakeet"},
                           {"model", "parakeet-v3"},
                           {"selected", {{"device", "auto"}, {"language", "auto"}}},
                           {"devices", Json::array({"auto", "cpu", "cuda"})},
                           {"compute_types", Json::object()},
                           {"languages", Json::array({"auto", "en", "de"})},
                           {"gpu", {{"name", "NVIDIA GeForce RTX 4070"}, {"total_mib", 12282}}},
                           {"dependencies", Json::array()}};

} // namespace

TEST_CASE("the host half of remote_probe") {
    Scenario scenario("probe");
    auto engine = std::make_shared<fv::remote_host::testing::FakeHostEngine>();
    std::mutex mutex;
    std::string owner;
    auto& host = scenario.host(
        [&](SpeechHostDeps& deps) {
            deps.tailscale_owner = [&] {
                std::lock_guard lock(mutex);
                return owner;
            };
        },
        engine);
    play_step(scenario, "available", host);
    engine->set_available(false);
    play_step(scenario, "unavailable", host);
    engine->set_available(true);
    {
        std::lock_guard lock(mutex);
        owner = "owner@example.com";
    }
    play_step(scenario, "tailscale_owner", host);
}

TEST_CASE("the host half of remote_wrong_path") {
    Scenario scenario("wrong_path");
    play_step(scenario, "wrong_path", scenario.host());
}

TEST_CASE("the host half of remote_protocol") {
    Scenario scenario("protocol");
    for (const char* step : {"protocol_2", "pair_protocol_2", "binary_first", "not_json",
                             "not_an_object", "unknown_type"}) {
        play_step(scenario, step, scenario.host());
    }
    play_step(scenario, "silence",
              scenario.host([](SpeechHostDeps& deps) { deps.limits.handshake = 500ms; }));
}

TEST_CASE("the host half of remote_pair_code") {
    Scenario scenario("pair_code");
    play_step(scenario, "pairing_closed", scenario.host());
    for (const char* step : {"wrong_code", "fifth_wrong_code", "right_code", "spaced_code"}) {
        auto& host = scenario.host();
        host.open_pairing(std::string(kCode));
        play_step(scenario, step, host);
    }
    // Each raw code has its own host.
    const int first = scenario.first_connection("raw_codes");
    for (int conn : {first, first + 1}) {
        auto& host = scenario.host();
        host.open_pairing(std::string(kCode));
        check(play(scenario, *host.port(), conn));
    }
    CHECK(scenario.store.devices().size() == 3);
}

TEST_CASE("the host half of remote_pair_tailscale") {
    Scenario scenario("pair_tailscale");
    fv::remote::testing::FakeTailscaleCli cli;
    const auto owner = [](SpeechHostDeps& deps) {
        deps.tailscale_owner = [] {
            return std::string("owner@example.com");
        };
    };
    play_step(scenario, "no_owner",
              scenario.host([&](SpeechHostDeps& deps) { deps.tailscale = &cli; }));
    // 127.0.0.1 is not a tailnet address: whois is never asked.
    play_step(scenario, "not_owners_computer", scenario.host([&](SpeechHostDeps& deps) {
        owner(deps);
        deps.tailscale = &cli;
    }));
    CHECK(cli.looked_up().empty());
    play_step(scenario, "owners_computer", scenario.host([&](SpeechHostDeps& deps) {
        owner(deps);
        deps.own_device = [](std::string_view, std::string_view, std::string_view login) {
            return std::optional<fv::remote::PeerIdentity>(
                fv::remote::PeerIdentity{"laptop-node", std::string(login), false});
        };
    }));
    REQUIRE(scenario.store.devices().size() == 1);
    CHECK(scenario.store.devices()[0].at("via") == "tailscale");
}

TEST_CASE("the host half of remote_pair_approval") {
    fv::FakeClock clock;
    Scenario scenario("pair_approval", clock);
    const auto answer_when_shown = [](SpeechHost& host, bool allow) {
        return [&host, allow] {
            REQUIRE(wait_for([&] { return host.pending_request().has_value(); }, 5s));
            host.answer_request(host.pending_request()->id, allow);
        };
    };
    {
        auto& host = scenario.host();
        const int conn = scenario.first_connection("allowed");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 4), answer_when_shown(host, true)}}));
    }
    {
        auto& host = scenario.host();
        const int conn = scenario.first_connection("denied");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 4), answer_when_shown(host, false)}}));
    }
    {
        auto& host = scenario.host();
        const int conn = scenario.first_connection("timeout");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 4), [&] {
                         REQUIRE(wait_for([&] { return host.pending_request().has_value(); }, 5s));
                         clock.advance(121s);
                     }}}));
    }
    {
        auto& host = scenario.host();
        const int conn = scenario.first_connection("canceled");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 4), [&] {
                         REQUIRE(wait_for([&] { return host.pending_request().has_value(); }, 5s));
                     }}}));
        CHECK(wait_for([&] { return !host.pending_request().has_value(); }));
    }
    play_step(scenario, "broken_reveal", scenario.host());
    play_step(scenario, "refused", scenario.host([](SpeechHostDeps& deps) {
        deps.peer_address = [](std::string_view) {
            return std::string("8.8.8.8");
        };
    }));
    {
        // Four requests from 127.0.0.1 made just now (each turned away at its nonce).
        auto& host = scenario.host();
        for (int made = 0; made < 4;) {
            RawClient client(*host.port());
            client.send(Json{{"type", "pair_request"}, {"protocol", 1}, {"device_name", "laptop"}});
            const auto reply = client.receive();
            REQUIRE(reply.has_value());
            if (reply->value("type", "") == "pair_commit") {
                client.send(Json{{"type", "pair_nonce"}, {"nonce", "zz"}});
                client.wait_closed();
                ++made;
            } else {
                REQUIRE(reply->value("code", "") == "approval_busy");
                std::this_thread::sleep_for(20ms);
            }
        }
        play_step(scenario, "limited", host);
    }
    {
        auto& host = scenario.host();
        const int first = scenario.first_connection("busy");
        std::promise<void> second_done;
        auto waited = second_done.get_future().share();
        auto background = std::async(std::launch::async, [&] {
            return play(scenario, *host.port(), first,
                        {{scenario.frame(first, 4), [&] {
                              waited.wait();
                              host.answer_request(host.pending_request()->id, false);
                          }}});
        });
        REQUIRE(wait_for([&] { return host.pending_request().has_value(); }, 5s));
        check(play(scenario, *host.port(), first + 1));
        second_done.set_value();
        check(background.get());
    }
    {
        auto& host = scenario.host();
        const int conn = scenario.first_connection("stopped");
        std::future<void> stopping;
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 4), [&] {
                         REQUIRE(wait_for([&] { return host.pending_request().has_value(); }, 5s));
                         stopping = std::async(std::launch::async, [&] { host.stop(); });
                     }}}));
        stopping.get();
    }
}

namespace {

// remote_hello_ready.py full_host: every optional callback, both permissions on.
void full_host(SpeechHostDeps& deps) {
    deps.models = [] {
        return kModels;
    };
    deps.runtime = [] {
        return kRuntime;
    };
    deps.select_model = [](std::string, std::string, std::string, std::optional<std::string>) {
        return Json::object();
    };
    deps.model_management = [] {
        return true;
    };
    deps.manage_models = [](std::string_view, const Json&, std::string_view) {
        return Json::object();
    };
    deps.configure_runtime = [](std::string, std::string, Json, std::string) {
        return Json::object();
    };
    deps.records = [](std::string_view, const Json&, std::span<const std::byte>,
                      const fv::remote::PairedDevice&) {
        return Json::object();
    };
    deps.records_enabled = [] {
        return true;
    };
    deps.records_summary = [](std::string_view) {
        return Json{{"dictation", {{"count", 3}, {"bytes", 30720}}},
                    {"meeting", {{"count", 1}, {"bytes", 4096}}}};
    };
    deps.mcp = [](std::string_view, const Json&, std::string_view) {
        return Json::object();
    };
    deps.mcp_enabled = [] {
        return true;
    };
}

} // namespace

TEST_CASE("the host half of remote_hello_ready") {
    Scenario scenario("hello_ready");
    auto& host = scenario.host(full_host);
    host.open_pairing(std::string(kCode));
    play_step(scenario, "pair", host);
    play_step(scenario, "ready_full", host);
    play_step(scenario, "ready_plain", scenario.host());
    play_step(scenario, "unauthorized", host);
    play_step(scenario, "loading", scenario.host([](SpeechHostDeps& deps) {
        full_host(deps);
        deps.engine_settled = [] {
            return false;
        };
    }));
    REQUIRE(wait_for([&] { return host.connected_clients().empty(); }));
    const Json hold = host.prepare_update();
    REQUIRE(hold.contains("token"));
    CHECK(hold.at("expires_in") == 60.0);
    play_step(scenario, "update_hold", host);
    CHECK(host.cancel_update(hold.at("token").get<std::string>()));
    {
        auto& busy = scenario.host([](SpeechHostDeps& deps) { deps.limits.max_connections = 1; });
        const int first = scenario.first_connection("busy_connections");
        std::promise<void> second_done;
        auto waited = second_done.get_future().share();
        auto background = std::async(std::launch::async, [&] {
            return play(scenario, *busy.port(), first, {{scenario.frame(first, 2), [&] {
                                                             waited.wait();
                                                         }}});
        });
        REQUIRE(wait_for([&] { return busy.connected_clients().size() == 1; }));
        check(play(scenario, *busy.port(), first + 1));
        second_done.set_value();
        check(background.get());
    }
    {
        auto& busy = scenario.host([](SpeechHostDeps& deps) { deps.limits.max_handshakes = 1; });
        const int first = scenario.first_connection("busy_handshakes");
        std::promise<void> second_done;
        auto waited = second_done.get_future().share();
        auto background = std::async(std::launch::async, [&] {
            return play(scenario, *busy.port(), first, {{scenario.frame(first, 0), [&] {
                                                             waited.wait();
                                                         }}});
        });
        REQUIRE(wait_for([&] { return busy.free_handshake_slots() == 0; }));
        check(play(scenario, *busy.port(), first + 1));
        second_done.set_value();
        check(background.get());
    }
}

namespace {

const Json kSwitched =
    Json{{"family", "parakeet"}, {"model", "parakeet-v2"}, {"label", "Parakeet TDT 0.6B v2"},
         {"device", "cuda"},     {"streaming", false},     {"available", true},
         {"status", "ready"}};
const Json kCatalog = Json{
    {"families", Json::array({Json{{"family", "parakeet"},
                                   {"models", Json::array({Json{{"model", "parakeet-v3"},
                                                                {"label", "Parakeet TDT 0.6B v3"},
                                                                {"downloaded", true}}})}}})}};
const Json kMcpState =
    Json{{"enabled", true}, {"port", 8765}, {"url", "http://127.0.0.1:8765/mcp"}};
const Json kRecordsStat = Json{{"dictation", {{"count", 3}, {"bytes", 30720}}},
                               {"meeting", {{"count", 0}, {"bytes", 0}}}};

struct Permissions {
    std::atomic<bool> models{false};
    std::atomic<bool> records{false};
    std::atomic<bool> mcp{false};
    std::atomic<bool> settled{true};
};

// remote_requests.py build_host: every optional callback, permissions switched per step.
std::function<void(SpeechHostDeps&)> requests_host(Permissions& permissions) {
    return [&permissions](SpeechHostDeps& deps) {
        deps.models = [] {
            return kModels;
        };
        deps.runtime = [] {
            return kRuntime;
        };
        deps.select_model = [](std::string, std::string model, std::string,
                               std::optional<std::string>) -> Json {
            if (model == "parakeet-missing") {
                throw std::runtime_error("That model isn't downloaded on the host.");
            }
            return kSwitched;
        };
        deps.model_management = [&permissions] {
            return permissions.models.load();
        };
        deps.manage_models = [](std::string_view op, const Json& header, std::string_view) -> Json {
            if (op == "model_catalog") {
                return kCatalog;
            }
            return Json{
                {"job", op}, {"family", header.at("family")}, {"model", header.at("model")}};
        };
        deps.configure_runtime = [](std::string, std::string, Json, std::string) {
            return kSwitched;
        };
        deps.records = [](std::string_view op, const Json&, std::span<const std::byte>,
                          const fv::remote::PairedDevice&) -> Json {
            if (op == "records_stat") {
                return kRecordsStat;
            }
            if (op == "records_open") {
                throw fv::remote_host::RecordNotFound("");
            }
            if (op == "records_put") {
                throw fv::remote_host::RecordsBusy(
                    "Another upload is still being written. Try again shortly.");
            }
            if (op == "records_delete") {
                throw fv::py::ValueError("Invalid record ID.");
            }
            return Json::object();
        };
        deps.records_enabled = [&permissions] {
            return permissions.records.load();
        };
        deps.records_summary = [](std::string_view) {
            return kRecordsStat;
        };
        deps.mcp = [](std::string_view op, const Json& header, std::string_view) -> Json {
            if (op == "mcp_configure") {
                const Json& port = header.at("settings").value("port", Json());
                if (!port.is_number_integer() && !port.is_boolean()) {
                    throw fv::py::ValueError("invalid_port: The port must be a whole number.");
                }
                Json state = kMcpState;
                state["port"] = port;
                return state;
            }
            return kMcpState;
        };
        deps.mcp_enabled = [&permissions] {
            return permissions.mcp.load();
        };
        deps.engine_settled = [&permissions] {
            return permissions.settled.load();
        };
    };
}

} // namespace

TEST_CASE("the host half of remote_requests") {
    Scenario scenario("requests");
    Permissions permissions;
    auto& host = scenario.host(requests_host(permissions));
    auto& bare = scenario.host();
    host.open_pairing(std::string(kCode));
    play_step(scenario, "pair", host);
    play_step(scenario, "transcribe", host);
    play_step(scenario, "bad_frames", host);
    play_step(scenario, "stream", host);
    play_step(scenario, "stream_refused",
              scenario.host({}, std::make_shared<RecordedEngine>("parakeet-v3", false)));

    {
        const int conn = scenario.first_connection("engine_loading");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 2),
                     [&] {
                         permissions.settled = false;
                     }},
                    {scenario.frame(conn, 8), [&] {
                         permissions.settled = true;
                     }}}));
    }
    {
        const int conn = scenario.first_connection("update_hold");
        Json hold;
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 2),
                     [&] {
                         REQUIRE(wait_for([&] { return host.connected_clients().size() == 1; }));
                         hold = host.prepare_update();
                     }},
                    {scenario.frame(conn, 10), [&] {
                         CHECK(host.cancel_update(hold.value("token", "")));
                     }}}));
        CHECK(hold.at("expires_in") == 60.0);
    }
    {
        auto engine = std::make_shared<RecordedEngine>();
        engine->gate();
        auto& busy = scenario.host(
            [](SpeechHostDeps& deps) {
                deps.limits.max_active_jobs = 1;
                deps.limits.max_queued_jobs = 0;
            },
            engine);
        const int first = scenario.first_connection("busy");
        std::promise<void> second_done;
        auto waited = second_done.get_future().share();
        auto background = std::async(std::launch::async, [&] {
            return play(scenario, *busy.port(), first, {{scenario.frame(first, 4), [&] {
                                                             waited.wait();
                                                         }}});
        });
        REQUIRE(engine->wait_entered());
        check(play(scenario, *busy.port(), first + 1));
        engine->release();
        second_done.set_value();
        check(background.get());
    }
    {
        std::mutex mutex;
        std::shared_ptr<HostEngine> engine = std::make_shared<RecordedEngine>();
        auto& changing = scenario.host([&](SpeechHostDeps& deps) {
            deps.engine_provider = [&] {
                std::lock_guard lock(mutex);
                return engine;
            };
        });
        const int conn = scenario.first_connection("engine_changed");
        check(play(scenario, *changing.port(), conn,
                   {{scenario.frame(conn, 2), [&] {
                         std::lock_guard lock(mutex);
                         engine = std::make_shared<RecordedEngine>("parakeet-v2");
                     }}}));
    }
    {
        const int conn = scenario.first_connection("select_model");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 10),
                     [&] {
                         permissions.models = true;
                     }},
                    {scenario.frame(conn, 16), [&] {
                         permissions.models = false;
                     }}}));
        check(play(scenario, *bare.port(), conn + 1));
    }
    {
        const int conn = scenario.first_connection("configure_runtime");
        check(play(scenario, *host.port(), conn));
        check(play(scenario, *bare.port(), conn + 1));
    }
    {
        const int conn = scenario.first_connection("model_ops");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 4),
                     [&] {
                         permissions.models = true;
                     }},
                    {scenario.frame(conn, 18), [&] {
                         permissions.models = false;
                     }}}));
    }
    {
        const int conn = scenario.first_connection("recording");
        check(play(scenario, *host.port(), conn));
        check(play(scenario, *host.port(), conn + 1));
    }
    {
        const int conn = scenario.first_connection("records_and_mcp");
        check(play(scenario, *bare.port(), conn));
        check(play(scenario, *host.port(), conn + 1,
                   {{scenario.frame(conn + 1, 10),
                     [&] {
                         permissions.records = true;
                     }},
                    {scenario.frame(conn + 1, 14),
                     [&] {
                         permissions.records = false;
                     }},
                    {scenario.frame(conn + 1, 16),
                     [&] {
                         permissions.mcp = true;
                     }},
                    {scenario.frame(conn + 1, 26), [&] {
                         permissions.mcp = false;
                     }}}));
    }
    {
        const int conn = scenario.first_connection("device_removed");
        check(play(scenario, *host.port(), conn,
                   {{scenario.frame(conn, 2), [&] {
                         CHECK(host.remove_device(scenario.bound("<device_id#1>")));
                     }}}));
    }
}
