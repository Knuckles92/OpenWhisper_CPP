// Audit of docs/port/map/a17-remote-engine-services.md: behaviour of the host and the service that
// no other test pinned, checked against services/remote_asr/host.py, runtime.py and service.py.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "host_rig.h"
#include "pyport/errors.h"
#include "remote/client.h"
#include "remote/connection.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/runtime_state.h"
#include "remote_host/service.h"
#include "remote_host/speech_host.h"
#include "remote_host/testing/fake_host_engine.h"
#include "settings/keys.h"
#include "speech/service.h"

using namespace fv;
using namespace fv::remote_host;
using namespace std::chrono_literals;
using Catch::Matchers::Message;

namespace {

MapEnvReader private_env(const std::filesystem::path& root) {
    return MapEnvReader{
        {std::string(identity().env_prefix) + "DATA_DIR", path_to_utf8(root)},
        {"LOCALAPPDATA", path_to_utf8(root / "local")},
        {"XDG_DATA_HOME", path_to_utf8(root / "local")},
        {"HOME", path_to_utf8(root / "home")},
        {"USERPROFILE", path_to_utf8(root / "home")},
    };
}

// A whole RemoteEngineService per data root, as test_service.cpp's ServiceRig.
struct Service {
    MapEnvReader env;
    Paths paths;
    SettingsStore settings{paths.user_file(identity().settings_file)};
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    std::shared_ptr<RemoteEngineService> service;

