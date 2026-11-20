// remote/pairing_settings.h against services/remote_asr/settings.py: the ported tests of
// tests/test_remote_pairing_ux.py, test_remote_token_scope.py, test_remote_rename.py and
// test_remote_engine.py, then the golden vectors (tests/golden/services.remote_asr.settings.json).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "remote/pairing_settings.h"
#include "remote_test_support.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/store.h"

using namespace fv;
using namespace fv::remote;
using fv::remote::test::TempFolder;

namespace {

constexpr std::string_view kModule = "services.remote_asr.settings";
constexpr std::string_view kKey = "remote_engine_client";

std::string fingerprint(std::string_view pair) {
    std::string fp;
    for (int i = 0; i < 32; ++i) {
        fp += pair;
    }
    return fp;
}

// The variable that gives the app a data directory of its own (a checkout's).
std::string data_dir_variable() {
    return std::string(identity().env_prefix) + "DATA_DIR";
}

// Every variable Paths::resolve reads for the per-user directory, on each platform, pointed into
// `folder`, so that no test reaches the real one (LOCALAPPDATA on Windows, XDG_DATA_HOME on
// Linux, the home folder on macOS and as the fallback).
void isolate_per_user_dir(MapEnvReader& env, const TempFolder& folder) {
    env.set("LOCALAPPDATA", path_to_utf8(folder / "local"));
    env.set("XDG_DATA_HOME", path_to_utf8(folder / "local"));
    env.set("HOME", path_to_utf8(folder / "home"));
    env.set("USERPROFILE", path_to_utf8(folder / "home"));
}

// A data root, its settings file and credential store, as OpenWhisper's tests get them from
// conftest: by default the data root is the per-user directory (the installed app).
struct Rig {
    explicit Rig(bool checkout = false) {
        isolate_per_user_dir(env, folder);
        if (checkout) {
            env.set(data_dir_variable(), path_to_utf8(folder / "checkout"));
        }
        paths = std::make_unique<Paths>(Paths::resolve(identity(), env));
        settings = std::make_unique<SettingsStore>(paths->user_file(identity().settings_file));
    }

    std::optional<std::string> token() {
        return load_client_token(credentials, env, dotenv, *paths, identity());
    }
    ClientPairing save(std::string_view host, int port, const PairingResult& result) {
        return save_client_pairing(*settings, credentials, *paths, identity(), host, port, result,
                                   clock);
    }

    TempFolder folder;
    MapEnvReader env;
    std::unique_ptr<Paths> paths;
    std::unique_ptr<SettingsStore> settings;
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore credentials{backend, identity()};
    DotEnv dotenv{folder / "missing.env"};
    FakeClock clock;
};

PairingResult result(std::string token, std::string host_name, std::vector<std::string> alternates,
                     std::string via) {
    return PairingResult{std::move(token),      "d",
                         std::move(host_name),  fingerprint("AB"),
                         std::move(alternates), std::move(via)};
}

// _saved_pairing of tests/test_remote_pairing_ux.py.
ClientPairing saved_pairing(Rig& rig, std::string host = "192.168.1.20", int port = 47821,
                            std::vector<std::string> alternates = {"100.64.0.2"}) {
    return rig.save(host, port, result("t", "devbox", std::move(alternates), "approval"));
}

Json pairing_record(const std::optional<ClientPairing>& pairing) {
    if (!pairing) {
        return nullptr;
    }
    return {{"host", pairing->host},
            {"port", pairing->port},
            {"fingerprint", pairing->fingerprint},
            {"host_name", pairing->host_name},
            {"device_id", pairing->device_id},
            {"paired_at", pairing->paired_at},
            {"alternates", pairing->alternates},
            {"via", pairing->via},
            {"paired_name", pairing->paired_name},
            {"renamed", pairing->renamed()},
            {"address", pairing->address()},
            {"over_tailscale", pairing->over_tailscale()},
            {"tailscale_fallback", pairing->tailscale_fallback()}};
}

Json settings_with(const Json& saved) {
    return Json{{std::string(kKey), saved}};
}

} // namespace

