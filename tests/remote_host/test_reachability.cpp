// What the host service says about reaching it, renaming and its engine
// (test_remote_pairing_ux.py's reachability functions, test_remote_engine_push.py's service
// functions and test_remote_rename.py's service functions): reachability.{h,cpp} and
// RemoteEngineService over a pinned address list, pinned network profiles and a counting record
// kind, on 127.0.0.1.

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "host_rig.h"
#include "net/lan_address.h"
#include "net/reactor.h"
#include "remote/protocol.h"
#include "remote/remote_backend.h"
#include "remote/testing/fake_discovery_bus.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/host_engine.h"
#include "remote_host/reachability.h"
#include "remote_host/service.h"
#include "remote_host/testing/fake_host_engine.h"
#include "remote_records/host_store.h"
#include "settings/store.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::remote_host;

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

// A record kind that counts what it is asked to rename, and owns one record per device asked.
class CountingKind final : public remote_records::RecordKind {
public:
    std::vector<std::pair<std::string, std::string>> renamed;
    int renames = 1;

    Json import_record(std::string_view, const std::filesystem::path&, const Json&,
                       const remote::PairedDevice&) override {
        return Json::object();
    }
    Json list(std::string_view, std::string_view, int) override { return Json::array(); }
    remote_records::Files export_record(std::string_view, std::string_view,
                                        const std::filesystem::path&) override {
        return {};
    }
    Json stored_files(std::string_view, std::string_view) override { return Json::object(); }
    bool remove(std::string_view, std::string_view) override { return false; }
    int delete_all(std::string_view) override { return 0; }
    int rename_owner(std::string_view device, std::string_view name) override {
        renamed.emplace_back(device, name);
        return renames;
    }
    Json summary(std::string_view) override { return Json::object(); }
    std::vector<remote::RecordOwnerGroup> owners() override { return {}; }
    bool owns(std::string_view, std::string_view) override { return false; }
};

struct Rig {
    MapEnvReader env;
    Paths paths;
    SettingsStore settings{paths.user_file(identity().settings_file)};
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore credentials{backend, identity()};
    std::shared_ptr<remote::testing::FakeTailscaleCli> tailscale =
        std::make_shared<remote::testing::FakeTailscaleCli>();
    std::shared_ptr<RemoteEngineService> service;

    Rig(const std::filesystem::path& root, std::string name, net::Reactor& reactor, Clock& clock,
        Executor& io, const std::function<void(RemoteEngineServiceDeps&)>& adjust = {},
        std::shared_ptr<remote_records::HostRecordStore> records = {})
        : env(private_env(root)), paths(Paths::resolve(identity(), env)) {
        RemoteEngineServiceDeps deps{.paths = paths,
                                     .identity = identity(),
                                     .settings = settings,
                                     .credentials = credentials,
                                     .env = env,
                                     .clock = clock,
                                     .io = io,
                                     .client_env = {reactor},
                                     .computer_name = std::move(name),
                                     .tailscale = tailscale,
                                     .engine = [] { return unavailable_engine("No engine."); },
                                     .records = std::move(records),
                                     .bind = "127.0.0.1",
                                     .discovery_port = std::nullopt,
                                     .listen_port = 0};
        if (adjust) {
            adjust(deps);
        }
        service = std::make_shared<RemoteEngineService>(std::move(deps));
    }
    ~Rig() { service->shutdown(); }
};

std::vector<std::string> kinds(const std::vector<remote::Note>& notes) {
    std::vector<std::string> out;
    for (const auto& note : notes) {
        out.push_back(note.kind);
    }
    return out;
}

} // namespace

