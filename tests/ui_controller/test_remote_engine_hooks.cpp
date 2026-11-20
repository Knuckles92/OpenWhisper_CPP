// The controller and the Remote engine (services/application_controller.py with
// services/runtime/remote.py RemoteRuntime): the readiness check reconnects rather than reloads,
// a host that drops or returns updates the engine card, and a meeting's engine lease goes through
// the controller and leaves a Remote engine connected. The Remote engine is a scripted backend
// here (its own link is tested on the real one in tests/remote_host).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QLabel>

#include "controller_support.h"
#include "meeting/model_lease.h"
#include "ui_record/quick_record_tab.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;
using namespace std::chrono_literals;
namespace speech = fv::speech;

namespace {

// What the scripted Remote engine reports, and what was done to it.
struct Link {
    std::mutex mutex;
    speech::Readiness loads_as{speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 on devbox | cpu",
                               ""};
    fv::Executor* notify = nullptr;
    std::function<void(speech::Readiness)> report;
    std::atomic<int> loads{0};
    std::atomic<int> releases{0};
    // What a decode hears; empty: it fails as a disconnected host does.
    std::string heard;

    // The link's own report, as RemoteSpeechBackend's keeper makes it between loads.
    void push(speech::Readiness readiness) {
        std::lock_guard lock(mutex);
        if (notify && report) {
            notify->post([report = report, readiness] { report(readiness); });
        }
    }
};

class LinkBackend final : public speech::Backend {
public:
    explicit LinkBackend(std::shared_ptr<Link> link) : link_(std::move(link)) {}
    std::string_view id() const override { return "remote"; }
    speech::Capabilities capabilities() const override { return {}; }
    speech::Readiness load(const fv::CancelToken&) override {
        ++link_->loads;
        std::lock_guard lock(link_->mutex);
        return link_->loads_as;
    }
    speech::DecodeResult decode(const speech::DecodeJob&) override {
        std::lock_guard lock(link_->mutex);
        if (link_->heard.empty()) {
            throw fv::EngineError("Remote engine is not connected");
        }
        speech::DecodeResult result;
        result.transcript.text = link_->heard;
        return result;
    }
    void release() override { ++link_->releases; }
    fv::Subscription watch_readiness(fv::Executor& notify,
                                     std::function<void(speech::Readiness)> report) override {
        std::lock_guard lock(link_->mutex);
        link_->notify = &notify;
        link_->report = std::move(report);
        return {};
    }

private:
    std::shared_ptr<Link> link_;
};

RigOptions remoteSelected(const std::shared_ptr<Link>& link) {
    RigOptions options;
    options.settings["selected_model"] = "remote";
    options.backends = [link](speech::BackendFactory fallback) -> speech::BackendFactory {
        return [link,
                fallback](const speech::BackendConfig& config) -> std::unique_ptr<speech::Backend> {
            if (config.backend == "remote") {
                return std::make_unique<LinkBackend>(link);
            }
            return fallback(config);
        };
    };
    return options;
}

struct Hooks {
    std::atomic<int> retries{0};
    std::atomic<bool> restoring{false};
};

void install(ControllerRig& rig, const std::shared_ptr<Hooks>& hooks) {
    rig.app().set_remote_engine(fv::RemoteEngineHooks{
        .retry_now = [hooks] { ++hooks->retries; },
        .restoring = [hooks] { return hooks->restoring.load(); },
        .host_name = [] { return std::string("devbox"); },
    });
}

bool settledOn(ControllerRig& rig, speech::ReadyState state) {
    return waitUntil([&rig, state] {
        return rig.app().reload_machine().settled() &&
               rig.speech().readiness(speech::Slot::Dictation).state == state;
    });
}

} // namespace

