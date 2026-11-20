// The host after pairing (services/remote_asr/host.py SpeechHost): hello and ready, requests,
// engine changes, the update hold, permissions rechecked per request, and what the host tells
// its dashboard, over loopback TLS with fake engines. Ported from tests/test_remote_engine.py,
// test_remote_engine_push.py, test_host_mode.py, test_remote_records.py,
// test_remote_mcp_control.py, test_remote_model_management.py, test_remote_dependencies.py,
// test_remote_runtime.py, test_remote_rename.py and test_personalize_s4_remote.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
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
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "host_rig.h"
#include "remote_host/dispatch.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using fv::Json;
using fv::remote_host::HostEngine;
using fv::remote_host::SpeechHostDeps;
using namespace fv::remote_host::rig;
using namespace std::chrono_literals;

namespace {

// tests/fakes/remote.py FakeEngine with a gate: an armed transcribe waits until released, and
// can watch the host while it decodes.
class GateEngine final : public HostEngine {
public:
    std::vector<std::string> identity() const override {
        std::lock_guard lock(mutex_);
        return {"fake", "parakeet", model_};
    }
    Json describe() const override {
        std::lock_guard lock(mutex_);
        return Json{{"family", "parakeet"}, {"model", model_},    {"label", "Fake Parakeet"},
                    {"device", "cuda"},     {"streaming", false}, {"available", true},
                    {"status", "ready"}};
    }
    Json transcribe(std::span<const float> audio, std::optional<std::string>,
                    std::span<const std::string>) override {
        std::function<void()> watch;
        {
            std::unique_lock lock(mutex_);
            ++calls_;
            watch = watch_;
            if (armed_) {
                entered_ = true;
                changed_.notify_all();
                changed_.wait_for(lock, 5s, [&] { return released_; });
                return Json{{"text", "ok"}, {"segments", Json::array()}};
            }
        }
        if (watch) {
            watch();
        }
        const std::string text = "heard " + std::to_string(audio.size());
        return Json{{"text", text}, {"segments", Json::array()}};
    }

    void arm() {
        std::lock_guard lock(mutex_);
        armed_ = true;
        released_ = false;
        entered_ = false;
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [&] { return entered_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    void set_model(std::string model) {
        std::lock_guard lock(mutex_);
        model_ = std::move(model);
    }
    void set_watch(std::function<void()> watch) {
        std::lock_guard lock(mutex_);
        watch_ = std::move(watch);
    }
    int calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string model_ = "parakeet-v3";
    bool armed_ = false;
    bool entered_ = false;
    bool released_ = false;
    int calls_ = 0;
    std::function<void()> watch_;
};

// Events as the host emitted them, on the emitting thread.
struct EventLog {
    explicit EventLog(fv::remote_host::SpeechHost& host)
        : subscription(host.events.connect(executor, [this](std::string kind, Json detail) {
              std::lock_guard lock(mutex);
              events.emplace_back(std::move(kind), std::move(detail));
          })) {}

    std::vector<Json> of(std::string_view kind) {
        std::lock_guard lock(mutex);
        std::vector<Json> out;
        for (const auto& [name, detail] : events) {
            if (name == kind) {
                out.push_back(detail);
            }
        }
        return out;
    }
    std::set<std::string> kinds() {
        std::lock_guard lock(mutex);
        std::set<std::string> out;
        for (const auto& [name, detail] : events) {
            out.insert(name);
        }
        return out;
    }

    InlineExecutor executor;
    std::mutex mutex;
    std::vector<std::pair<std::string, Json>> events;
    fv::Subscription subscription;
};

const Json kHostModels = Json::array({
    Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Parakeet TDT 0.6B v3"}},
    Json{{"family", "nemotron"}, {"model", "nemotron-3.5"}, {"label", "Nemotron 3.5 ASR 0.6B"}},
    Json{{"family", "local_whisper"}, {"model", "small"}, {"label", "Whisper small"}},
});

// What a refused upgrade answers, read with a plain TLS client (ws clients report the status
// only).
struct HttpAnswer {
    int status = 0;
    std::string body;
};

HttpAnswer https_get(std::uint16_t port, const std::string& path, bool upgrade) {
    namespace asio = boost::asio;
    asio::io_context io;
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, context);
    asio::ip::tcp::resolver resolver(io);
    asio::connect(stream.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
    stream.handshake(asio::ssl::stream_base::client);
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (upgrade) {
        request += "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    } else {
        request += "Connection: close\r\n";
    }
    request += "\r\n";
    asio::write(stream, asio::buffer(request));
    std::string answer;
    boost::system::error_code error;
    std::array<char, 4096> buffer{};
    for (;;) {
        const std::size_t n = stream.read_some(asio::buffer(buffer), error);
        answer.append(buffer.data(), n);
        if (error) {
            break;
        }
        const auto head_end = answer.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const auto length_at = answer.find("content-length: ");
            const auto length_at_upper = answer.find("Content-Length: ");
            const auto at = length_at != std::string::npos ? length_at : length_at_upper;
            if (at != std::string::npos && at < head_end) {
                const std::size_t length = std::stoul(answer.substr(at + 16));
                if (answer.size() >= head_end + 4 + length) {
                    break;
                }
            }
        }
    }
    HttpAnswer out;
    if (answer.starts_with("HTTP/1.1 ")) {
        out.status = std::stoi(answer.substr(9, 3));
    }
    const auto head_end = answer.find("\r\n\r\n");
    if (head_end != std::string::npos) {
        out.body = answer.substr(head_end + 4);
    }
    return out;
}

std::string error_of(const Json& reply) {
    return reply.value("error", "");
}

} // namespace

// ---- hello and the path
// ---------------------------------------------------------------------------

TEST_CASE("test_unknown_token_is_refused") {
    HostRig rig;
    auto refused = connect(rig.port, "not-a-token");
    CHECK(refused.ready == Json{{"type", "error"},
                                {"code", "unauthorized"},
                                {"message", "This computer isn't paired with the host anymore. "
                                            "Pair it again."}});
    CHECK(refused.client->wait_closed());
    CHECK(refused.client->close_code() == fv::remote::CLOSE_UNAUTHORIZED);
    CHECK(refused.client->close_reason() == "unauthorized");
}

// DeviceRegistry.authenticate turns down a token that is not a non-empty str before hashing it,
// so the client hears "unauthorized" (4401), not a dropped connection (1011).
TEST_CASE("a token that is not a string is refused as unknown") {
    HostRig rig;
    REQUIRE(!rig.pair().token.empty());
    for (const Json& token : {Json(123456), Json::array({"x"}), Json(true), Json(""), Json()}) {
        RawClient client(rig.port);
        REQUIRE(client.connected());
        Json hello = Json::object();
        hello["type"] = "hello";
        hello["protocol"] = 1;
        hello["token"] = token;
        client.send(hello);
        const auto reply = client.receive();
        REQUIRE(reply.has_value());
        CHECK(reply->value("code", "") == "unauthorized");
        CHECK(client.wait_closed());
        CHECK(client.close_code() == fv::remote::CLOSE_UNAUTHORIZED);
        CHECK(client.close_reason() == "unauthorized");
    }
    CHECK(rig.host->connected_clients().empty());
}

TEST_CASE("a port past 16 bits is refused, not cut down") {
    HostRig rig;
    rig.host->stop();
    const auto started = rig.host->start(65536, "127.0.0.1", std::nullopt);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == "bind(): port must be 0-65535.");
    CHECK_FALSE(rig.host->running());
    CHECK(rig.host->start(-1, "127.0.0.1", std::nullopt).error() ==
          "bind(): port must be 0-65535.");
}

