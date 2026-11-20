// Quit and a model request a paired host never answers: remote_model_request connects and waits
// under the service's shutdown token, as remote_mcp_request does, so shutdown() (Quit's
// RemoteEngine step, before the io strand is waited for) ends a select_model's 250 s wait at
// once instead of holding Quit.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "host_rig.h"
#include "remote/pairing_settings.h"
#include "remote/protocol.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/host_engine.h"
#include "remote_host/service.h"
#include "settings/store.h"

using namespace fv;
using namespace std::chrono_literals;
namespace rig = fv::remote_host::rig;

namespace {

// The host's model handler, held until the test opens it.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;
};

MapEnvReader private_env(const std::filesystem::path& root) {
    return MapEnvReader{
        {std::string(identity().env_prefix) + "DATA_DIR", path_to_utf8(root)},
        {"LOCALAPPDATA", path_to_utf8(root / "local")},
        {"XDG_DATA_HOME", path_to_utf8(root / "local")},
        {"HOME", path_to_utf8(root / "home")},
        {"USERPROFILE", path_to_utf8(root / "home")},
    };
}

} // namespace

TEST_CASE("sweep: shutdown ends a model request the paired host never answers") {
    auto gate = std::make_shared<Gate>();
    rig::HostRig host([gate](remote_host::SpeechHostDeps& deps) {
        deps.model_management = [] {
            return true;
        };
        deps.manage_models = [gate](std::string_view, const Json&, std::string_view) -> Json {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->open; });
            return Json{{"models", Json::array()}};
        };
    });
    const rig::Paired paired = host.pair();
    REQUIRE_FALSE(paired.token.empty());

    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    const MapEnvReader env = private_env(folder.path() / "client");
    const Paths paths = Paths::resolve(identity(), env);
    SettingsStore settings{paths.user_file(identity().settings_file)};
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    remote::PairingResult result;
    result.token = paired.token;
    result.device_id = paired.device_id;
    result.host_name = "devbox";
    result.fingerprint = paired.fingerprint;
    remote::save_client_pairing(settings, credentials, paths, identity(), "127.0.0.1", host.port,
                                result, clock);
    auto service =
        std::make_shared<remote_host::RemoteEngineService>(remote_host::RemoteEngineServiceDeps{
            .paths = paths,
            .identity = identity(),
            .settings = settings,
            .credentials = credentials,
            .env = env,
            .clock = clock,
            .io = io,
            .client_env = {reactor},
            .computer_name = "Laptop",
            .tailscale = std::make_shared<remote::testing::FakeTailscaleCli>(),
            .engine = [] { return remote_host::unavailable_engine("No speech engine."); },
            .bind = "127.0.0.1",
            .discovery_port = std::nullopt,
            .listen_port = std::nullopt});

    auto request = std::async(std::launch::async, [&service] {
        try {
            (void)service->remote_model_request("model_catalog", Json::object());
            return std::string("answered");
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, 10s, [&] { return gate->entered; }));
    }
    const auto quitAt = std::chrono::steady_clock::now();
    service->shutdown();
    const bool ended = request.wait_for(2s) == std::future_status::ready;
    {
        std::lock_guard lock(gate->mutex);
        gate->open = true;
    }
    gate->changed.notify_all();
    CHECK(ended);
    CHECK(std::chrono::steady_clock::now() - quitAt < 3s);
    CHECK(request.get() != "answered");
}