// ---- tests/test_remote_pairing_ux.py: following a host that moved ----

TEST_CASE("a new address from the router replaces the old one", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_a_new_address_from_the_router_replaces_the_old_one
    Rig rig;
    saved_pairing(rig);
    const std::vector<std::string> listed = {"192.168.1.37", "100.64.0.2"};
    const auto moved =
        remember_host_addresses(*rig.settings, fingerprint("ab"), "192.168.1.37", 47821, listed);
    REQUIRE(moved);
    CHECK(moved->host == "192.168.1.37");
    CHECK(moved->alternates == std::vector<std::string>{"100.64.0.2"});
    const auto loaded = load_client_pairing(*rig.settings);
    REQUIRE(loaded);
    CHECK(loaded->host == "192.168.1.37");
    CHECK(loaded->via == "approval");
    // Nothing new: nothing written.
    CHECK_FALSE(
        remember_host_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37", 47821, listed));
}

TEST_CASE("away from home the home address stays first", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_away_from_home_the_home_address_stays_first
    Rig rig;
    saved_pairing(rig);
    const std::vector<std::string> listed = {"192.168.1.20", "100.64.0.2"};
    CHECK_FALSE(
        remember_host_addresses(*rig.settings, fingerprint("AB"), "100.64.0.2", 47821, listed));
    CHECK(load_client_pairing(*rig.settings)->host == "192.168.1.20");
}

TEST_CASE("a named host keeps its name and a tailnet address isnt dropped",
          "[test_remote_pairing_ux]") {
    // from
    // tests/test_remote_pairing_ux.py::test_a_named_host_keeps_its_name_and_a_tailnet_address_isnt_dropped
    Rig rig;
    saved_pairing(rig, "devbox.local", 47821, {"192.168.1.20", "100.64.0.2"});
    const std::vector<std::string> listed = {"192.168.1.37"}; // Tailscale is off there for now
    const auto moved =
        remember_host_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37", 47821, listed);
    REQUIRE(moved);
    CHECK(moved->host == "devbox.local");
    CHECK(moved->alternates == std::vector<std::string>{"192.168.1.37", "100.64.0.2"});
}

TEST_CASE("another hosts addresses are never saved", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_another_hosts_addresses_are_never_saved
    Rig rig;
    saved_pairing(rig);
    const std::vector<std::string> listed = {"10.0.0.1"};
    CHECK_FALSE(
        remember_host_addresses(*rig.settings, fingerprint("CD"), "10.0.0.1", 47821, listed));
    CHECK(load_client_pairing(*rig.settings)->host == "192.168.1.20");
}

TEST_CASE("a pairing is the same one after its address moves", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_a_pairing_is_the_same_one_after_its_address_moves
    Rig rig;
    const ClientPairing pairing = saved_pairing(rig);
    const std::vector<std::string> listed = {"192.168.1.37"};
    const auto moved =
        remember_host_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37", 47821, listed);
    REQUIRE(moved);
    CHECK(*moved == pairing);
    CHECK(moved->host != pairing.host);
}

// ---- tests/test_remote_token_scope.py ----

TEST_CASE("the per user directory keeps the original name", "[test_remote_token_scope]") {
    // from tests/test_remote_token_scope.py::test_the_per_user_directory_keeps_the_original_name
    Rig rig;
    CHECK(token_credential(*rig.paths, identity()) == unscoped_token_credential(identity()));
    CHECK(unscoped_token_credential(identity()) ==
          std::string(identity().env_prefix) + "REMOTE_ENGINE_TOKEN");
}

TEST_CASE("each other data directory gets its own name", "[test_remote_token_scope]") {
    // from tests/test_remote_token_scope.py::test_each_other_data_directory_gets_its_own_name
    TempFolder folder;
    MapEnvReader env;
    isolate_per_user_dir(env, folder);
    std::set<std::string> names;
    for (const char* checkout : {"one", "two"}) {
        env.set(data_dir_variable(), path_to_utf8(folder / checkout));
        names.insert(token_credential(Paths::resolve(identity(), env), identity()));
    }
    CHECK(names.size() == 2);
    CHECK_FALSE(names.contains(unscoped_token_credential(identity())));
}

TEST_CASE("pairing a checkout leaves the installed apps token alone", "[test_remote_token_scope]") {
    // from
    // tests/test_remote_token_scope.py::test_pairing_a_checkout_leaves_the_installed_apps_token_alone
    Rig rig(true);
    const std::string unscoped = unscoped_token_credential(identity());
    rig.credentials.set(unscoped, "installed-token");
    rig.save("100.64.0.2", 47821, result("checkout-token", "jed", {}, "tailscale"));
    CHECK(rig.credentials.get(unscoped) == "installed-token");
    CHECK(rig.token() == "checkout-token");

    forget_client_pairing(*rig.settings, rig.credentials, *rig.paths, identity());
    CHECK(rig.credentials.get(unscoped) == "installed-token");
    CHECK_FALSE(rig.credentials.get(token_credential(*rig.paths, identity())));
    CHECK_FALSE(load_client_pairing(*rig.settings));
}

TEST_CASE("a checkout paired before scoping still reads the shared token",
          "[test_remote_token_scope]") {
    // from
    // tests/test_remote_token_scope.py::test_a_checkout_paired_before_scoping_still_reads_the_shared_token
    Rig rig(true);
    rig.credentials.set(unscoped_token_credential(identity()), "older-token");
    CHECK(rig.token() == "older-token");
}

// ---- tests/test_remote_rename.py ----

TEST_CASE("the host can go by another name here and back", "[test_remote_rename]") {
    // from tests/test_remote_rename.py::test_the_host_can_go_by_another_name_here_and_back
    Rig rig;
    const ClientPairing pairing =
        rig.save("192.168.1.20", 47821, result("t", "DESKTOP-4K2J9", {}, "approval"));
    const auto renamed = rename_client_host(*rig.settings, " Studio PC ");
    REQUIRE(renamed);
    CHECK(renamed->host_name == "Studio PC");
    CHECK(renamed->paired_name == "DESKTOP-4K2J9");
    const auto loaded = load_client_pairing(*rig.settings);
    REQUIRE(loaded);
    CHECK(loaded->host_name == "Studio PC");
    CHECK(loaded->renamed());
    // Still the pairing a window opened before the rename holds.
    CHECK(*loaded == pairing);
    const std::vector<std::string> listed = {"192.168.1.37"};
    const auto moved =
        remember_host_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37", 47821, listed);
    REQUIRE(moved);
    CHECK(moved->host_name == "Studio PC");
    CHECK(moved->paired_name == "DESKTOP-4K2J9");
    const auto back = rename_client_host(*rig.settings, "");
    REQUIRE(back);
    CHECK(back->host_name == "DESKTOP-4K2J9");
    CHECK_FALSE(back->renamed());
    const Json saved = rig.settings->value(kKey);
    CHECK(saved["host_name"] == "DESKTOP-4K2J9");
    CHECK_FALSE(saved.contains("paired_name"));
}

TEST_CASE("renaming the host with nothing paired does nothing", "[test_remote_rename]") {
    // from tests/test_remote_rename.py::test_renaming_the_host_with_nothing_paired_does_nothing
    Rig rig;
    CHECK_FALSE(rename_client_host(*rig.settings, "Studio PC"));
    CHECK_FALSE(load_client_pairing(*rig.settings));
}

TEST_CASE("only a host that isnt renamed goes by the name it reports", "[test_remote_rename]") {
    // from
    // tests/test_remote_rename.py::test_only_a_host_that_isnt_renamed_goes_by_the_name_it_reports
    ClientPairing pairing;
    pairing.host = "10.0.0.2";
    pairing.port = 47821;
    pairing.fingerprint = fingerprint("AB");
    pairing.host_name = "DESKTOP-4K2J9";
    CHECK(pairing.name_for("NEW-NAME") == "NEW-NAME");
    CHECK(pairing.name_for("") == "DESKTOP-4K2J9");
    ClientPairing renamed = pairing;
    renamed.host_name = "Studio PC";
    renamed.paired_name = "DESKTOP-4K2J9";
    CHECK(renamed.name_for("NEW-NAME") == "Studio PC");
}

// ---- tests/test_remote_engine.py ----

TEST_CASE("remote is a valid saved engine", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_remote_is_a_valid_saved_engine (the settings half;
    // the "Remote computer" display name is the backend table's)
    Rig rig;
    save_model_selection(*rig.settings, "remote");
    CHECK(load_model_selection(*rig.settings) == "remote");
}

// ---- what the store and the clock add ----

TEST_CASE("a fresh pairing saves the token first and the pairing as Python writes it",
          "[pairing_settings]") {
    Rig rig;
    rig.clock.advance(std::chrono::milliseconds(3'723'456)); // 01:02:03.456 on the first day
    const ClientPairing pairing =
        rig.save("192.168.1.20", 47821,
                 result("tok", "devbox", {"192.168.1.20", "100.64.0.2", "100.64.0.2"}, "code"));
    CHECK(pairing.paired_at == "2026-01-01T01:02:03+00:00");
    CHECK(pairing.alternates == std::vector<std::string>{"100.64.0.2", "100.64.0.2"});
    CHECK(rig.token() == "tok");
    const Json saved = rig.settings->value(kKey);
    CHECK(
        saved.dump() ==
        Json::parse(
            R"({"host":"192.168.1.20","port":47821,"fingerprint":")" + fingerprint("AB") +
            R"(","host_name":"devbox","device_id":"d","paired_at":"2026-01-01T01:02:03+00:00","alternates":["100.64.0.2","100.64.0.2"],"via":"code"})")
            .dump());
}