TEST_CASE("test_wrong_path_is_not_mistaken_for_a_host") {
    HostRig rig;
    RawClient other(rig.port, "/other");
    CHECK_FALSE(other.connected());
    CHECK_THAT(other.error(), ContainsSubstring("404"));
    const auto answer = https_get(rig.port, "/other", true);
    CHECK(answer.status == 404);
    CHECK(answer.body == fv::remote::NOT_AN_ENDPOINT_BODY);
    // The engine's path with a query is still the engine's.
    RawClient with_query(rig.port, std::string(fv::remote::PATH) + "?x=1");
    CHECK(with_query.connected());
}

TEST_CASE("a plain request is no remote engine either") {
    HostRig rig;
    CHECK(https_get(rig.port, "/other", false).status == 404);
    const auto endpoint = https_get(rig.port, std::string(fv::remote::PATH), false);
    CHECK(endpoint.status == 426);
    CHECK_THAT(endpoint.body, ContainsSubstring("Failed to open a WebSocket connection"));
}

TEST_CASE("test_probe_answers_without_a_token_and_reveals_no_secrets") {
    std::mutex mutex;
    std::string owner = "owner@example.com";
    HostRig rig([&](SpeechHostDeps& deps) {
        deps.tailscale_owner = [&] {
            std::lock_guard lock(mutex);
            return owner;
        };
    });
    const auto probe = [&] {
        RawClient client(rig.port);
        client.send(Json{{"type", "probe"}, {"protocol", 1}});
        auto answer = client.receive();
        REQUIRE(answer.has_value());
        client.wait_closed();
        CHECK(client.close_code() == 1000);
        return *answer;
    };
    const Json answer = probe();
    CHECK(answer == Json{{"type", "probe"},
                         {"protocol", 1},
                         {"host", {{"name", "devbox"}}},
                         {"engine",
                          {{"label", "Fake Parakeet"},
                           {"device", "cuda"},
                           {"available", true},
                           {"family", "parakeet"}}},
                         {"tailscale_pairing", true},
                         {"approval", true}});
    CHECK(rig.host->connected_clients().empty());
    {
        std::lock_guard lock(mutex);
        owner.clear();
    }
    CHECK(probe().at("tailscale_pairing") == false);
}

// Sharing as the app shares: every interface, which serves on this computer's network and may
// open a firewall prompt (amendment 14). Written for the owner; never run by a worker.
TEST_CASE("a host shared on every interface pairs and answers on loopback", "[interactive]") {
    HostRig rig;
    rig.host->stop();
    const auto started = rig.host->start(0, "0.0.0.0", std::nullopt);
    REQUIRE(started.has_value());
    rig.port = *started;
    auto connection = rig.connected();
    CHECK(connection.client->request("transcribe", Json::object(), tone(160))
              .at("result")
              .at("text") == "heard 160");
}

TEST_CASE("two hosts cannot share a port") {
    HostRig rig;
    fv::remote_host::testing::MemoryDeviceStore store;
    fv::remote_host::DeviceRegistry registry(store.loader(), store.saver(), fv::system_clock());
    SpeechHostDeps deps{.reactor = rig.reactor,
                        .registry = registry,
                        .identity = test_identity(),
                        .host_name = "other",
                        .clock = fv::system_clock()};
    deps.engine_provider = [] {
        return std::make_shared<fv::remote_host::testing::FakeHostEngine>();
    };
    fv::remote_host::SpeechHost second(std::move(deps));
    const auto started = second.start(rig.port, "127.0.0.1", std::nullopt);
    REQUIRE_FALSE(started.has_value());
    CHECK_FALSE(started.error().empty());
    CHECK_FALSE(second.running());
}

// ---- requests -----------------------------------------------------------------------------------

TEST_CASE("test_transcribe_round_trip_carries_audio_and_language") {
    HostRig rig;
    auto connection = rig.connected();
    CHECK(connection.ready.at("engine").at("family") == "parakeet");
    CHECK(connection.ready.at("device").at("name") == "laptop");
    std::vector<float> audio(16000);
    for (int i = 0; i < 16000; ++i) {
        audio[static_cast<std::size_t>(i)] = static_cast<float>(i - 8000) / 32768.0f;
    }
    const Json reply = connection.client->request("transcribe", Json{{"language", "en"}}, audio);
    CHECK(reply.at("result").at("text") == "heard 16000");
    const auto calls = rig.engine->calls();
    REQUIRE(!calls.empty());
    CHECK(calls.back().op == "transcribe");
    CHECK(calls.back().language == "en");
    CHECK(calls.back().audio == audio);
}

TEST_CASE("test_streams_are_scoped_per_connection_and_canceled_on_disconnect") {
    HostRig rig;
    const auto paired = rig.pair();
    auto first = connect(rig.port, paired.token);
    auto second = connect(rig.port, paired.token);
    first.client->request("stream", Json{{"session", "preview"}, {"finish", false}},
                          std::vector<float>(800, 0.0f));
    second.client->request("stream", Json{{"session", "preview"}, {"finish", false}},
                           std::vector<float>(1600, 0.0f));
    std::set<std::string> sessions;
    for (const auto& call : rig.engine->calls()) {
        if (call.op == "stream") {
            sessions.insert(call.session);
        }
    }
    CHECK(sessions.size() == 2);
    for (const auto& session : sessions) {
        CHECK(session.ends_with("-preview"));
        CHECK(session.starts_with("remote-"));
    }
    first.client->close();
    second.client->close();
    CHECK(wait_for([&] { return rig.engine->canceled().size() == 2; }));
}

TEST_CASE("test_engine_errors_come_back_as_request_errors") {
    HostRig rig;
    auto connection = rig.connected();
    auto none = fv::remote_host::unavailable_engine("x");
    CHECK_THROWS_WITH(none->stream("s", std::vector<float>(1), std::nullopt, false),
                      ContainsSubstring("no live stream"));
    const Json reply = connection.client->request("explode");
    CHECK(reply == Json{{"id", 1}, {"error", "Unknown operation: 'explode'"}});
    // The connection survives an error.
    CHECK(connection.client->request("describe").at("result").at("model") == "parakeet-v3");
}

