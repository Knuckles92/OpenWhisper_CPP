// remote/tailscale_cli.h against services/remote_asr/tailscale.py: the ported tests of
// tests/test_remote_engine.py, the whois vectors
// (tests/golden/services.remote_asr.tailscale_whois.json), and the CLI runs over a fake launcher
// and, on Windows, over a real child that stands in for tailscale.exe.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "net/tailscale.h"
#include "process/testing/fake_launcher.h"
#include "remote/protocol.h"
#include "remote/tailscale_cli.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_test_support.h"

using namespace fv;
using namespace fv::remote;
using fv::remote::test::TempFolder;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kModule = "services.remote_asr.tailscale_whois";
constexpr std::string_view kOwner = "owner@example.com";

// _status_json of tests/test_remote_engine.py: `tailscale status --json` trimmed to the fields
// read, with made-up names.
Json status_json() {
    return Json::parse(R"({
        "BackendState": "Running",
        "Self": {"HostName": "LAPTOP-1", "DNSName": "laptop.tail0000.ts.net.", "UserID": 1,
                 "TailscaleIPs": ["100.64.0.1", "fd7a:115c:a1e0::1"]},
        "CurrentTailnet": {"Name": "owner@example.com", "MagicDNSSuffix": "tail0000.ts.net"},
        "User": {"1": {"LoginName": "owner@example.com"}, "2": {"LoginName": "friend@example.com"},
                 "3": {"LoginName": "tagged-devices"}},
        "Peer": {
            "a": {"HostName": "devbox", "DNSName": "devbox.tail0000.ts.net.", "OS": "linux",
                  "Online": true, "UserID": 1, "TailscaleIPs": ["100.64.0.2", "fd7a:115c:a1e0::2"]},
            "b": {"HostName": "friends-pc", "DNSName": "friends-pc.tail0000.ts.net.",
                  "OS": "windows", "Online": true, "UserID": 2, "TailscaleIPs": ["100.64.0.3"]},
            "c": {"HostName": "gpu-server", "DNSName": "gpu.tail0000.ts.net.", "OS": "linux",
                  "Online": true, "UserID": 3, "Tags": ["tag:server"],
                  "TailscaleIPs": ["100.64.0.4"]},
            "d": {"HostName": "localhost", "DNSName": "phone.tail0000.ts.net.", "OS": "iOS",
                  "Online": true, "UserID": 1, "TailscaleIPs": ["100.64.0.5"]},
            "e": {"HostName": "Shared Mac", "DNSName": "mac.other.ts.net.", "OS": "macOS",
                  "Online": false, "UserID": 1, "ShareeNode": true, "TailscaleIPs": ["100.64.0.6"]}
        }
    })");
}

// A folder on PATH holding an (empty) tailscale.EXE, so find_cli finds a CLI that the fake
// launcher then "runs". The files these tests make are spelled as PATHEXT spells their
// extension, so that the Windows lookup also finds them on a case-sensitive file system (the
// Linux gate).
struct FakeCliRig {
    FakeCliRig() {
        std::filesystem::create_directories(folder / "bin");
        std::ofstream(folder / "bin" / "tailscale.EXE").put('\0');
        env.set("PATH", path_to_utf8(folder / "bin"));
        env.set("PATHEXT", ".COM;.EXE");
    }
    ProcessTailscaleCli cli(double timeout_s = TAILSCALE_TIMEOUT_S) {
        return ProcessTailscaleCli(env, launcher, timeout_s, CliPlatform::win32);
    }

    TempFolder folder;
    MapEnvReader env;
    std::shared_ptr<FakeProcessLauncher> launcher = std::make_shared<FakeProcessLauncher>();
};

FakeProcessScript answering(const Json& document) {
    FakeProcessScript script;
    if (document.is_string() && document.get<std::string>().starts_with("error:")) {
        script.exit_code = 1;
        script.stderr_bytes = document.get<std::string>().substr(6) + "\n";
    } else {
        script.stdout_bytes = dumps_default(document);
    }
    return script;
}

Json identity_record(const std::optional<PeerIdentity>& identity) {
    if (!identity) {
        return nullptr;
    }
    return {{"node", identity->node}, {"owner", identity->owner}, {"tagged", identity->tagged}};
}

} // namespace

// ---- tests/test_remote_engine.py ----