TEST_CASE("test_network_profiles_parse_from_either_powershell", "[unit]") {
    CHECK(parse_profiles(
              R"([{"Name":"Home","InterfaceAlias":"Wi-Fi","NetworkCategory":0},)"
              R"({"Name":"Tailscale","InterfaceAlias":"Tailscale","NetworkCategory":1}])") ==
          std::vector<NetworkProfile>{{"Home", "Wi-Fi", "public"},
                                      {"Tailscale", "Tailscale", "private"}});
    CHECK(
        parse_profiles(
            R"({"Name":"Office","InterfaceAlias":"Ethernet","NetworkCategory":"DomainAuthenticated"})") ==
        std::vector<NetworkProfile>{{"Office", "Ethernet", "domain"}});
    CHECK(parse_profiles("").empty());
    CHECK(parse_profiles("not json").empty());
    // An unknown category is left out; a missing name reads "".
    CHECK(parse_profiles(R"([{"InterfaceAlias":"Wi-Fi","NetworkCategory":"Public"},)"
                         R"({"Name":"X","NetworkCategory":7}])") ==
          std::vector<NetworkProfile>{{"", "Wi-Fi", "public"}});
}

TEST_CASE("test_a_public_network_a_vpn_and_a_taken_port_each_say_what_to_do", "[unit]") {
    const auto lan = net::make_local_address("192.168.1.20", "Wi-Fi", net::kLan, 24, true);
    const auto vpn = net::make_local_address("10.2.0.2", "ProtonVPN", net::kVpn, 32);
    const std::vector<NetworkProfile> publicNetwork{{"Home", "Wi-Fi", "public"}};
    const auto notes = host_notes(lan, {lan, vpn}, publicNetwork, "UDP port 47821 is in use");
    CHECK(kinds(notes) == std::vector<std::string>{"public_network", "vpn", "discovery"});
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].message ==
          "Windows treats \"Home\" as a public network, which usually keeps other computers "
          "from connecting here. If they can't, make it a private network.");
    CHECK(notes[0].url == "ms-settings:network-wifi");
    CHECK(notes[0].action == "Open network settings");
    CHECK(notes[2].message == "Other computers can't find this one by searching (UDP port 47821 "
                              "is in use). They can still enter its address and pairing code.");
    const std::vector<NetworkProfile> privateNetwork{{"Home", "Wi-Fi", "private"}};
    CHECK(host_notes(lan, {lan}, privateNetwork).empty());
    CHECK(host_notes(lan, {lan}, std::nullopt).empty());
    // Unnamed, and on a cable.
    const auto wired = net::make_local_address("192.168.1.21", "Ethernet 2", net::kLan, 24, true);
    const auto unnamed =
        host_notes(wired, {wired}, std::vector<NetworkProfile>{{"", "Ethernet 2", "public"}});
    REQUIRE(unnamed.size() == 1);
    CHECK(unnamed[0].message.starts_with("Windows treats this network as a public network"));
    CHECK(unnamed[0].url == "ms-settings:network-ethernet");
    CHECK(settings_page("Cellular") == "ms-settings:network-status");
}

TEST_CASE("the sharing host lists reachability notes from the profiles it read", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io;
    int reads = 0;
    const auto lan = net::make_local_address("192.168.1.20", "Wi-Fi", net::kLan, 24, true);
    Rig host(folder.path(), "devbox", reactor, clock, io, [&](RemoteEngineServiceDeps& deps) {
        deps.local_addresses = [lan] {
            return std::vector<net::LocalAddress>{lan};
        };
        deps.network_profiles = [&reads] {
            ++reads;
            return std::vector<NetworkProfile>{{"Home", "Wi-Fi", "public"}};
        };
    });
    std::vector<std::string> events;
    const auto watching = host.service->changed.connect(
        io, [&](std::string event) { events.push_back(std::move(event)); });
    host.service->set_host_enabled(true);
    // Sharing just started: the profiles are read fresh, in the background.
    auto state = host.service->host_state();
    REQUIRE(state.running);
    CHECK(state.notes.empty());
    io.run_all();
    CHECK(reads == 1);
    CHECK(std::ranges::find(events, "state") != events.end());
    state = host.service->host_state();
    CHECK(kinds(state.notes) == std::vector<std::string>{"public_network"});
    CHECK(state.address == remote::format_address("192.168.1.20", state.port));
    CHECK(state.address_kind == "lan");
    // Within NETWORK_PROFILES_MAX_AGE_S they are not read again.
    io.run_all();
    CHECK(reads == 1);
}