TEST_CASE("test_switching_the_host_engine_tells_clients_to_reconnect") {
    HostRig rig;
    auto connection = rig.connected();
    rig.engine->set_model("parakeet-other");
    const Json reply =
        connection.client->request("transcribe", Json::object(), std::vector<float>(160, 0.0f));
    CHECK(reply == Json{{"id", 1},
                        {"code", "engine_changed"},
                        {"error", "The host switched speech engines. Reconnecting."}});
    CHECK(connection.client->wait_closed());
    CHECK(connection.client->close_code() == fv::remote::CLOSE_ENGINE_CHANGED);
}

TEST_CASE("test_removing_a_device_drops_its_connection") {
    HostRig rig;
    auto connection = rig.connected();
    const auto devices = rig.registry.list();
    REQUIRE(devices.size() == 1);
    CHECK(rig.host->remove_device(devices[0].id));
    CHECK(connection.client->wait_closed());
    CHECK(connection.client->close_code() == fv::remote::CLOSE_UNAUTHORIZED);
    CHECK(connection.client->close_reason() == "device removed");
    CHECK(connect(rig.port, rig.token).ready.value("code", "") == "unauthorized");
}

TEST_CASE("test_turning_sharing_off_cuts_off_connected_clients") {
    HostRig rig;
    auto connection = rig.connected();
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == 1; }));
    rig.host->stop();
    CHECK(connection.client->wait_closed());
    CHECK(connection.client->close_code() == 1001);
    CHECK(rig.host->connected_clients().empty());
    CHECK_FALSE(rig.host->running());
}

// A model switch can hold its connection for minutes: turning sharing off neither waits for it
// nor leaves a request queued behind it.
TEST_CASE("turning sharing off does not wait for a model switch in progress") {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    HostRig rig([&](SpeechHostDeps& deps) {
        deps.limits.max_active_jobs = 1;
        deps.select_model = [&](std::string, std::string, std::string,
                                std::optional<std::string>) -> Json {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait_for(lock, 20s, [&] { return released; });
            return Json{{"family", "parakeet"}, {"model", "parakeet-v2"}};
        };
    });
    auto switching = rig.connected();
    auto waiting = connect(rig.port, rig.token);
    REQUIRE(waiting.ready.value("type", "") == "ready");
    switching.client->send_binary(fv::remote::pack_request(
        Json{{"id", 1}, {"op", "select_model"}, {"family", "parakeet"}, {"model", "parakeet-v2"}}));
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 5s, [&] { return entered; }));
    }
    waiting.client->send_binary(fv::remote::pack_request(Json{{"id", 1}, {"op", "describe"}}));
    CHECK(waiting.client->receive().has_value()); // describe takes no work slot
    waiting.client->send_binary(
        fv::remote::pack_request(Json{{"id", 2}, {"op", "transcribe"}}, tone(160)));
    REQUIRE(wait_for([&] { return rig.host->queued_jobs() == 1; }));

    // At most stop_grace for a waiting pairing request plus the server's grace (2 s each), far
    // short of the switch's 20 s.
    // The test clients read only inside their calls, so they wait for the close here while
    // another thread stops the host.
    auto stopping = std::async(std::launch::async, [&] {
        const auto started = std::chrono::steady_clock::now();
        rig.host->stop();
        return std::chrono::steady_clock::now() - started;
    });
    CHECK(waiting.client->wait_closed());
    CHECK(waiting.client->close_code() == 1001);
    CHECK(switching.client->wait_closed());
    CHECK(switching.client->close_code() == 1001);
    CHECK(stopping.get() < 8s);
    CHECK_FALSE(rig.host->running());
    CHECK(rig.host->queued_jobs() == 0);
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    changed.notify_all();
}

TEST_CASE("bad frames are answered and the connection goes on") {
    HostRig rig;
    auto connection = rig.connected();
    connection.client->send_text("{}");
    CHECK(connection.client->receive() == Json{{"id", nullptr},
                                               {"error", "Requests must be binary frames"},
                                               {"code", "bad_request"}});
    const std::vector<std::byte> truncated{std::byte{0}, std::byte{1}};
    connection.client->send_binary(truncated);
    CHECK(connection.client->receive().value().at("error") == "Request frame is truncated");
    // An op Python cannot look up drops the connection, as the handler's error does there.
    connection.client->send_binary(
        fv::remote::pack_request(Json{{"id", 1}, {"op", Json::array()}}));
    CHECK(connection.client->wait_closed());
    CHECK(connection.client->close_code() == 1000);
    CHECK(wait_for([&] { return rig.host->connected_clients().empty(); }));
}

// ---- recordings and the update hold ----------------------------------------------------------

TEST_CASE("test_recording_reservation_blocks_updates_without_any_preview") {
    HostRig rig;
    const std::string recording = "0123456789abcdef0123456789abcdef";
    auto connection = rig.connected(true);
    CHECK(connection.client->request("recording_begin", Json{{"recording_id", recording}})
              .at("result") == Json::object());
    CHECK(rig.host->update_status().at("recordings") == 1);
    CHECK(rig.engine->calls().empty());
    CHECK(rig.host->prepare_update().contains("reason"));

    connection.client->close();
    CHECK(wait_for([&] { return rig.host->update_status().at("reconnecting_recording") == true; }));
    CHECK(rig.host->prepare_update().contains("reason"));
    auto again = connect(rig.port, rig.token, true);
    CHECK(again.client->request("recording_begin", Json{{"recording_id", recording}})
              .contains("result"));
    CHECK(rig.host->update_status().at("recordings") == 1);
    CHECK(rig.host->update_status().at("reconnecting_recording") == false);
    again.client->request("recording_end", Json{{"recording_id", recording}});
    CHECK(wait_for([&] {
        const Json status = rig.host->update_status();
        return status.at("recordings") == 0 && status.at("requests") == 0;
    }));

    const Json update = rig.host->prepare_update();
    REQUIRE(update.contains("token"));
    CHECK(update.at("token").get<std::string>().size() == 32);
    CHECK(update.at("expires_in") == 60.0);
    CHECK(again.client->request("recording_begin", Json{{"recording_id", recording}}).at("code") ==
          std::string(fv::remote::ENGINE_RELOADING));
    const Json refused = again.client->request("transcribe", Json::object(), tone(160));
    CHECK(refused.at("code") == std::string(fv::remote::ENGINE_RELOADING));
    CHECK(refused.at("error") == fv::remote_host::host_update_message());
    CHECK(rig.engine->calls().empty());
    CHECK(rig.host->prepare_update().at("reason") == "Another update already reserved the host");

    CHECK_FALSE(rig.host->cancel_update("another token"));
    CHECK(rig.host->cancel_update(update.at("token").get<std::string>()));
    CHECK(again.client->request("recording_begin", Json{{"recording_id", recording}})
              .contains("result"));
}