TEST_CASE("tailscale status marks only the owners untagged computers as mine",
          "[test_remote_engine]") {
    // from
    // tests/test_remote_engine.py::test_tailscale_status_marks_only_the_owners_untagged_computers_as_mine
    const auto status = net::parse_status(status_json());
    CHECK(status.running());
    CHECK(status.owner == kOwner);
    CHECK(status.name == "laptop");
    CHECK(status.address == "100.64.0.1");
    std::map<std::string, net::TailscalePeer> peers;
    for (const auto& peer : status.peers) {
        peers[peer.name] = peer;
    }
    CHECK(peers["devbox"].mine);
    CHECK(peers["devbox"].is_desktop());
    CHECK(peers["devbox"].address == "100.64.0.2"); // IPv4, not the ULA
    CHECK_FALSE(peers["friends-pc"].mine);
    CHECK(peers["friends-pc"].owner == "friend@example.com");
    CHECK_FALSE(peers["gpu"].mine);
    CHECK(peers["gpu"].tagged);
    CHECK(peers["gpu"].owner.empty());
    CHECK_FALSE(peers["phone"].is_desktop());
    CHECK_FALSE(peers["mac"].mine);               // shared in from someone else's tailnet
    CHECK(status.peers.front().name == "devbox"); // the owner's own computers first
}

TEST_CASE("tailscale status states", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_tailscale_status_states
    const auto [state, expected] = GENERATE(table<std::string, std::string>({
        {"Stopped", "stopped"},
        {"NeedsLogin", "needs_login"},
        {"Starting", "starting"},
    }));
    Json data = status_json();
    data["BackendState"] = state;
    const auto status = net::parse_status(data);
    CHECK(status.state == expected);
    CHECK_FALSE(status.running());
}

TEST_CASE("tailscale status never raises", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_tailscale_status_never_raises, with the CLI runs a
    // fake launcher plays: missing, exit 1, unreadable output, too slow, unable to start.
    SECTION("no CLI") {
        MapEnvReader env;
        env.set("PATH", "");
        auto launcher = std::make_shared<FakeProcessLauncher>();
        ProcessTailscaleCli cli(env, launcher, TAILSCALE_TIMEOUT_S, CliPlatform::win32);
        CHECK(cli.status().state == "not_installed");
        CHECK(launcher->specs().empty());
    }
    FakeCliRig rig;
    SECTION("tailscaled isn't running") {
        FakeProcessScript script;
        script.exit_code = 1;
        script.stderr_bytes = "failed to connect to local tailscaled\nsecond line\n";
        rig.launcher->push(script);
        const auto status = rig.cli().status();
        CHECK(status.state == "unavailable");
        CHECK(status.detail == "failed to connect to local tailscaled");
    }
    SECTION("a failure with nothing on stderr") {
        FakeProcessScript script;
        script.exit_code = 3;
        script.stderr_bytes = "  \r\n";
        rig.launcher->push(script);
        CHECK(rig.cli().status().detail == "Tailscale isn't running");
    }
    SECTION("unreadable output") {
        for (const char* output : {"not json", "[1, 2]", ""}) {
            FakeProcessScript script;
            script.stdout_bytes = output;
            rig.launcher->push(script);
            const auto status = rig.cli().status();
            CHECK(status.state == "unavailable");
            CHECK(status.detail == "Tailscale's answer was unreadable");
        }
    }
    SECTION("too slow") {
        FakeProcessScript script;
        script.exit_code = std::nullopt;
        rig.launcher->push(script);
        const auto status = rig.cli(0.2).status();
        CHECK(status.state == "unavailable");
        CHECK(status.detail == "Tailscale didn't answer in time");
        REQUIRE(rig.launcher->records().size() == 1);
        CHECK(rig.launcher->records()[0]->killed);
    }
    SECTION("couldn't start") {
        FakeProcessScript script;
        script.start_error = "The program wasn't found.";
        rig.launcher->push(script);
        CHECK(rig.cli().status().detail == "Couldn't run Tailscale: The program wasn't found.");
    }
    SECTION("a document parse_status cannot read") {
        FakeProcessScript script;
        script.stdout_bytes = R"({"BackendState": "Running", "Self": {"TailscaleIPs": 5}})";
        rig.launcher->push(script);
        CHECK(rig.cli().status().state == "unavailable");
    }
    SECTION("running") {
        FakeProcessScript script;
        script.stdout_bytes = status_json().dump();
        rig.launcher->push(script);
        const auto status = rig.cli().status();
        CHECK(status.running());
        CHECK(status.owner == kOwner);
        const auto specs = rig.launcher->specs();
        REQUIRE(specs.size() == 1);
        CHECK((specs[0].program == rig.folder / "bin" / "tailscale.COM" ||
               specs[0].program == rig.folder / "bin" / "tailscale.EXE"));
        CHECK(specs[0].args == std::vector<std::string>{"status", "--json"});
        CHECK(specs[0].pipes);
        CHECK(specs[0].stdin_null);
        CHECK(specs[0].own_tree);
        CHECK_FALSE(specs[0].new_console);
    }
}