TEST_CASE("a token the store refuses leaves the settings untouched", "[pairing_settings]") {
    Rig rig;
    // A key with a space is not a key the store saves (validate_api_key).
    CHECK_THROWS_AS(rig.save("192.168.1.20", 47821, result("not a token", "devbox", {}, "code")),
                    std::invalid_argument);
    CHECK_FALSE(load_client_pairing(*rig.settings));
}

TEST_CASE("the per user directory of every test is in its temporary folder", "[pairing_settings]") {
    const auto inside = [](const Rig& rig, const std::filesystem::path& path) {
        return normalized_path(path).starts_with(normalized_path(rig.folder.path()));
    };
    Rig rig;
    CHECK(inside(rig, rig.paths->local_app_dir()));
    CHECK(inside(rig, rig.paths->data_root()));
    Rig checkout(true);
    CHECK(inside(checkout, checkout.paths->local_app_dir()));
    CHECK(inside(checkout, checkout.paths->data_root()));
}

TEST_CASE("addresses a host lists are read as Python iterates them", "[pairing_settings]") {
    Rig rig;
    saved_pairing(rig);
    // A dict: its keys.
    const auto moved = remember_ready_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37",
                                                47821, Json{{"192.168.1.37", 1}, {"", 2}});
    REQUIRE(moved);
    CHECK(moved->host == "192.168.1.37");
    CHECK(moved->alternates == std::vector<std::string>{"100.64.0.2"});
    // A string: its characters, none of them an address the host is at.
    const auto chars = remember_ready_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37",
                                                47821, Json("ab"));
    REQUIRE(chars);
    CHECK(chars->alternates == std::vector<std::string>{"a", "b", "100.64.0.2"});
    CHECK_THROWS_AS(
        remember_ready_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37", 47821, Json(5)),
        py::TypeError);
    CHECK_THROWS_AS(remember_ready_addresses(*rig.settings, fingerprint("AB"), "192.168.1.37",
                                             47821, Json(nullptr)),
                    py::TypeError);
}