TEST_CASE("a reservation left behind lapses after its grace") {
    fv::FakeClock clock;
    HostRig rig({}, clock);
    auto connection = rig.connected(true);
    connection.client->request("recording_begin",
                               Json{{"recording_id", "0123456789abcdef0123456789abcdef"}});
    connection.client->close();
    REQUIRE(
        wait_for([&] { return rig.host->update_status().at("reconnecting_recording") == true; }));
    clock.advance(15s);
    CHECK(rig.host->update_status().at("reconnecting_recording") == false);
    CHECK(rig.host->prepare_update().contains("token"));
    clock.advance(60s);
    CHECK_FALSE(rig.host->update_active());
}

TEST_CASE("test_updates_defer_when_connected_client_cannot_report_recordings") {
    HostRig rig;
    auto connection = rig.connected();
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == 1; }));
    CHECK(rig.host->update_status().at("untracked_clients") == 1);
    CHECK_THAT(rig.host->prepare_update().at("reason").get<std::string>(),
               ContainsSubstring("cannot report"));
}

TEST_CASE("test_updates_defer_during_remote_decode_without_a_recording") {
    auto engine = std::make_shared<GateEngine>();
    HostRig rig;
    rig.serve(engine);
    auto connection = rig.connected(true);
    engine->arm();
    auto reply = std::async(std::launch::async, [&] {
        return connection.client->request("transcribe", Json::object(), tone(160));
    });
    REQUIRE(engine->wait_entered());
    CHECK(rig.host->update_status().at("recordings") == 0);
    CHECK(rig.host->update_status().at("requests") == 1);
    CHECK(rig.host->prepare_update().contains("reason"));
    engine->release();
    CHECK(reply.get().at("result").at("text") == "ok");
}

TEST_CASE("test_host_reports_a_reload_and_refuses_audio_before_decoding") {
    std::atomic<bool> settled{true};
    HostRig rig([&](SpeechHostDeps& deps) {
        deps.engine_settled = [&] {
            return settled.load();
        };
    });
    auto connection = rig.connected();
    CHECK(connection.ready.at("engine").at("available") == true);

    settled = false;
    const Json refused = connection.client->request("transcribe", Json::object(), tone(160));
    CHECK(refused == Json{{"id", 1},
                          {"code", "engine_reloading"},
                          {"error", "The host is loading its speech engine."}});
    CHECK(rig.engine->calls().empty());
    auto reloading = connect(rig.port, rig.token);
    CHECK(reloading.ready.at("engine").at("available") == false);
    CHECK_THAT(reloading.ready.at("engine").at("status").get<std::string>(),
               ContainsSubstring("loading its speech engine"));
    CHECK_THAT(rig.host->prepare_update().at("reason").get<std::string>(),
               ContainsSubstring("starting or reloading"));

    settled = true;
    CHECK(connection.client->request("transcribe", Json::object(), tone(160))
              .at("result")
              .at("text") == "heard 160");
    CHECK(rig.engine->calls().size() == 1);
}

// ---- who the host decodes for -----------------------------------------------------------------

TEST_CASE("test_the_host_says_who_it_is_decoding_for") {
    auto engine = std::make_shared<GateEngine>();
    HostRig rig;
    rig.serve(engine);
    EventLog events(*rig.host);
    auto connection = rig.connected();
    std::vector<std::vector<fv::remote::ConnectedClient>> seen;
    engine->set_watch([&] { seen.push_back(rig.host->connected_clients()); });
    connection.client->request("transcribe", Json::object(), tone(160));
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].size() == 1);
    CHECK(seen[0][0].busy);
    CHECK(seen[0][0].name == "laptop");
    CHECK_FALSE(rig.host->connected_clients()[0].busy);
    CHECK(events.of("activity") == std::vector<Json>{Json{{"name", "laptop"}, {"busy", true}},
                                                     Json{{"name", "laptop"}, {"busy", false}}});
}

TEST_CASE("test_the_host_logs_what_a_paired_computer_asked_for") {
    HostRig rig;
    auto connection = rig.connected();
    // The ready frame can arrive before the host registers the connection.
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == 1; }));
    const double now =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK_THAT(rig.host->connected_clients()[0].since, WithinAbs(now, 5));
    connection.client->request("transcribe", Json::object(), tone(2 * fv::remote::SAMPLE_RATE));
    connection.client->request("stream", Json{{"session", "preview"}, {"finish", false}},
                               tone(1600));
    rig.engine->set_failure("fell over");
    CHECK_THAT(error_of(connection.client->request("transcribe", Json::object(), tone(1600))),
               ContainsSubstring("fell over"));
    connection.client->close();
    REQUIRE(wait_for([&] {
        return rig.host->activity().snapshot().at("events")[0].at("kind") == "disconnected";
    }));
    const Json snapshot = rig.host->activity().snapshot();
    CHECK(snapshot.at("transcriptions") == 1);
    CHECK_THAT(snapshot.at("audio_s").get<double>(), WithinAbs(2.0, 1e-9));
    CHECK(snapshot.at("previews") == 1);
    CHECK(snapshot.at("errors") == 1);
    std::vector<std::string> kinds;
    for (const auto& event : snapshot.at("events")) {
        kinds.insert(kinds.begin(), event.at("kind").get<std::string>());
    }
    CHECK(kinds ==
          std::vector<std::string>{"paired", "connected", "transcribed", "failed", "disconnected"});
    REQUIRE(snapshot.at("devices").size() == 1);
    const Json device = snapshot.at("devices").begin().value();
    CHECK(device.at("name") == "laptop");
    CHECK(device.at("transcriptions") == 1);
}

TEST_CASE("test_a_transcription_is_counted_before_the_event_that_ends_it") {
    HostRig rig;
    std::vector<Json> seen;
    InlineExecutor inline_executor;
    auto subscription =
        rig.host->events.connect(inline_executor, [&](std::string kind, Json detail) {
            if (kind == "activity" && detail.at("busy") == false) {
                seen.push_back(rig.host->activity().snapshot().at("transcriptions"));
            }
        });
    auto connection = rig.connected();
    connection.client->request("transcribe", Json::object(), tone(1600));
    CHECK(seen == std::vector<Json>{Json(1)});
}

// ---- engine changes -----------------------------------------------------------------------------