TEST_CASE("test_host_state_labels_a_vpn_address", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    const auto vpn = net::make_local_address("10.2.0.2", "proton0", net::kVpn, 32);
    Rig host(folder.path() / "host", "devbox", reactor, clock, io,
             [vpn](RemoteEngineServiceDeps& deps) {
                 deps.local_addresses = [vpn] {
                     return std::vector<net::LocalAddress>{vpn};
                 };
             });
    host.service->set_host_enabled(true);
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    CHECK(state.address == remote::format_address("10.2.0.2", state.port));
    CHECK(state.address_kind == "vpn");
    // Nobody is told to reach it there: a pairing's alternates leave the VPN address out.
    Rig client(folder.path() / "client", "laptop", reactor, clock, io);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    const auto paired =
        client.service->pair(remote::format_address("127.0.0.1", state.port), *code, false);
    CHECK(std::ranges::find(paired.alternates, "10.2.0.2") == paired.alternates.end());
}

TEST_CASE("test_service_refreshes_the_page_and_pushes_the_change", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io;
    Rig host(folder.path(), "devbox", reactor, clock, io);
    rig::InlineExecutor here;
    std::vector<std::string> events;
    const auto watching = host.service->changed.connect(
        here, [&](std::string event) { events.push_back(std::move(event)); });
    host.service->engine_changed();
    // Not sharing: nothing to push, the page is still told.
    CHECK(events == std::vector<std::string>{"engine"});
    io.run_all();
    events.clear();
    host.service->set_host_enabled(true);
    io.run_all();
    events.clear();
    host.service->engine_changed();
    CHECK(events == std::vector<std::string>{"engine"});
    // The host tells its connections off the caller's thread.
    CHECK(io.queued() >= 1);
    io.run_all();
}

TEST_CASE("test_the_service_renames_a_paired_computer_on_what_it_keeps_too", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto kind = std::make_shared<CountingKind>();
    auto store = std::make_shared<remote_records::HostRecordStore>(
        folder.path() / "transfers", remote_records::HostRecordStore::Kinds{{"dictation", kind}},
        clock, [](const std::filesystem::path&) { return 8ULL << 30; });
    std::string device;
    {
        // Paired while sharing; the host has since stopped, as Python's registry-only case.
        Rig earlier(folder.path() / "host", "devbox", reactor, clock, io, {}, store);
        Rig client(folder.path() / "client", "DESKTOP-LAPTOP", reactor, clock, io);
        earlier.service->set_host_enabled(true);
        const auto code = earlier.service->open_pairing();
        REQUIRE(code);
        device = client.service
                     ->pair(remote::format_address("127.0.0.1", earlier.service->host_state().port),
                            *code, false)
                     .device_id;
        earlier.service->set_host_enabled(false);
    }
    Rig host(folder.path() / "host", "devbox", reactor, clock, io, {}, store);
    std::vector<std::string> events;
    const auto watching = host.service->changed.connect(
        io, [&](std::string event) { events.push_back(std::move(event)); });

    const auto renamed = host.service->rename_device(device, "Kitchen laptop");
    const auto heard = events;
    REQUIRE(renamed);
    CHECK(renamed->name == "Kitchen laptop");
    CHECK(heard == std::vector<std::string>{"devices", "records"});
    REQUIRE(host.service->host_state().devices.size() == 1);
    CHECK(host.service->host_state().devices[0].paired_name == "DESKTOP-LAPTOP");
    CHECK(kind->renamed ==
          std::vector<std::pair<std::string, std::string>>{{device, "Kitchen laptop"}});
    CHECK_FALSE(host.service->rename_device("not-paired", "Studio"));

    // Nothing kept here was renamed: no "records".
    kind->renames = 0;
    events.clear();
    REQUIRE(host.service->rename_device(device, "Den laptop"));
    CHECK(events == std::vector<std::string>{"devices"});
}