TEST_CASE("the host's own settings read their defaults", "[pairing_settings]") {
    Rig rig;
    const Json empty = Json::object();
    CHECK_FALSE(host_enabled(empty));
    CHECK_FALSE(host_model_management(empty));
    CHECK_FALSE(host_keeps_records(empty));
    CHECK_FALSE(host_manages_mcp(empty));
    CHECK_FALSE(client_shares_history(empty));
    CHECK(records_location(empty) == "local");
    CHECK(host_port(empty) == 47821);
    CHECK(host_tailscale_trust(empty));
    CHECK(host_port(Json{{"remote_host_port", 0}}) == 47821);
    CHECK(records_location(Json{{"remote_records_location", "both"}}) == "both");

    CHECK((load_host_devices(*rig.settings) == Json::array()));
    const Json devices = Json::array({{{"id", "d1"}, {"name", "laptop"}}});
    save_host_devices(*rig.settings, devices);
    CHECK((load_host_devices(*rig.settings) == devices));
}

// ---- golden vectors ----

TEST_CASE("settings constants match Python", "[golden]") {
    golden::for_each(kModule, "constants", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(golden::equal_rebranded(expected["TOKEN_CREDENTIAL"].get<std::string>(),
                                      unscoped_token_credential(identity())));
        CHECK((expected["PAIRING_ROUTES"] == Json(PAIRING_ROUTES)));
        CHECK((expected["RECORD_LOCATIONS"] == Json(RECORD_LOCATIONS)));
    });
}