TEST_CASE("test_an_idle_client_is_told_the_engine_changed") {
    HostRig rig;
    auto connection = rig.connected();
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == 1; }));
    CHECK(rig.host->engine_changed() == 0); // same engine: nothing to say
    rig.engine->set_model("turbo");
    CHECK(rig.host->engine_changed() == 1);
    CHECK(connection.client->wait_closed());
    CHECK(connection.client->close_code() == fv::remote::CLOSE_ENGINE_CHANGED);
}

TEST_CASE("test_engine_change_during_handshake_never_leaves_a_stale_client") {
    std::mutex mutex;
    std::vector<int> changed;
    std::atomic<bool> armed{false};
    std::unique_ptr<HostRig> rig_holder;
    const auto change = [&] {
        if (armed.exchange(false)) {
            rig_holder->engine->set_model("turbo");
            const int closed = rig_holder->host->engine_changed();
            std::lock_guard lock(mutex);
            changed.push_back(closed);
        }
    };
    SECTION("before_registration") {
        rig_holder = std::make_unique<HostRig>([&](SpeechHostDeps& deps) {
            deps.models = [&] {
                change();
                return Json::array();
            };
        });
        const auto paired = rig_holder->pair();
        armed = true;
        RawClient client(rig_holder->port);
        client.send(hello_message(paired.token));
        // The host closed without a ready.
        CHECK_FALSE(client.receive().has_value());
        CHECK(client.close_code() == fv::remote::CLOSE_ENGINE_CHANGED);
        std::lock_guard lock(mutex);
        CHECK(changed == std::vector<int>{0});
    }
    SECTION("before_ready") {
        rig_holder = std::make_unique<HostRig>();
        InlineExecutor inline_executor;
        auto subscription =
            rig_holder->host->events.connect(inline_executor, [&](std::string kind, Json) {
                if (kind == "clients") {
                    change();
                }
            });
        const auto paired = rig_holder->pair();
        armed = true;
        RawClient client(rig_holder->port);
        client.send(hello_message(paired.token));
        CHECK_FALSE(client.receive().has_value());
        CHECK(client.close_code() == fv::remote::CLOSE_ENGINE_CHANGED);
        CHECK(wait_for([&] {
            std::lock_guard lock(mutex);
            return changed == std::vector<int>{1};
        }));
    }
    CHECK(wait_for([&] { return rig_holder->host->connected_clients().empty(); }));
    rig_holder.reset();
}

TEST_CASE("test_a_reconnect_hands_the_client_the_new_engine") {
    HostRig rig;
    auto connection = rig.connected();
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == 1; }));
    rig.engine->set_model("turbo");
    rig.host->engine_changed();
    CHECK(connection.client->wait_closed());
    auto again = connect(rig.port, rig.token);
    CHECK(again.ready.at("engine").at("model") == "turbo");
}

TEST_CASE("test_a_client_in_the_middle_of_a_request_keeps_its_connection") {
    auto engine = std::make_shared<GateEngine>();
    HostRig rig;
    rig.serve(engine);
    auto connection = rig.connected();
    engine->arm();
    auto reply = std::async(std::launch::async, [&] {
        return connection.client->request("transcribe", Json::object(), tone(1600));
    });
    REQUIRE(engine->wait_entered());
    engine->set_model("turbo");
    CHECK(rig.host->engine_changed() == 0);
    engine->release();
    CHECK(reply.get().at("result").at("text") == "ok");
}

// ---- choosing the host's model ----------------------------------------------------------------

namespace {

struct Switchable {
    Switchable()
        : rig([this](SpeechHostDeps& deps) {
              deps.models = [] {
                  return kHostModels;
              };
              deps.select_model = [this](std::string family, std::string model,
                                         std::string device_name, std::optional<std::string>) {
                  {
                      std::lock_guard lock(mutex);
                      chosen.push_back(family + "|" + model + "|" + device_name);
                  }
                  if (model == "busy") {
                      throw std::runtime_error(
                          "devbox is transcribing right now. Try again in a moment.");
                  }
                  rig.engine->set_family(family);
                  rig.engine->set_model(model);
                  return rig.engine->describe();
              };
          }) {}

    std::mutex mutex;
    std::vector<std::string> chosen;
    HostRig rig;
};

} // namespace

TEST_CASE("test_ready_lists_the_models_the_host_can_switch_to") {
    Switchable fixture;
    CHECK(fixture.rig.connected().ready.at("models") == kHostModels);
}

TEST_CASE("test_select_model_switches_the_host_and_answers_with_the_new_engine") {
    Switchable fixture;
    EventLog events(*fixture.rig.host);
    auto connection = fixture.rig.connected();
    const Json reply = connection.client->request(
        "select_model", Json{{"family", "nemotron"}, {"model", "nemotron-3.5"}});
    CHECK(reply.at("result").at("engine").at("family") == "nemotron");
    CHECK(reply.at("result").at("engine").at("model") == "nemotron-3.5");
    CHECK(reply.at("result").at("models") == kHostModels);
    CHECK(reply.contains("host_ms"));
    {
        std::lock_guard lock(fixture.mutex);
        CHECK(fixture.chosen == std::vector<std::string>{"nemotron|nemotron-3.5|laptop"});
    }
    CHECK(events.of("engine") ==
          std::vector<Json>{
              Json{{"family", "nemotron"}, {"model", "nemotron-3.5"}, {"by", "laptop"}}});
    CHECK(fixture.rig.host->activity().snapshot(1).at("events")[0].at("kind") == "switched");
    // This connection was opened on the engine the host ran before.
    CHECK(connection.client->request("transcribe", Json::object(), tone(160)).at("code") ==
          "engine_changed");
    CHECK(connection.client->wait_closed());
}

TEST_CASE("test_select_model_follows_a_host_side_switch_and_passes_refusals_on") {
    Switchable fixture;
    auto connection = fixture.rig.connected();
    fixture.rig.engine->set_model("switched-on-the-host");
    const Json busy =
        connection.client->request("select_model", Json{{"family", "parakeet"}, {"model", "busy"}});
    CHECK_THAT(error_of(busy), ContainsSubstring("transcribing right now"));
    CHECK_FALSE(busy.contains("code"));
    CHECK_THAT(error_of(connection.client->request("select_model", Json{{"family", "parakeet"}})),
               ContainsSubstring("Choose a model"));
    CHECK(connection.client->close_code() == std::nullopt);
}

TEST_CASE("test_a_host_that_cannot_switch_lists_nothing_and_refuses") {
    HostRig rig;
    auto connection = rig.connected();
    CHECK(connection.ready.at("models") == Json::array());
    CHECK_THAT(error_of(connection.client->request(
                   "select_model", Json{{"family", "parakeet"}, {"model", "parakeet-v3"}})),
               ContainsSubstring("doesn't let paired computers"));
}

// ---- records ------------------------------------------------------------------------------------