// from tests/test_remote_engine.py::test_readiness_retries_a_remote_engine_that_could_not_connect
TEST_CASE("test_readiness_retries_a_remote_engine_that_could_not_connect",
          "[test_remote_engine][widget]") {
    auto link = std::make_shared<Link>();
    link->loads_as = {speech::ReadyState::Offline, "Couldn't reach devbox:47821.",
                      "Couldn't reach devbox:47821."};
    ControllerRig rig(remoteSelected(link));
    auto hooks = std::make_shared<Hooks>();
    install(rig, hooks);
    rig.mainWindowShown();
    REQUIRE(settledOn(rig, speech::ReadyState::Offline));
    const int loads = link->loads;

    CHECK(rig.app().transcription_readiness_message() ==
          std::optional<std::string>("Couldn't reach devbox:47821. Trying again..."));
    // A quiet reconnect rather than a full engine reload.
    CHECK(hooks->retries == 1);
    QApplication::processEvents();
    CHECK(rig.app().reload_machine().settled());
    CHECK(link->loads == loads);

    // While a reconnect runs, and before any attempt failed, it is "Connecting to".
    hooks->restoring = true;
    CHECK(rig.app().transcription_readiness_message() ==
          std::optional<std::string>("Connecting to devbox..."));
    CHECK(hooks->retries == 2);
    // A dictation start refused on it says the same on the status line.
    CHECK_FALSE(rig.app().start_recording());
    CHECK(rig.hasStatus("Connecting to devbox..."));
}

