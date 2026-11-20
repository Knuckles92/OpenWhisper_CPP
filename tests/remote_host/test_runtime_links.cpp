// The remote engine's runtime as the app sees it (services/runtime/remote.py RemoteRuntime and
// services/remote_asr/service.py): what RemoteEngineService hands the controller of the dictation
// engine it made, what it stops retrying, and what paired computers hear while this host reloads.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "host_rig.h"
#include "models/testing/fake_engine_control.h"
#include "remote/pairing_settings.h"
#include "remote/remote_backend.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/service.h"
#include "remote_host/testing/fake_host_engine.h"

using namespace fv;
using namespace fv::remote_host;
using Catch::Matchers::ContainsSubstring;

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

// One computer's RemoteEngineService, with the app's engine control when one is given.
struct Computer {
    MapEnvReader env;
    Paths paths;
    SettingsStore settings{paths.user_file(identity().settings_file)};
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    std::shared_ptr<RemoteEngineService> service;

    Computer(const std::filesystem::path& root, std::string name, net::Reactor& reactor,
             Clock& clock, Executor& io, std::shared_ptr<HostEngine> engine,
             models::EngineControl* control = nullptr)
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
              .engine_control = control,
              .bind = "127.0.0.1",
              .discovery_port = std::nullopt,
              .listen_port = 0})) {}
};

} // namespace

// remote.py RemoteSpeechBackend.restore_link: a pairing without a token is "unpaired", which
// the runtime never retries (_on_settled schedules nothing for it).
TEST_CASE("a pairing whose token is missing is not retried") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io(clock);
    Computer laptop(folder.path() / "laptop", "Laptop", reactor, clock, io,
                    std::make_shared<testing::FakeHostEngine>());
    remote::ClientPairing pairing;
    pairing.host = "127.0.0.1";
    pairing.port = 9;
    pairing.fingerprint = std::string(64, 'a');
    pairing.host_name = "devbox";
    pairing.device_id = "device-1";
    remote::write_pairing(laptop.settings, pairing);

    auto made = laptop.service->make_backend();
    auto& backend = dynamic_cast<remote::RemoteSpeechBackend&>(*made);
    const auto readiness = backend.load(CancelToken{});
    CHECK(readiness.state == speech::ReadyState::Offline);
    CHECK_THAT(readiness.device_info, ContainsSubstring("missing its token"));
    // Nothing is scheduled: no retry can succeed until this computer pairs again. (What io
    // holds are the link's change notices.)
    io.run_all();
    CHECK_FALSE(backend.retry_at());
    CHECK(backend.restore_link() == "unpaired");
    io.run_all();
    CHECK_FALSE(backend.retry_at());
    CHECK_THAT(backend.device_info(), ContainsSubstring("missing its token"));

    // The readiness check's retry_now still asks once, and that attempt schedules nothing more.
    const auto link = laptop.service->dictation_link();
    link.retry_now();
    CHECK(io.run_all() >= 1);
    CHECK_FALSE(backend.retry_at());
    CHECK_FALSE(link.restoring());
    clock.advance(std::chrono::seconds(120));
    io.run_all();
    CHECK_FALSE(backend.retry_at());
}

// The controller keeps no pointer to the engine the SpeechService owns: the service's link to it
// reaches it while it lives and does nothing after.
TEST_CASE("the dictation link reaches the engine while it lives and nothing after") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io(clock);
    Computer laptop(folder.path() / "laptop", "Laptop", reactor, clock, io,
                    std::make_shared<testing::FakeHostEngine>());
    CHECK_FALSE(laptop.service->dictation_link().restoring());
    laptop.service->dictation_link().retry_now(); // no engine yet
    CHECK(io.queued() == 0);

    auto made = laptop.service->make_backend();
    const auto link = laptop.service->dictation_link();
    link.retry_now();
    CHECK(io.queued() == 1);
    io.run_all(); // unpaired: one quiet attempt
    made.reset();
    link.retry_now();
    CHECK(io.queued() == 0);
    CHECK_FALSE(link.restoring());
}

// services/remote_asr/service.py passes the controller's _engine_settled to the host, which
// answers hello and transcribe with "The host is loading its speech engine." until the reload
// queued or running here has ended.
TEST_CASE("paired computers hear that the host is loading while its reload runs") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    models::FakeEngineControl control;
    auto engine = std::make_shared<testing::FakeHostEngine>();
    Computer desk(folder.path() / "desk", "Desk", reactor, clock, io, engine, &control);
    Computer laptop(folder.path() / "laptop", "Laptop", reactor, clock, io, engine);
    desk.service->set_host_enabled(true);
    const auto code = desk.service->open_pairing();
    REQUIRE(code);
    const auto saved = laptop.service->pair(
        remote::format_address("127.0.0.1", desk.service->host_state().port), *code, false);

    control.set_engine_settled(false);
    {
        const Json ready = laptop.service->client_connection(saved)->connect();
        CHECK(ready["engine"]["available"] == false);
        CHECK(ready["engine"]["status"] == "The host is loading its speech engine.");
    }
    control.set_engine_settled(true);
    {
        const Json ready = laptop.service->client_connection(saved)->connect();
        CHECK(ready["engine"]["status"] != "The host is loading its speech engine.");
    }
    desk.service->set_host_enabled(false);
}

// The dictation engine's hello does not offer recording reservations it never sends.
TEST_CASE("the dictation engine does not advertise recording reservations") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto engine = std::make_shared<testing::FakeHostEngine>();
    Computer desk(folder.path() / "desk", "Desk", reactor, clock, io, engine);
    Computer laptop(folder.path() / "laptop", "Laptop", reactor, clock, io, engine);
    desk.service->set_host_enabled(true);
    const auto code = desk.service->open_pairing();
    REQUIRE(code);
    (void)laptop.service->pair(remote::format_address("127.0.0.1", desk.service->host_state().port),
                               *code, false);
    auto made = laptop.service->make_backend();
    auto& backend = dynamic_cast<remote::RemoteSpeechBackend&>(*made);
    REQUIRE(backend.load(CancelToken{}).state == speech::ReadyState::Ready);
    // The host counts it as a client that cannot report recordings, so an update waits for it.
    const Json status = desk.service->update_status();
    INFO(status.dump());
    CHECK(status.value("untracked_clients", -1) == 1);
    backend.release();
    desk.service->set_host_enabled(false);
}

// meeting/asr/remote.py _check_pairing: a meeting's audio goes only to the computer its route
// names; with that pairing forgotten or replaced the meeting says to pair with it again.
TEST_CASE("a meeting whose host is no longer paired asks to pair with it again") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    Computer laptop(folder.path() / "laptop", "Laptop", reactor, clock, io,
                    std::make_shared<testing::FakeHostEngine>());
    auto route = std::make_shared<Json>(Json{{"fingerprint", std::string(64, 'A')},
                                             {"host_name", "devbox"},
                                             {"family", "parakeet"},
                                             {"model", "parakeet-v3"}});
    auto made = laptop.service->make_meeting_backend(route);
    const auto readiness = made->load(CancelToken{});
    CHECK(readiness.state != speech::ReadyState::Ready);
    CHECK(readiness.message == "Pair with devbox again to transcribe this meeting. Its audio "
                               "will not be sent to a different computer.");

    remote::ClientPairing other;
    other.host = "127.0.0.1";
    other.fingerprint = std::string(64, 'B');
    other.host_name = "other";
    remote::write_pairing(laptop.settings, other);
    CHECK(made->load(CancelToken{}).message ==
          "Pair with devbox again to transcribe this meeting. Its audio will not be sent to a "
          "different computer.");
}