namespace {

struct RecordsHost {
    RecordsHost()
        : rig([this](SpeechHostDeps& deps) {
              deps.records = [this](std::string_view op, const Json&, std::span<const std::byte>,
                                    const fv::remote::PairedDevice& device) {
                  std::lock_guard lock(mutex);
                  ops.push_back(std::string(op) + "|" + device.name);
                  return Json{{"op", op}};
              };
              deps.records_enabled = [this] {
                  return keeps.load();
              };
              deps.records_summary = [](std::string_view) {
                  return Json{{"dictation", {{"count", 0}, {"bytes", 0}}},
                              {"meeting", {{"count", 0}, {"bytes", 0}}}};
              };
          }) {}

    std::atomic<bool> keeps{true};
    std::mutex mutex;
    std::vector<std::string> ops;
    HostRig rig;
};

} // namespace

TEST_CASE("test_ready_advertises_records_and_counts") {
    RecordsHost fixture;
    const auto connection = fixture.rig.connected();
    CHECK(connection.ready.at("capabilities").at("records") == true);
    CHECK(connection.ready.at("records").at("dictation") == Json{{"count", 0}, {"bytes", 0}});
}

TEST_CASE("test_turning_storage_off_still_lets_a_device_read_and_delete_its_records") {
    RecordsHost fixture;
    EventLog events(*fixture.rig.host);
    auto connection = fixture.rig.connected();
    fixture.keeps = false;
    CHECK(connection.client->request("records_list", Json{{"kind", "dictation"}}).at("result") ==
          Json{{"op", "records_list"}});
    CHECK(connection.client->request("records_delete", Json{{"kind", "dictation"}})
              .contains("result"));
    const Json refused = connection.client->request("records_begin", Json{{"kind", "dictation"}});
    CHECK(refused.at("code") == "forbidden");
    CHECK_THAT(error_of(refused), ContainsSubstring("isn't keeping records for other computers"));
    {
        std::lock_guard lock(fixture.mutex);
        CHECK(fixture.ops ==
              std::vector<std::string>{"records_list|laptop", "records_delete|laptop"});
    }
    CHECK(events.of("records") ==
          std::vector<Json>{Json{{"device", "laptop"}, {"op", "records_delete"}}});
}

TEST_CASE("records errors become the replies Python gives them") {
    HostRig rig([](SpeechHostDeps& deps) {
        deps.records_enabled = [] {
            return true;
        };
        deps.records = [](std::string_view op, const Json&, std::span<const std::byte>,
                          const fv::remote::PairedDevice&) -> Json {
            if (op == "records_open") {
                throw fv::remote_host::RecordNotFound("");
            }
            if (op == "records_put") {
                throw fv::remote_host::RecordsBusy("Another upload is still being written.");
            }
            if (op == "records_delete") {
                throw fv::py::ValueError("Invalid record ID.");
            }
            throw std::runtime_error("");
        };
    });
    auto connection = rig.connected();
    CHECK(connection.client->request("records_open") ==
          Json{{"id", 1}, {"code", "not_found"}, {"error", "No such record."}});
    CHECK(connection.client->request("records_put") ==
          Json{{"id", 2},
               {"code", "busy"},
               {"retry_after_ms", 1000},
               {"error", "Another upload is still being written."}});
    CHECK(connection.client->request("records_delete") ==
          Json{{"id", 3}, {"code", "bad_request"}, {"error", "Invalid record ID."}});
    CHECK(connection.client->request("records_list") == Json{{"id", 4}, {"error", "RuntimeError"}});
}

// ---- MCP control
// -----------------------------------------------------------------------------------

namespace {

// HostMcpControl's validation as the host passes it on: ValueError("code: text").
struct McpHost {
    McpHost()
        : rig([this](SpeechHostDeps& deps) {
              deps.mcp_enabled = [this] {
                  return allowed.load();
              };
              deps.mcp = [this](std::string_view op, const Json& header, std::string_view) -> Json {
                  std::lock_guard lock(mutex);
                  if (op == "mcp_configure") {
                      const Json& settings = header.at("settings");
                      if (settings.contains("port")) {
                          const Json& port = settings.at("port");
                          if (!port.is_number_integer() || port.get<int>() < 1 ||
                              port.get<int>() > 65535) {
                              throw fv::py::ValueError(
                                  "invalid_value: The port must be between 1 and 65535.");
                          }
                          if (enabled) {
                              throw fv::py::ValueError(
                                  "busy: Turn MCP off before changing its port.");
                          }
                      }
                      if (settings.value("enabled", false) && !enabled) {
                          enabled = true;
                          ++starts;
                      }
                  }
                  return Json{{"state", enabled ? "running" : "stopped"}};
              };
          }) {}

    int start_count() {
        std::lock_guard lock(mutex);
        return starts;
    }

    std::atomic<bool> allowed{false};
    std::mutex mutex;
    bool enabled = false;
    int starts = 0;
    HostRig rig;
};

} // namespace

TEST_CASE("test_pairing_alone_never_grants_mcp_control") {
    McpHost fixture;
    auto connection = fixture.rig.connected();
    CHECK(connection.ready.at("capabilities").at("mcp_control") == false);
    for (const auto& [op, fields] : std::vector<std::pair<std::string, Json>>{
             {"mcp_state", Json::object()},
             {"mcp_configure", Json{{"settings", {{"enabled", true}}}}}}) {
        const Json refused = connection.client->request(op, fields);
        CHECK(refused.at("code") == "forbidden");
        CHECK_THAT(error_of(refused), ContainsSubstring("isn't letting paired"));
    }
    CHECK(fixture.start_count() == 0);
    CHECK(connection.client->request("describe").at("result").at("available") == true);
}

TEST_CASE("test_permission_is_rechecked_on_existing_connections (MCP)") {
    McpHost fixture;
    auto connection = fixture.rig.connected();
    fixture.allowed = true;
    CHECK(connection.client->request("mcp_state").at("result").at("state") == "stopped");
    fixture.allowed = false;
    CHECK_THAT(error_of(connection.client->request("mcp_configure",
                                                   Json{{"settings", {{"enabled", true}}}})),
               ContainsSubstring("isn't letting paired"));
    CHECK(fixture.start_count() == 0);
}

TEST_CASE("test_malformed_configure_requests_are_refused") {
    McpHost fixture;
    fixture.allowed = true;
    auto connection = fixture.rig.connected();
    for (const Json& fields : {Json::object(), Json{{"settings", Json::array()}},
                               Json{{"settings", {{"enabled", true}}}, {"extra", 1}}}) {
        const Json refused = connection.client->request("mcp_configure", fields);
        CHECK(refused.at("code") == "bad_request");
        CHECK(error_of(refused) == "Invalid MCP request.");
    }
    CHECK(connection.client->request("mcp_state", Json{{"extra", 1}}).at("code") == "bad_request");
    CHECK(fixture.start_count() == 0);
}