    Service(const std::filesystem::path& root, std::string name, net::Reactor& reactor,
            Clock& clock, Executor& io, std::shared_ptr<HostEngine> engine,
            std::optional<int> listen = 0)
        : env(private_env(root)), paths(Paths::resolve(identity(), env)),
          service(std::make_shared<RemoteEngineService>(RemoteEngineServiceDeps{
              .paths = paths,
              .identity = identity(),
              .settings = settings,
              .credentials = credentials,
              .env = env,
              .clock = clock,
              .io = io,
              .client_env = {reactor},
              .computer_name = std::move(name),
              .tailscale = std::make_shared<remote::testing::FakeTailscaleCli>(),
              .engine = [engine = std::move(engine)] { return engine; },
              .bind = "127.0.0.1",
              .discovery_port = std::nullopt,
              .listen_port = listen})) {}
};

// A port nothing listens on now: one a host just gave back.
int free_port() {
    rig::HostRig taken;
    return taken.port;
}

std::string engine_error(const std::function<void()>& call) {
    try {
        call();
    } catch (const remote::RemoteEngineError& error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("a17 audit: the host's limits are OpenWhisper's", "[audit]") {
    // host.py's _MAX_HANDSHAKES, _MAX_CONNECTIONS, _MAX_ACTIVE_JOBS, _MAX_QUEUED_JOBS,
    // _JOB_WAIT_S, HANDSHAKE_TIMEOUT_S, PING_INTERVAL_S / PING_TIMEOUT_S (A17-16), the approval
    // window and limits (A17-11, A17-12), the pairing code's life (A17-09), the update hold
    // (A17-26) and a reservation's reconnect grace (A17-25).
    const HostLimits limits;
    CHECK(limits.max_handshakes == 12);
    CHECK(limits.max_connections == 32);
    CHECK(limits.max_active_jobs == 2);
    CHECK(limits.max_queued_jobs == 4);
    CHECK(limits.job_wait == 30s);
    CHECK(limits.handshake == 10s);
    CHECK(limits.ping == 20s);
    CHECK(limits.approval == 120s);
    CHECK(limits.approval_window == 600s);
    CHECK(limits.max_approval_requests == 4);
    CHECK(limits.stop_grace == 2s);
    CHECK(limits.pairing_ttl_s == 300.0);
    CHECK(limits.update_hold == 60s);
    CHECK(limits.recording_grace == 15s);
}

TEST_CASE("a17 audit: a runtime change is refused in runtime.py's words", "[audit]") {
    // A17-28: validate_runtime's sentences, which a paired computer shows as they are.
    const Json engine{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cpu"}};
    const models::Host host{
        .platform = std::string(models::platform::kWinAmd64),
        .is_installed =
            [](std::string_view id) { return id == models::component_id::kAsrNvidiaCpu; },
    };
    const Json settings{{"local_asr_devices", {{"parakeet", "auto"}}}};
    const auto refusal = [&](std::string_view family, std::string_view model, const Json& changes) {
        try {
            (void)validate_runtime(engine, settings, host, family, model, changes);
        } catch (const py::ValueError& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    CHECK(refusal("parakeet", "orukeet-v0.1", Json{{"device", "cpu"}}) ==
          "The host changed engines. Reconnect before changing its runtime.");
    CHECK(refusal("parakeet", "parakeet-v3", Json::object()) ==
          "Choose a device or precision supported by the host.");
    CHECK(refusal("parakeet", "parakeet-v3", Json{{"threads", 4}}) ==
          "Choose a device or precision supported by the host.");
    CHECK(refusal("parakeet", "parakeet-v3", Json{{"device", "cuda"}}) ==
          "That device isn't ready on the host. Check its hardware and installed runtimes.");
    CHECK(refusal("parakeet", "parakeet-v3", Json{{"compute_type", "float16"}}) ==
          "This engine manages its own precision on the host.");
    CHECK(refusal("parakeet", "parakeet-v3", Json{{"language", "xx"}}) ==
          "Choose a language supported by this engine on the host.");
    CHECK(refusal("parakeet", "parakeet-v3", Json{{"device", "cpu"}}).empty());
}

TEST_CASE("a17 audit: this computer's engine settling tells idle paired computers to reconnect",
          "[audit]") {
    // A17-20, service.py engine_changed: the trigger is the dictation slot leaving Loading.
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    speech::SpeechService speech({}, io);
    auto engine = std::make_shared<remote_host::testing::FakeHostEngine>();
    Service host(folder.path() / "host", "Desk", reactor, clock, io, engine);
    Service client(folder.path() / "client", "Laptop", reactor, clock, io,
                   unavailable_engine("No speech engine."));
    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    client.service->pair(remote::format_address("127.0.0.1", host.service->host_state().port),
                         *code, false);
    host.service->watch_engine(speech);
    auto connection = client.service->client_connection();
    REQUIRE(connection->connect()["type"] == "ready");

    engine->set_model("parakeet-other");
    // Still loading: nobody is told yet.
    speech.readiness_changed.emit(speech::Slot::Dictation,
                                  speech::Readiness{speech::ReadyState::Loading, "", ""});
    // Another slot settling is not this computer's dictation engine.
    speech.readiness_changed.emit(speech::Slot::Meeting,
                                  speech::Readiness{speech::ReadyState::Ready, "", ""});
    std::this_thread::sleep_for(200ms);
    CHECK(connection->alive());
    speech.readiness_changed.emit(speech::Slot::Dictation,
                                  speech::Readiness{speech::ReadyState::Ready, "", ""});
    CHECK(rig::wait_for([&] { return connection->closed_for_engine_change(); }));
    connection->close();
    host.service->shutdown();
}

TEST_CASE("a17 audit: a model request names what keeps it from the host", "[audit]") {
    // A17-42 and A17-64: service.py remote_model_request's gates, before and after connecting.
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    // No model cache or components: the host advertises model management as off.
    Service host(folder.path() / "host", "Desk", reactor, clock, io,
                 std::make_shared<remote_host::testing::FakeHostEngine>());
    Service client(folder.path() / "client", "Laptop", reactor, clock, io,
                   unavailable_engine("No speech engine."));

    CHECK_THROWS_MATCHES(client.service->remote_model_request("format_disk", Json::object()),
                         py::ValueError, Message("Unknown model management operation."));
    CHECK(engine_error([&] {
              client.service->remote_model_request("model_catalog", Json::object());
          }) == "Pair with a host before managing its models.");

    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto pairing = client.service->pair(
        remote::format_address("127.0.0.1", host.service->host_state().port), *code, false);

    CHECK(engine_error([&] {
              client.service->remote_model_request("model_catalog", Json::object(), pairing);
          }) == "Model management is disabled on the host. Enable it in Settings → Remote "
                "engine there.");
    // A window opened for another pairing is told so before anything is sent.
    auto stale = pairing;
    stale.fingerprint = std::string(64, 'A');
    CHECK(engine_error([&] {
              client.service->remote_model_request("model_catalog", Json::object(), stale);
          }) == "The paired host changed. Close this window and open Manage host models again.");
    // Moved addresses or a new name are still the same pairing.
    auto moved = pairing;
    moved.alternates = {"192.0.2.7"};
    moved.host_name = "Office";
    CHECK(engine_error([&] {
              client.service->remote_model_request("model_catalog", Json::object(), moved);
          }).starts_with("Model management is disabled"));

    // A host without MCP control says it is too old, as the "unsupported" code.
    try {
        (void)client.service->remote_mcp_request("mcp_state", Json::object(), pairing);
        FAIL("a host without MCP control answered");
    } catch (const remote::RemoteRequestError& error) {
        CHECK(error.code == std::optional<std::string>("unsupported"));
        CHECK(std::string(error.what()) == "Update " + std::string(identity().product) +
                                               " on the host to manage its MCP server from here.");
    }
    host.service->shutdown();
}

TEST_CASE("a17 audit: a new port moves sharing only while it is on", "[audit]") {
    // A17-56, service.py set_host_port: saved, and re-applied, which restarts a running host on
    // the new port; a host that isn't sharing only keeps the port for later.
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    Service host(folder.path() / "host", "Desk", reactor, clock, io,
                 std::make_shared<remote_host::testing::FakeHostEngine>(), std::nullopt);
    const int first = free_port();
    host.service->set_host_port(first);
    CHECK(host.settings.get(keys::remote_host_port) == first);
    CHECK_FALSE(host.service->host_state().running);

    host.service->set_host_enabled(true);
    REQUIRE(host.service->host_state().running);
    CHECK(host.service->host_state().port == first);
    remote::ClientEnv env{reactor};
    CHECK(remote::probe_host(env, "127.0.0.1", first).host_name == "Desk");

    const int second = free_port();
    host.service->set_host_port(second);
    REQUIRE(host.service->host_state().running);
    CHECK(host.service->host_state().port == second);
    CHECK(remote::probe_host(env, "127.0.0.1", second).host_name == "Desk");
    CHECK_THROWS_AS(remote::probe_host(env, "127.0.0.1", first), remote::RemoteEngineError);

    // OpenWhisper_CPP's own check (OpenWhisper saves any int and fails to listen on it).
    CHECK_THROWS_MATCHES(host.service->set_host_port(0), py::ValueError,
                         Message("Enter a port from 1 to 65535."));
    host.service->set_host_enabled(false);
    CHECK_FALSE(host.service->host_state().running);
    host.service->shutdown();
}