TEST_CASE("load_client_pairing matches Python", "[golden]") {
    golden::for_each(kModule, "load_client_pairing", [](const golden::Case& c) {
        const Json actual = pairing_record(load_client_pairing(settings_with(c.arg<Json>(0))));
        INFO(actual.dump());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(kModule, "name_for", [](const golden::Case& c) {
        const auto pairing = load_client_pairing(settings_with(c.arg<Json>(0)));
        REQUIRE(pairing);
        const Json reported = c.arg<Json>(1);
        CHECK(pairing->name_for(reported.is_string() ? reported.get<std::string>() : "") ==
              c.result<std::string>());
    });
}

TEST_CASE("remember_host_addresses and rename_client_host match Python", "[golden]") {
    const auto replay = [](const golden::Case& c, const Json& saved, auto&& call) {
        Rig rig;
        if (!saved.is_null()) {
            rig.settings->save_setting(kKey, saved);
        }
        const Json actual_result = pairing_record(call(*rig.settings));
        const Json expected = c.result<Json>();
        INFO(actual_result.dump());
        CHECK(golden::equal(expected["result"], actual_result));
        if (expected["writes"] == 0) {
            CHECK((rig.settings->value(kKey) == saved));
        } else {
            CHECK((rig.settings->value(kKey) == expected["written"]));
            CHECK(rig.settings->value(kKey).dump() == expected["written"].dump());
        }
    };
    golden::for_each(kModule, "remember_host_addresses", [&](const golden::Case& c) {
        replay(c, c.arg<Json>(0), [&](SettingsStore& settings) {
            return remember_ready_addresses(settings, c.arg<std::string>(1), c.arg<std::string>(2),
                                            c.arg<int>(3), c.arg<Json>(4));
        });
    });
    golden::for_each(kModule, "rename_client_host", [&](const golden::Case& c) {
        replay(c, c.arg<Json>(0), [&](SettingsStore& settings) {
            const Json name = c.arg<Json>(1);
            return rename_client_host(settings, name.is_string() ? name.get<std::string>() : "");
        });
    });
}

TEST_CASE("token_credential matches Python", "[golden]") {
#ifndef _WIN32
    SKIP("the vectors hold Windows paths, which normcase lowercases there only");
#endif
    golden::for_each(kModule, "token_credential", [](const golden::Case& c) {
        const std::string actual =
            token_credential(path_from_utf8(c.arg<std::string>(0)),
                             path_from_utf8(c.arg<std::string>(1)), identity());
        CHECK(golden::equal_rebranded(c.result<std::string>(), actual));
    });
}

TEST_CASE("remote model management requires an explicit boolean true", "[pairing_settings]") {
    CHECK_FALSE(host_model_management(Json::object()));
    for (const Json& value : {Json(nullptr), Json(false), Json(0), Json(1), Json("true"),
                              Json::array(), Json::object()}) {
        INFO(value.dump());
        CHECK_FALSE(host_model_management(Json{{"remote_host_model_management", value}}));
    }
    CHECK(host_model_management(Json{{"remote_host_model_management", true}}));
}