TEST_CASE("test_the_service_renames_its_host_and_says_so", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    Rig host(folder.path() / "host", "DESKTOP-4K2J9", reactor, clock, io);
    Rig client(folder.path() / "client", "laptop", reactor, clock, io);
    std::vector<std::string> events;
    const auto watching = client.service->changed.connect(
        io, [&](std::string event) { events.push_back(std::move(event)); });
    CHECK_FALSE(client.service->rename_host("Studio PC"));
    CHECK(events.empty());

    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    (void)client.service->pair(remote::format_address("127.0.0.1", host.service->host_state().port),
                               *code, false);
    events.clear();
    const auto renamed = client.service->rename_host("Studio PC");
    REQUIRE(renamed);
    CHECK(renamed->host_name == "Studio PC");
    CHECK(events == std::vector<std::string>{"host_renamed"});
    REQUIRE(client.service->client_pairing());
    CHECK(client.service->client_pairing()->host_name == "Studio PC");
}

TEST_CASE("test_dictation_calls_the_host_its_new_name_without_reconnecting", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto engine = std::make_shared<remote_host::testing::FakeHostEngine>();
    Rig host(folder.path() / "host", "devbox", reactor, clock, io,
             [engine](RemoteEngineServiceDeps& deps) {
                 deps.engine = [engine] {
                     return engine;
                 };
             });
    Rig client(folder.path() / "client", "laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto code = host.service->open_pairing();
    REQUIRE(code);
    (void)client.service->pair(remote::format_address("127.0.0.1", host.service->host_state().port),
                               *code, false);
    auto made = client.service->make_backend();
    auto* backend = dynamic_cast<remote::RemoteSpeechBackend*>(made.get());
    REQUIRE(backend != nullptr);
    REQUIRE(backend->load({}).state == speech::ReadyState::Ready);
    const auto label = engine->describe().at("label").get<std::string>();
    CHECK(backend->name() == label + " on devbox");
    const auto connections = host.service->connected_clients().size();
    int changes = 0;
    const auto watching = backend->link_changed.connect(io, [&] { ++changes; });

    REQUIRE(client.service->rename_host("Studio PC"));
    CHECK(backend->name() == label + " on Studio PC");
    CHECK(backend->link().host == "Studio PC");
    CHECK(changes >= 1);
    // The same connection: nothing reconnected.
    CHECK(backend->link().state == "connected");
    CHECK(host.service->connected_clients().size() == connections);
    // A reload keeps the new name.
    REQUIRE(backend->load({}).state == speech::ReadyState::Ready);
    CHECK(backend->name() == label + " on Studio PC");
}

TEST_CASE("test_an_allowed_request_pairs_with_the_number_both_screens_showed", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    const auto lan = net::make_local_address("192.0.2.10", "Ethernet", net::kLan, 24, true);
    Rig host(folder.path() / "host", "devbox", reactor, clock, io, [lan](auto& deps) {
        deps.local_addresses = [lan] {
            return std::vector<net::LocalAddress>{lan};
        };
    });
    Rig client(folder.path() / "client", "laptop", reactor, clock, io);
    host.service->set_host_enabled(true);
    const auto port = host.service->host_state().port;

    std::mutex lock;
    std::vector<std::string> codes;
    std::optional<remote::ClientPairing> result;
    std::exception_ptr failure;
    std::thread asking([&] {
        try {
            auto paired = client.service->request_pairing(remote::format_address("127.0.0.1", port),
                                                          [&](std::string sas) {
                                                              std::lock_guard held(lock);
                                                              codes.push_back(std::move(sas));
                                                          },
                                                          {});
            std::lock_guard held(lock);
            result = std::move(paired);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    REQUIRE(rig::wait_for([&] { return host.service->pending_pair_request().has_value(); },
                          std::chrono::seconds(5)));
    const auto pending = *host.service->pending_pair_request();
    CHECK(pending.name == "laptop");
    CHECK(pending.address == "127.0.0.1");
    CHECK(pending.seconds_left > 100);
    CHECK(pending.seconds_left <= 120);
    REQUIRE(rig::wait_for(
        [&] {
            std::lock_guard held(lock);
            return !codes.empty();
        },
        std::chrono::seconds(5)));
    {
        std::lock_guard held(lock);
        CHECK(codes == std::vector<std::string>{pending.sas});
    }

    CHECK(host.service->answer_pair_request(pending.id, true));
    asking.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    REQUIRE(result);
    CHECK(result->via == "approval");
    CHECK(result->host_name == "devbox");
    CHECK(result->alternates == std::vector<std::string>{"192.0.2.10"});
    const auto devices = host.service->host_state().devices;
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].via == "approval");
    CHECK(devices[0].name == "laptop");
    CHECK_FALSE(host.service->pending_pair_request());
    CHECK_FALSE(host.service->answer_pair_request(pending.id, true)); // answered once
    // The token it was given connects.
    auto connection = client.service->client_connection();
    REQUIRE(connection);
    CHECK(connection->connect().at("host").at("name") == "devbox");
    connection->close();
}

TEST_CASE("the pairing question's grants follow what this host allows", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    auto store = std::make_shared<remote_records::HostRecordStore>(
        folder.path() / "transfers",
        remote_records::HostRecordStore::Kinds{{"dictation", std::make_shared<CountingKind>()}},
        clock, [](const std::filesystem::path&) { return 8ULL << 30; });
    Rig host(folder.path() / "host", "devbox", reactor, clock, io, {}, store);
    CHECK(host.service->pairing_grants().empty());
    host.service->set_keep_records(true);
    CHECK(host.service->pairing_grants() ==
          std::vector<std::string>{"keep its dictations and finished meetings here"});
    // Paired computers' MCP is a session the host grants later, never part of pairing
    // (a17-paired-mcp): without one, the old switch is refused.
    CHECK_THROWS(host.service->set_manage_mcp(true));
    CHECK(host.service->pairing_grants().size() == 1);
}

// A17-33: service.py's _reachability_notes on a running host. A VPN beside the LAN address says
// to allow local network access, and discovery that could not listen says others can still
// enter the address; a private network adds nothing.
TEST_CASE("the sharing host notes a VPN and discovery that could not listen", "[unit]") {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    ManualExecutor io;
    fv::remote::testing::FakeDiscoveryBus bus;
    bus.fail_bind("127.0.0.1", remote::DISCOVERY_PORT,
                  "Only one usage of each socket address is normally permitted");
    const auto lan = net::make_local_address("192.168.1.20", "Wi-Fi", net::kLan, 24, true);
    const auto vpn = net::make_local_address("10.2.0.2", "ProtonVPN", net::kVpn, 32);
    Rig host(folder.path(), "devbox", reactor, clock, io, [&](RemoteEngineServiceDeps& deps) {
        deps.client_env.discovery = bus.factory("127.0.0.1");
        deps.discovery_port = remote::DISCOVERY_PORT;
        deps.local_addresses = [lan, vpn] {
            return std::vector<net::LocalAddress>{lan, vpn};
        };
        deps.network_profiles = [] {
            return std::vector<NetworkProfile>{{"Home", "Wi-Fi", "private"}};
        };
    });
    host.service->set_host_enabled(true);
    io.run_all();
    const auto state = host.service->host_state();
    REQUIRE(state.running);
    CHECK(kinds(state.notes) == std::vector<std::string>{"vpn", "discovery"});
    REQUIRE(state.notes.size() == 2);
    CHECK(state.notes[0].message == "A VPN is on. If other computers can't connect, allow local "
                                    "network (LAN) access in its settings.");
    CHECK(state.notes[0].action.empty());
    CHECK(state.notes[1].message ==
          "Other computers can't find this one by searching (UDP port 47821 is in use (Only one "
          "usage of each socket address is normally permitted)). They can still enter its address "
          "and pairing code.");
    // The address shown stays the LAN one, not the VPN's.
    CHECK(state.address_kind == "lan");
}
