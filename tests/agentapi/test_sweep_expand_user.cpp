// Sweep A32: HistoryStore expands "--database '~...'" as Path(database).expanduser() does
// (services/agent_api/store.py:148), with base's home rules: on Windows USERPROFILE, else
// HOMEDRIVE + HOMEPATH; elsewhere HOME with its trailing separators dropped, and "~\" left alone.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

#include "agentapi/history_store.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "support.h"
#include "testkit/testkit.h"

using fv::MapEnvReader;
using fv::agentapi::HistoryStore;
using fv::agentapi::test::make_test_database;

namespace {

bool opens(const std::filesystem::path& database, const MapEnvReader& env) {
    try {
        HistoryStore store(database, env);
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

} // namespace

TEST_CASE("history store expands a home-relative database path", "[sweep][history_store]") {
    fv::testkit::TempDataRoot root;
    const auto database = make_test_database(root.path());
    const std::string home = fv::path_to_utf8(root.path());
    const auto name = fv::path_to_utf8(database.filename());
#ifdef _WIN32
    SECTION("USERPROFILE") {
        const MapEnvReader env{{"USERPROFILE", home}};
        HistoryStore store(fv::path_from_utf8("~/" + name), env);
        CHECK(std::filesystem::equivalent(store.database(), database));
        CHECK(opens(fv::path_from_utf8("~\\" + name), env));
    }
    SECTION("HOMEDRIVE and HOMEPATH without USERPROFILE") {
        const MapEnvReader env{{"HOMEDRIVE", home.substr(0, 2)}, {"HOMEPATH", home.substr(2)}};
        HistoryStore store(fv::path_from_utf8("~\\" + name), env);
        CHECK(std::filesystem::equivalent(store.database(), database));
    }
#else
    SECTION("HOME, trailing separators and all") {
        const MapEnvReader env{{"HOME", home + "//"}};
        HistoryStore store(fv::path_from_utf8("~/" + name), env);
        CHECK(fv::path_to_utf8(store.database()).find("//") == std::string::npos);
        CHECK(std::filesystem::equivalent(store.database(), database));
    }
    SECTION("a backslash is not a separator here") {
        // HOME is <root>/home; a database named "home\<name>" beside it is what "~\<name>"
        // would reach if "~" were expanded before a backslash.
        const auto folder = root.path() / "home";
        std::filesystem::create_directories(folder);
        std::filesystem::copy_file(database, root.path() / fv::path_from_utf8("home\\" + name));
        const MapEnvReader env{{"HOME", fv::path_to_utf8(folder)}};
        CHECK_FALSE(opens(fv::path_from_utf8("~\\" + name), env));
    }
#endif
    SECTION("no home at all leaves the path as given") {
        const MapEnvReader env;
        CHECK_FALSE(opens(fv::path_from_utf8("~/" + name), env));
    }
}