TEST_CASE("test_host_validation_errors_reach_the_client_without_internals") {
    McpHost fixture;
    fixture.allowed = true;
    EventLog events(*fixture.rig.host);
    auto connection = fixture.rig.connected();
    const Json invalid =
        connection.client->request("mcp_configure", Json{{"settings", {{"port", 0}}}});
    CHECK(invalid == Json{{"id", 1},
                          {"code", "invalid_value"},
                          {"error", "The port must be between 1 and 65535."}});
    CHECK(connection.client->request("mcp_configure", Json{{"settings", {{"enabled", true}}}})
              .contains("result"));
    const Json busy =
        connection.client->request("mcp_configure", Json{{"settings", {{"port", 9000}}}});
    CHECK(busy.at("code") == "busy");
    CHECK_THAT(error_of(busy), ContainsSubstring("Turn MCP off"));
    CHECK(events.of("mcp") == std::vector<Json>{Json{{"by", "laptop"}}});
}

TEST_CASE("test_revoked_devices_lose_mcp_control") {
    McpHost fixture;
    fixture.allowed = true;
    auto connection = fixture.rig.connected();
    fixture.rig.registry.remove(fixture.rig.device_id);
    Json header = Json{{"id", 1}, {"op", "mcp_configure"}, {"settings", {{"enabled", true}}}};
    connection.client->send_binary(fv::remote::pack_request(header));
    CHECK_FALSE(connection.client->receive().has_value());
    CHECK(connection.client->close_code() == fv::remote::CLOSE_UNAUTHORIZED);
    CHECK(connection.client->close_reason() == "device removed");
    CHECK(fixture.start_count() == 0);
}

// ---- model management and runtimes
// ---------------------------------------------------------------

namespace {

struct ManagedHost {
    ManagedHost()
        : rig([this](SpeechHostDeps& deps) {
              deps.model_management = [this] {
                  return allowed.load();
              };
              deps.manage_models = [this](std::string_view op, const Json& header,
                                          std::string_view) -> Json {
                  std::lock_guard lock(mutex);
                  calls.push_back(std::string(op));
                  if (op == "model_catalog") {
                      return Json{{"models", Json::array({Json{{"model", "parakeet-v3"}}})}};
                  }
                  return Json{{"job", op}, {"family", header.at("family")}};
              };
          }) {}

    std::vector<std::string> call_log() {
        std::lock_guard lock(mutex);
        return calls;
    }

    std::atomic<bool> allowed{false};
    std::mutex mutex;
    std::vector<std::string> calls;
    HostRig rig;
};

} // namespace

TEST_CASE("test_permission_is_rechecked_on_existing_connections (model management)") {
    ManagedHost fixture;
    auto connection = fixture.rig.connected();
    CHECK(connection.ready.at("capabilities").at("model_management") == false);
    for (const char* op : {"model_catalog", "download_model", "install_runtime"}) {
        const Json refused = connection.client->request(op);
        CHECK(refused.at("code") == "forbidden");
        CHECK_THAT(error_of(refused), ContainsSubstring("disabled"));
    }
    CHECK(connection.client->request("describe").at("result").at("available") == true);
    fixture.allowed = true;
    CHECK(!connection.client->request("model_catalog").at("result").at("models").empty());
    fixture.allowed = false;
    for (const char* op : {"model_catalog", "download_model", "install_runtime"}) {
        CHECK_THAT(error_of(connection.client->request(
                       op, Json{{"family", "local_whisper"}, {"model", "tiny"}})),
                   ContainsSubstring("disabled"));
    }
}

TEST_CASE("test_management_does_not_relax_authentication_or_revocation") {
    ManagedHost fixture;
    fixture.allowed = true;
    auto connection = fixture.rig.connected();
    CHECK(connect(fixture.rig.port, "wrong-token").ready.at("code") == "unauthorized");
    // Only the registry entry goes, to exercise the per-request check without remove_device's
    // proactive close.
    fixture.rig.registry.remove(fixture.rig.device_id);
    Json header =
        Json{{"id", 1}, {"op", "download_model"}, {"family", "local_whisper"}, {"model", "tiny"}};
    connection.client->send_binary(fv::remote::pack_request(header));
    CHECK_FALSE(connection.client->receive().has_value());
    CHECK(connection.client->close_code() == fv::remote::CLOSE_UNAUTHORIZED);
    CHECK(fixture.call_log().empty());
}

TEST_CASE("test_malformed_mutations_never_reach_downloader") {
    ManagedHost fixture;
    fixture.allowed = true;
    auto connection = fixture.rig.connected();
    for (const Json& fields :
         {Json::object(), Json{{"family", Json::array()}, {"model", "tiny"}},
          Json{{"family", "local_whisper"}, {"model", nullptr}},
          Json{
              {"family", "local_whisper"}, {"model", "tiny"}, {"url", "https://example.com/model"}},
          Json{{"family", "local_whisper"},
               {"model", "tiny"},
               {"settings", {{"remote_host_model_management", true}}}}}) {
        const Json refused = connection.client->request("download_model", fields);
        CHECK(refused.at("code") == "bad_request");
        CHECK(error_of(refused) == "Invalid model management request.");
    }
    CHECK(connection.client
              ->request("install_runtime",
                        Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "tpu"}})
              .at("code") == "bad_request");
    CHECK(connection.client->request("model_catalog", Json{{"family", "parakeet"}}).at("code") ==
          "bad_request");
    CHECK(fixture.call_log().empty());
}

TEST_CASE("test_install_rechecks_permission_and_revocation_on_existing_connection") {
    ManagedHost fixture;
    fixture.allowed = true;
    auto connection = fixture.rig.connected();
    CHECK(connection.ready.at("capabilities").at("runtime_installation") == true);
    const Json install{{"family", "nemotron"}, {"model", "nemotron-3.5"}, {"device", "cuda"}};
    fixture.allowed = false;
    CHECK_THAT(error_of(connection.client->request("install_runtime", install)),
               ContainsSubstring("disabled"));
    fixture.allowed = true;
    fixture.rig.registry.remove(fixture.rig.device_id);
    Json header = install;
    header["id"] = 2;
    header["op"] = "install_runtime";
    connection.client->send_binary(fv::remote::pack_request(header));
    CHECK_FALSE(connection.client->receive().has_value());
    CHECK(fixture.call_log().empty());
}