TEST_CASE("tailscale cli is found where the windows installer puts it", "[test_remote_engine]") {
    // from
    // tests/test_remote_engine.py::test_tailscale_cli_is_found_where_the_windows_installer_puts_it
    TempFolder folder;
    const auto exe = folder / "Tailscale" / "tailscale.exe";
    std::filesystem::create_directories(exe.parent_path());
    std::ofstream(exe).put('\0');
    MapEnvReader env; // nothing on PATH
    env.set("ProgramFiles", path_to_utf8(folder.path()));
    CHECK(find_cli(env, CliPlatform::win32) == exe);

    env.unset("ProgramFiles");
    env.set("ProgramFiles(x86)", path_to_utf8(folder.path()));
    CHECK(find_cli(env, CliPlatform::win32) == exe);
    env.unset("ProgramFiles(x86)");
    CHECK_FALSE(find_cli(env, CliPlatform::win32));
}

TEST_CASE("is tailscale address", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_is_tailscale_address
    const auto [address, expected] = GENERATE(table<std::string, bool>({
        {"100.64.0.2", true},
        {"100.127.255.254", true},
        {"100.128.0.1", false},
        {"192.168.1.20", false},
        {"fd7a:115c:a1e0::9636:1603", true},
        {"::ffff:100.64.0.2", true},
        {"fe80::1%eth0", false},
        {"devbox.local", false},
        {"", false},
    }));
    CHECK(net::is_tailscale_address(address) == expected);
}

TEST_CASE("own device needs tailnet addresses the same owner and no tags", "[test_remote_engine]") {
    // from
    // tests/test_remote_engine.py::test_own_device_needs_tailnet_addresses_the_same_owner_and_no_tags
    testing::FakeTailscaleCli cli;
    cli.set_identity("100.64.0.2", PeerIdentity{"devbox", std::string(kOwner), false});
    cli.set_identity("100.64.0.3", PeerIdentity{"friends-pc", "friend@example.com", false});
    cli.set_identity("100.64.0.4", PeerIdentity{"gpu", "", true});
    const std::string local = "100.64.0.1";
    const auto devbox = own_device("100.64.0.2", local, kOwner, cli);
    REQUIRE(devbox);
    CHECK(devbox->node == "devbox");
    CHECK_FALSE(own_device("100.64.0.3", local, kOwner, cli));
    CHECK_FALSE(own_device("100.64.0.4", local, kOwner, cli));
    CHECK_FALSE(own_device("100.64.0.2", local, "", cli));
    // Arriving on a LAN interface doesn't count, whatever the source claims.
    CHECK_FALSE(own_device("100.64.0.2", "192.168.1.5", kOwner, cli));
    CHECK_FALSE(own_device("192.168.1.9", local, kOwner, cli));
    CHECK(cli.looked_up() == std::vector<std::string>{"100.64.0.2", "100.64.0.3", "100.64.0.4"});
}

// ---- the rest of the CLI ----

TEST_CASE("which looks along PATH with PATHEXT, each folder once", "[tailscale_cli]") {
    TempFolder folder;
    for (const char* dir : {"a", "b", "c"}) {
        std::filesystem::create_directories(folder / dir);
    }
    std::ofstream(folder / "b" / "tailscale.CMD").put('\0');
    std::ofstream(folder / "c" / "tailscale.EXE").put('\0');
    std::filesystem::create_directories(folder / "a" / "tailscale.EXE"); // a folder is no CLI
    MapEnvReader env;
    env.set("PATH", path_to_utf8(folder / "a") + ";;" + path_to_utf8(folder / "c") + ";" +
                        path_to_utf8(folder / "b"));
    env.set("PATHEXT", ".EXE;.CMD");
    CHECK(find_cli(env, CliPlatform::win32) == folder / "c" / "tailscale.EXE");
    env.set("PATHEXT", ".CMD;.EXE");
    CHECK(find_cli(env, CliPlatform::win32) == folder / "c" / "tailscale.EXE");
    env.set("PATH", path_to_utf8(folder / "b"));
    CHECK(find_cli(env, CliPlatform::win32) == folder / "b" / "tailscale.CMD");
    env.unset("PATHEXT"); // the default list has .CMD
    CHECK(find_cli(env, CliPlatform::win32) == folder / "b" / "tailscale.CMD");
}