// RemoteRuntime._on_watch_tick and _on_settled: a host that drops is said at once, one that stays
// away is not said again, and its return updates the card without a reload.
TEST_CASE("a remote host that drops or returns updates the engine card without a reload",
          "[test_remote_engine][widget]") {
    auto link = std::make_shared<Link>();
    ControllerRig rig(remoteSelected(link));
    install(rig, std::make_shared<Hooks>());
    REQUIRE(rig.startEngine());
    std::vector<std::pair<std::string, std::optional<bool>>> shown;
    auto seen = rig.app().device_info_update.connect(
        rig.uiExecutor(), [&shown](std::string info, std::optional<bool> usable) {
            shown.emplace_back(info, usable);
        });
    const int loads = link->loads;

    link->push({speech::ReadyState::Offline, "The connection to devbox closed.",
                "The connection to devbox closed."});
    REQUIRE(waitUntil([&shown] { return !shown.empty(); }));
    CHECK(shown.back() ==
          std::pair<std::string, std::optional<bool>>("The connection to devbox closed.", false));
    link->push({speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 on devbox | cpu", ""});
    REQUIRE(waitUntil([&shown] { return shown.size() == 2; }));
    CHECK(shown.back() == std::pair<std::string, std::optional<bool>>(
                              "Parakeet TDT 0.6B v3 on devbox | cpu", true));
    CHECK(link->loads == loads);
    CHECK(rig.app().reload_machine().settled());
    CHECK(rig.app().transcription_readiness_message() == std::nullopt);
}

// from tests/test_remote_engine.py::test_a_meeting_leaves_the_remote_connection_alone
TEST_CASE("test_a_meeting_leaves_the_remote_connection_alone", "[test_remote_engine][widget]") {
    auto link = std::make_shared<Link>();
    ControllerRig rig(remoteSelected(link));
    REQUIRE(rig.startEngine());
    std::vector<std::string> shown;
    auto seen = rig.app().device_info_update.connect(
        rig.uiExecutor(),
        [&shown](std::string info, std::optional<bool>) { shown.push_back(info); });
    const int loads = link->loads;

    CHECK_FALSE(rig.app().release_local_engine());
    QApplication::processEvents();
    CHECK(link->releases == 0);
    CHECK(rig.speech().readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready);
    CHECK(std::ranges::none_of(
        shown, [](const std::string& info) { return info.find("Released") != std::string::npos; }));
    rig.app().restore_local_engine();
    CHECK(rig.app().reload_machine().settled());
    QApplication::processEvents();
    CHECK(link->loads == loads); // no reconnect

    // The same through the meeting's lease, whose model is its own: the connection stays.
    fv::meeting::SpeechModelLease lease(rig.speech(), [] { return true; });
    lease.route_dictation(rig.app().lease_release_hook(), rig.app().lease_restore_hook());
    auto acquired = std::async(std::launch::async, [&lease] { return lease.acquire(); });
    REQUIRE(waitUntil([&acquired] { return acquired.wait_for(0ms) == std::future_status::ready; }));
    CHECK(acquired.get());
    auto released = std::async(std::launch::async, [&lease] { lease.release(); });
    REQUIRE(waitUntil([&released] { return released.wait_for(0ms) == std::future_status::ready; }));
    QApplication::processEvents();
    CHECK(link->releases == 0);
    CHECK(link->loads == loads);
    CHECK(rig.app().reload_machine().settled());
}

// MeetingRuntime._model_lease is the controller's release_local_engine / restore_local_engine:
// the lease, taken on a meeting's thread, goes through the reload machine.
TEST_CASE("a meeting's engine lease releases and restores through the controller",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    std::vector<std::string> shown;
    auto seen = rig.app().device_info_update.connect(
        rig.uiExecutor(),
        [&shown](std::string info, std::optional<bool>) { shown.push_back(info); });
    std::vector<bool> busy;
    auto busyLink = rig.app().engine_busy_changed.connect(
        rig.uiExecutor(), [&busy](bool value) { busy.push_back(value); });

    fv::meeting::SpeechModelLease lease(rig.speech(), [] { return true; });
    lease.route_dictation(rig.app().lease_release_hook(), rig.app().lease_restore_hook());
    auto acquired = std::async(std::launch::async, [&lease] { return lease.acquire(); });
    REQUIRE(waitUntil([&acquired] { return acquired.wait_for(0ms) == std::future_status::ready; }));
    CHECK(acquired.get());
    REQUIRE(waitUntil([&rig] {
        return rig.speech().readiness(speech::Slot::Dictation).state != speech::ReadyState::Ready;
    }));
    CHECK(rig.app().reload_machine().released_for_lease());
    CHECK(std::ranges::find(shown, "Released while Meeting Mode runs") != shown.end());

    const auto loadsBefore = std::ranges::count(rig.engine().ops(), std::string("load"));
    auto released = std::async(std::launch::async, [&lease] { lease.release(); });
    REQUIRE(waitUntil([&released] { return released.wait_for(0ms) == std::future_status::ready; }));
    REQUIRE(waitUntil([&rig, loadsBefore] {
        return std::ranges::count(rig.engine().ops(), std::string("load")) > loadsBefore &&
               rig.app().reload_machine().settled();
    }));
    CHECK(rig.hasStatus("Reloading speech engine..."));
    CHECK(std::ranges::find(busy, true) != busy.end());
    CHECK_FALSE(rig.app().reload_machine().released_for_lease());
    CHECK(rig.speech().readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready);
}

namespace {

fv::remote::RemoteModels devboxModels() {
    fv::remote::RemoteModels state;
    state.host = "devbox";
    const fv::remote::HostModel v3{"parakeet", "parakeet-v3", "Parakeet TDT 0.6B v3"};
    const fv::remote::HostModel orukeet{"parakeet", "orukeet-v0.1", "Orukeet TDT 0.6B"};
    state.models = std::vector<fv::remote::HostModel>{v3, orukeet};
    state.current = v3;
    state.runtime = fv::Json{{"can_configure", true}};
    return state;
}

} // namespace

// services/runtime/remote.py select_remote_model: the Model field's choice rides on the next
// reload with "Switching <host> to <label>...", is refused during a meeting (the field shows the
// host's model again), and the model already served does nothing.
TEST_CASE("a Model field choice switches the paired computer through the controller",
          "[test_remote_engine][widget]") {
    auto link = std::make_shared<Link>();
    ControllerRig rig(remoteSelected(link));
    std::vector<std::string> requested;
    rig.app().set_remote_engine(fv::RemoteEngineHooks{
        .host_name = [] { return std::string("devbox"); },
        .model_choices = []() -> std::optional<fv::remote::RemoteModels> { return devboxModels(); },
        .request_model =
            [&requested](const std::string& family, const std::string& model,
                         const std::optional<fv::Json>& changes) {
                requested.push_back(family + "/" + model + (changes ? " " + changes->dump() : ""));
            },
    });
    REQUIRE(rig.startEngine());
    const int loads = link->loads;
    int stale = 0;
    auto staleLink = rig.app().remote_models_stale.connect(rig.uiExecutor(), [&stale] { ++stale; });

    // The Model field reads the host's choices through the controller.
    REQUIRE(rig.ui().getRemoteModels);
    CHECK(rig.ui().getRemoteModels() == std::optional<fv::remote::RemoteModels>(devboxModels()));

    rig.ui().onRemoteModelSelected("parakeet", "parakeet-v3"); // already served
    CHECK(requested.empty());

    rig.activity().set_meeting_active(true);
    rig.ui().onRemoteModelSelected("parakeet", "orukeet-v0.1");
    CHECK(requested.empty());
    REQUIRE(waitUntil([&stale] { return stale == 1; }));
    CHECK(rig.hasStatus("End the meeting before changing the engine"));
    rig.activity().set_meeting_active(false);

    rig.ui().onRemoteModelSelected("parakeet", "orukeet-v0.1");
    CHECK(requested == std::vector<std::string>{"parakeet/orukeet-v0.1"});
    REQUIRE(waitUntil([&rig, &link, loads] {
        return link->loads > loads && rig.app().reload_machine().settled();
    }));
    CHECK(rig.hasStatus("Switching devbox to Orukeet TDT 0.6B..."));

    // The runtime fields: "Updating the runtime on <host>...".
    const int before = link->loads;
    rig.ui().onRemoteRuntimeSelected("parakeet", "parakeet-v3", fv::Json{{"device", "cpu"}});
    REQUIRE(requested.size() == 2);
    CHECK(requested.back() == "parakeet/parakeet-v3 {\"device\":\"cpu\"}");
    REQUIRE(waitUntil([&rig, &link, before] {
        return link->loads > before && rig.app().reload_machine().settled();
    }));
    CHECK(rig.hasStatus("Updating the runtime on devbox..."));
}

// RemoteRuntime._publish_link, remote_clients_changed and on_remote_retry reach the record tabs.
TEST_CASE("the remote link, the connected computers and a retry reach the tabs",
          "[test_remote_engine][widget]") {
    auto link = std::make_shared<Link>();
    ControllerRig rig(remoteSelected(link));
    auto hooks = std::make_shared<Hooks>();
    fv::remote::RemoteLink online;
    online.state = "connected";
    online.host = "devbox";
    rig.app().set_remote_engine(fv::RemoteEngineHooks{
        .retry_now = [hooks] { ++hooks->retries; },
        .link = [online]() -> std::optional<fv::remote::RemoteLink> { return online; },
        .connected_clients =
            [] {
                return std::vector<fv::remote::ConnectedClient>{
                    {.device_id = "d1", .name = "Laptop", .busy = true}};
            },
    });
    REQUIRE(rig.startEngine());
    int linkSeen = 0;
    auto linkLink =
        rig.app().remote_link_changed.connect(rig.uiExecutor(), [&linkSeen] { ++linkSeen; });
    std::vector<fv::remote::ConnectedClient> clients;
    auto clientLink = rig.app().remote_clients_changed.connect(
        rig.uiExecutor(),
        [&clients](std::vector<fv::remote::ConnectedClient> now) { clients = std::move(now); });

    std::thread([&rig] {
        rig.app().remote_link_poked();
        rig.app().remote_clients_poked();
    }).join();
    REQUIRE(waitUntil([&] { return linkSeen == 1 && clients.size() == 1; }));
    CHECK(rig.app().remote_link() == std::optional<fv::remote::RemoteLink>(online));
    CHECK(clients[0].name == "Laptop");

    REQUIRE(rig.ui().onRemoteRetry);
    rig.ui().onRemoteRetry();
    CHECK(hooks->retries == 1);
}

// services/runtime/remote.py select_remote_runtime's gate and select_remote_model's recording
// gate: refused in OpenWhisper's words, nothing requested, the fields shown as the host has them.
TEST_CASE("the paired computer's model and runtime wait for the current activity",
          "[test_remote_engine][widget]") {
    auto link = std::make_shared<Link>();
    ControllerRig rig(remoteSelected(link));
    std::vector<std::string> requested;
    rig.app().set_remote_engine(fv::RemoteEngineHooks{
        .host_name = [] { return std::string("devbox"); },
        .model_choices = []() -> std::optional<fv::remote::RemoteModels> { return devboxModels(); },
        .request_model =
            [&requested](const std::string& family, const std::string& model,
                         const std::optional<fv::Json>&) {
                requested.push_back(family + "/" + model);
            },
    });
    REQUIRE(rig.startEngine());
    int stale = 0;
    auto staleLink = rig.app().remote_models_stale.connect(rig.uiExecutor(), [&stale] { ++stale; });

    rig.activity().set_meeting_active(true);
    rig.ui().onRemoteRuntimeSelected("parakeet", "parakeet-v3", fv::Json{{"device", "cpu"}});
    REQUIRE(waitUntil([&stale] { return stale == 1; }));
    CHECK(rig.hasStatus("Finish the current activity before changing the remote runtime"));
    rig.activity().set_meeting_active(false);

    // A runtime change for a model the host no longer serves: the backend's own sentence.
    rig.ui().onRemoteRuntimeSelected("parakeet", "orukeet-v0.1", fv::Json{{"device", "cpu"}});
    REQUIRE(waitUntil([&stale] { return stale == 2; }));
    CHECK(rig.hasStatus("The host changed engines. Reconnect before changing its runtime."));

    REQUIRE(rig.app().start_recording());
    rig.ui().onRemoteModelSelected("parakeet", "orukeet-v0.1");
    REQUIRE(waitUntil([&stale] { return stale == 3; }));
    CHECK(rig.hasStatus("Finish recording before changing the engine"));
    (void)rig.app().cancel();
    CHECK(requested.empty());
}

// transcription.py _timed_decode through set_transcription_stats: a dictation decoded on the
// paired computer puts "on <host> · N ms network" under the Quick Record card's stats.
TEST_CASE("a remote dictation's stats name the host and the network's share",
          "[test_remote_link_ui][widget]") {
    auto link = std::make_shared<Link>();
    link->heard = "Hello from devbox.";
    ControllerRig rig(remoteSelected(link));
    rig.runtime().set_remote_timing(
        []() -> std::function<std::optional<fv::remote::RemoteTiming>()> {
            return [] {
                return std::optional<fv::remote::RemoteTiming>(
                    fv::remote::RemoteTiming{"devbox", 2, 0.5, 0.469});
            };
        });
    REQUIRE(rig.startEngine());
    REQUIRE(rig.app().start_recording());
    REQUIRE(waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    rig.app().stop_recording();
    REQUIRE(waitUntil([&] { return !rig.runtime().has_active_job(); }, 30s));
    auto* detail = rig.quickRecord()->findChild<QLabel*>(QStringLiteral("statsRemoteDetail"));
    REQUIRE(detail != nullptr);
    REQUIRE(waitUntil([detail] { return !detail->text().isEmpty(); }));
    CHECK(detail->text() == QStringLiteral(u"on devbox · 31 ms network"));
    CHECK_FALSE(detail->isHidden());
}