TEST_CASE("test_protocol_rejects_extra_fields_and_removed_clients") {
    std::mutex mutex;
    std::string device = "cpu";
    HostRig rig([&](SpeechHostDeps& deps) {
        deps.configure_runtime = [&](std::string, std::string, Json settings, std::string) -> Json {
            const std::string wanted = settings.value("device", "");
            if (wanted != "cpu" && wanted != "cuda") {
                throw std::runtime_error("The " + wanted + " runtime isn't ready on devbox.");
            }
            std::lock_guard lock(mutex);
            device = wanted;
            return Json{{"device", wanted}};
        };
    });
    auto connection = rig.connected();
    CHECK(connection.ready.at("capabilities").at("engine_controls") == true);
    const Json extra =
        connection.client->request("configure_runtime", Json{{"family", "parakeet"},
                                                             {"model", "parakeet-v3"},
                                                             {"settings", {{"device", "cuda"}}},
                                                             {"arbitrary", "value"}});
    CHECK(extra == Json{{"id", 1}, {"code", "bad_request"}, {"error", "Invalid runtime request."}});
    CHECK_THAT(error_of(connection.client->request("configure_runtime",
                                                   Json{{"family", "parakeet"},
                                                        {"model", "parakeet-v3"},
                                                        {"settings", {{"device", "invalid"}}}})),
               ContainsSubstring("isn't ready"));
    {
        std::lock_guard lock(mutex);
        CHECK(device == "cpu");
    }
    rig.host->remove_device(rig.device_id);
    CHECK(connection.client->wait_closed());
    {
        std::lock_guard lock(mutex);
        CHECK(device == "cpu");
    }
}

TEST_CASE("a runtime change answers with the new engine and the runtime") {
    HostRig rig([](SpeechHostDeps& deps) {
        deps.runtime = [] {
            return Json{{"family", "parakeet"}};
        };
        deps.configure_runtime = [](std::string, std::string, Json, std::string) {
            return Json{{"model", "parakeet-v2"}};
        };
    });
    EventLog events(*rig.host);
    auto connection = rig.connected();
    const Json reply = connection.client->request(
        "configure_runtime",
        Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"settings", {{"device", "cpu"}}}});
    CHECK(reply.at("result") ==
          Json{{"engine", {{"model", "parakeet-v2"}}}, {"runtime", {{"family", "parakeet"}}}});
    CHECK(events.of("engine") == std::vector<Json>{Json{{"by", "laptop"}}});
}

// ---- names and hints
// -------------------------------------------------------------------------------

TEST_CASE("test_a_connected_computer_goes_by_its_new_name_at_once") {
    HostRig rig;
    EventLog events(*rig.host);
    auto connection = rig.connected();
    connection.client->request("transcribe", Json::object(), tone(1600));
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == 1; }));
    const auto renamed = rig.host->rename_device(rig.device_id, "Kitchen laptop");
    REQUIRE(renamed.has_value());
    CHECK(renamed->name == "Kitchen laptop");
    CHECK(rig.host->connected_clients()[0].name == "Kitchen laptop");
    CHECK(rig.host->activity().snapshot().at("devices").at(rig.device_id).at("name") ==
          "Kitchen laptop");
    CHECK(events.kinds().contains("devices"));
    CHECK(events.kinds().contains("clients"));
    // The connection opened before the rename makes its next request under the new name.
    connection.client->request("transcribe", Json::object(), tone(1600));
    const Json newest = rig.host->activity().snapshot(1).at("events")[0];
    CHECK(newest.at("kind") == "transcribed");
    CHECK(newest.at("name") == "Kitchen laptop");
    connection.client->close();
    REQUIRE(wait_for([&] {
        return rig.host->activity().snapshot(1).at("events")[0].at("kind") == "disconnected";
    }));
    CHECK(rig.host->activity().snapshot(1).at("events")[0].at("name") == "Kitchen laptop");
}

TEST_CASE("test_round_trip_reaches_an_engine_that_uses_hints") {
    HostRig rig;
    rig.engine->set_accepts_phrases(true);
    auto connection = rig.connected();
    CHECK(connection.ready.at("capabilities").at("recognition_hints") == true);
    const Json phrases = Json::array({"Ksenia", "Kubernetes", std::string(65, 'x'), 7});
    connection.client->request("transcribe", Json{{"language", "en"}, {"phrases", phrases}},
                               tone(1600));
    auto calls = rig.engine->calls();
    CHECK(calls.back().language == "en");
    CHECK(calls.back().phrases == std::vector<std::string>{"Ksenia", "Kubernetes"});
    connection.client->request("transcribe", Json{{"language", "en"}}, tone(1600));
    CHECK(rig.engine->calls().back().phrases.empty());
}

TEST_CASE("test_a_host_without_the_capability_ignores_phrases") {
    HostRig rig;
    auto connection = rig.connected();
    CHECK(connection.ready.at("capabilities").at("recognition_hints") == false);
    const Json reply = connection.client->request(
        "transcribe", Json{{"language", "en"}, {"phrases", Json::array({"Ksenia", "Kubernetes"})}},
        tone(1600));
    CHECK(reply.at("result").at("text") == "heard 1600");
    const auto calls = rig.engine->calls();
    CHECK(calls.back().op == "transcribe");
    CHECK(calls.back().language == "en");
    CHECK(calls.back().phrases.empty());
}

TEST_CASE("test_phrases_never_change_the_engine_identity") {
    HostRig rig;
    rig.engine->set_accepts_phrases(true);
    const auto before = rig.engine->identity();
    auto connection = rig.connected();
    connection.client->request(
        "transcribe", Json{{"language", "en"}, {"phrases", Json::array({"Ksenia"})}}, tone(1600));
    CHECK(rig.engine->identity() == before);
    CHECK(connection.client->request("describe").at("result").at("family") == "parakeet");
}

// ---- the reactor never waits
// -----------------------------------------------------------------------

TEST_CASE(
    "a thousand requests on one connection keep the reactor quick and the host's state flat") {
    std::mutex mutex;
    std::chrono::nanoseconds slowest{0};
    std::size_t callbacks = 0;
    HostRig rig([&](SpeechHostDeps& deps) {
        deps.reactor_timing = [&](std::chrono::nanoseconds took) {
            std::lock_guard lock(mutex);
            slowest = std::max(slowest, took);
            ++callbacks;
        };
    });
    auto connection = rig.connected();
    for (int i = 0; i < 1000; ++i) {
        const Json reply = connection.client->request(
            i % 2 == 0 ? "describe" : "transcribe", Json::object(),
            i % 2 == 0 ? std::nullopt : std::optional<std::vector<float>>(tone(1600)));
        REQUIRE(reply.contains("result"));
    }
    CHECK(rig.host->connected_clients().size() == 1);
    CHECK(rig.host->queued_jobs() == 0);
    CHECK(rig.host->update_status().at("requests") == 0);
    CHECK(rig.host->activity().snapshot().at("events").size() <=
          static_cast<std::size_t>(fv::remote_host::kMaxEvents));
    std::lock_guard lock(mutex);
    CHECK(callbacks >= 1000);
    CHECK(slowest < 5ms);
}