TEST_CASE("PATHEXT entries lose their trailing dots as shutil.which strips them",
          "[tailscale_cli]") {
    TempFolder folder;
    std::ofstream(folder / "tailscale.EXE").put('\0');
    MapEnvReader env;
    env.set("PATH", path_to_utf8(folder.path()));
    env.set("PATHEXT", ".COM.;.EXE..");
    CHECK(find_cli(env, CliPlatform::win32) == folder / "tailscale.EXE");
}

TEST_CASE("only macOS runs the CLI with TAILSCALE_BE_CLI", "[tailscale_cli]") {
    const auto darwin = cli_extra_env(CliPlatform::darwin);
    CHECK(darwin == std::map<std::string, std::string>{{"TAILSCALE_BE_CLI", "1"}});
    CHECK(cli_extra_env(CliPlatform::win32).empty());
    CHECK(cli_extra_env(CliPlatform::other).empty());
    FakeCliRig rig;
    rig.launcher->push(answering(status_json()));
    (void)rig.cli().status();
    CHECK_FALSE(rig.launcher->specs().at(0).env.contains("TAILSCALE_BE_CLI"));
}

TEST_CASE("whois asks only about Tailscale addresses", "[tailscale_cli]") {
    FakeCliRig rig;
    auto cli = rig.cli();
    CHECK_FALSE(cli.whois("192.168.1.20"));
    CHECK(rig.launcher->specs().empty());
    rig.launcher->push(answering(Json::parse(R"({"Node": {"ComputedName": "devbox"},
                                                 "UserProfile": {"LoginName": "o@x"}})")));
    const auto identity = cli.whois("100.64.0.2");
    REQUIRE(identity);
    CHECK(identity->node == "devbox");
    CHECK(rig.launcher->specs().at(0).args ==
          std::vector<std::string>{"whois", "--json", "100.64.0.2"});
}

TEST_CASE("the CLI runs with the user's environment", "[tailscale_cli]") {
    FakeCliRig rig;
    rig.launcher->push(answering(status_json()));
    (void)rig.cli().status();
    // child_env(): never empty, so the child gets the user's environment rather than this
    // process's own copy of it.
    CHECK_FALSE(rig.launcher->specs().at(0).env.empty());
}

// ---- golden vectors ----

TEST_CASE("whois matches Python", "[golden]") {
    golden::for_each(kModule, "whois", [](const golden::Case& c) {
        FakeCliRig rig;
        rig.launcher->push(answering(c.arg<Json>(0)));
        const Json actual = identity_record(rig.cli().whois(c.arg<std::string>(1)));
        INFO(actual.dump());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("own_device matches Python", "[golden]") {
    golden::for_each(kModule, "own_device", [](const golden::Case& c) {
        FakeCliRig rig;
        rig.launcher->push(answering(c.arg<Json>(3)));
        auto cli = rig.cli();
        const Json actual = identity_record(
            own_device(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<std::string>(2), cli));
        INFO(actual.dump());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

// ---- a real child ----

TEST_CASE("the real launcher runs the CLI and reads its answer", "[platform]") {
#ifndef _WIN32
    SKIP("the stand-in CLI is a Windows executable");
#else
    // FV_FAKE_TAILSCALE is a small program that answers as its mode file says; it is copied into
    // this test's scratch folder in the build tree as tailscale.exe.
    const std::filesystem::path scratch = std::filesystem::path(FV_REMOTE_SCRATCH) / "real-cli";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::filesystem::copy_file(FV_FAKE_TAILSCALE, scratch / "tailscale.exe");
    MapEnvReader env;
    env.set("PATH", path_to_utf8(scratch));
    env.set("PATHEXT", ".EXE");
    ProcessTailscaleCli cli(env, nullptr, 10.0, CliPlatform::win32);

    std::ofstream(scratch / "mode.txt") << "status";
    const auto status = cli.status();
    CHECK(status.running());
    CHECK(status.owner == kOwner);
    std::ifstream args_file(scratch / "args.txt");
    std::string args((std::istreambuf_iterator<char>(args_file)), {});
    args_file.close();
    CHECK(args == "status\n--json\n");

    std::ofstream(scratch / "mode.txt", std::ios::trunc) << "exit1";
    const auto failed = cli.status();
    CHECK(failed.state == "unavailable");
    CHECK(failed.detail == "failed to connect to local tailscaled");

    std::ofstream(scratch / "mode.txt", std::ios::trunc) << "whois";
    const auto identity = cli.whois("100.64.0.2");
    REQUIRE(identity);
    CHECK(identity->node == "devbox");
    CHECK(identity->owner == kOwner);
    std::filesystem::remove_all(scratch);
#endif
}
