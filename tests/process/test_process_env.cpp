// The user's environment for children: OpenWhisper's tests/test_process_env.py and
// tests/test_automation_r3c_child_env.py, and the golden vectors of services.process_env.
// OpenWhisper's tests change os.environ; here each test runs ProcessEnv over its own MapEnv,
// with the rules of the platform it runs on.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/json.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/resolve.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "golden/golden.h"
#include "process/process_env.h"
#include "process/testing/map_env.h"
#include "process_test_support.h"
#include "pyport/text.h"
#include "testkit/testkit.h"

using fv::EnvMap;
using fv::MapEnv;
using fv::ProcessEnv;

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

// Whether a variable of that name is in `env`, matching names as the system does.
bool has_name(const EnvMap& env, std::string_view name) {
    const auto system_name = [](std::string_view text) {
        return ProcessEnv::kWindows ? fv::py::upper(text) : std::string(text);
    };
    return std::any_of(env.begin(), env.end(), [&](const auto& item) {
        return system_name(item.first) == system_name(name);
    });
}

std::string prefixed(std::string_view suffix) {
    return std::string(fv::identity().env_prefix) + std::string(suffix);
}

// tests/test_automation_r3c_child_env.py load_dotenv: `values` in the app's .env, with the
// process environment holding them as an editor that loaded the same file would; the app adopts
// them at startup.
struct DotEnvFixture {
    fv::testkit::TempDataRoot root;
    std::filesystem::path file = root.path() / ".env";
    fv::MapEnvReader interpolation;
    MapEnv env;
    ProcessEnv process{env};

    void load(const Pairs& values, bool also_in_environment = true) {
        std::string text;
        for (const auto& [name, value] : values) {
            text += name + "=" + value + "\n";
            if (also_in_environment) {
                env.set(name, value);
            }
        }
        fv::process_test::write_file(file, text);
        process.adopt_dotenv(fv::DotEnv(file, interpolation));
    }
    // tests/test_automation_r3c_child_env.py rewrite: a change the parse cache sees.
    void rewrite(std::string_view text) {
        fv::process_test::write_file(file, text);
        std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) +
                                                   std::chrono::milliseconds(1));
        process.adopt_dotenv(fv::DotEnv(file, interpolation));
    }
};

Pairs sorted_pairs(const EnvMap& env) {
    Pairs out(env.begin(), env.end());
    std::sort(out.begin(), out.end());
    return out;
}

Pairs sorted_pairs(Pairs items) {
    std::sort(items.begin(), items.end());
    return items;
}

Pairs pairs_from(const fv::Json& json, bool rebrand) {
    Pairs out;
    if (json.is_null()) {
        return out;
    }
    for (const auto& item : json) {
        auto name = item.at(0).get<std::string>();
        auto value = item.at(1).get<std::string>();
        out.emplace_back(rebrand ? fv::golden::rebrand(name) : name,
                         rebrand ? fv::golden::rebrand(value) : value);
    }
    return out;
}

// Replays one vector: the steps against a MapEnv with Windows' rules, then external_env or
// child_env, compared with OpenWhisper's result after the rebrand of names.
void replay(const fv::golden::Case& c, bool child) {
    MapEnv env(true);
    for (const auto& [name, value] : pairs_from(c.args()[0], false)) {
        // Upper-cased first, as the mapping stores it, so "Openwhisper_X" is rebranded too.
        env.set(child ? fv::golden::rebrand(fv::py::upper(name)) : name,
                child ? fv::golden::rebrand(value) : value);
    }
    ProcessEnv process(env, true);
    fv::Json refused = fv::Json::array();
    for (const auto& step : c.args()[1]) {
        const auto op = step.at(0).get<std::string>();
        if (op == "set") {
            const auto name = step.at(1).get<std::string>();
            try {
                process.set_for_app(name, step.at(2).get<std::string>(), step.at(3).get<bool>());
            } catch (const std::invalid_argument& error) {
                refused.push_back(fv::Json::array({name, error.what()}));
            }
        } else if (op == "load") {
            const auto values = pairs_from(step.at(1), child);
            for (const auto& name : process.load_for_app(values)) {
                refused.push_back(name);
            }
        } else if (op == "prepend") {
            process.prepend_for_app(step.at(1).get<std::string>(),
                                    step.at(2).get<std::vector<std::string>>());
        }
    }
    std::map<std::string, std::string> extra;
    for (const auto& [name, value] : pairs_from(c.args()[2], child)) {
        extra[name] = value;
    }
    const auto result = child ? fv::child_env(process, extra) : process.external_env(extra);
    const auto expected = c.result<fv::Json>();
    CHECK(sorted_pairs(result) == sorted_pairs(pairs_from(expected.at("env"), child)));
    CHECK(sorted_pairs(env.items()) == sorted_pairs(pairs_from(expected.at("environ"), child)));
    CHECK(fv::golden::equal(expected.at("refused"), refused));
}

} // namespace

TEST_CASE("external_env replays OpenWhisper's vectors", "[process][golden]") {
    fv::golden::for_each("services.process_env", "external_env",
                         [](const fv::golden::Case& c) { replay(c, false); });
}

TEST_CASE("child_env replays OpenWhisper's agent_child_env vectors", "[process][golden]") {
    fv::golden::for_each("services.process_env", "child_env",
                         [](const fv::golden::Case& c) { replay(c, true); });
}

// -- tests/test_process_env.py ---------------------------------------------------------------

// from tests/test_process_env.py::test_variables_the_app_set_are_restored
TEST_CASE("variables the app set are restored", "[test_process_env]") {
    MapEnv env(ProcessEnv::kWindows, {{"QT_WAYLAND_DISABLE_WINDOWDECORATION", "0"}});
    ProcessEnv process(env);
    process.set_for_app("QT_QPA_PLATFORMTHEME", "");
    process.set_for_app("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");
    CHECK(env.get("QT_QPA_PLATFORMTHEME") == "");

    const auto result = process.external_env();
    CHECK_FALSE(result.contains("QT_QPA_PLATFORMTHEME"));
    CHECK(result.at("QT_WAYLAND_DISABLE_WINDOWDECORATION") == "0");
}

// from tests/test_process_env.py::test_a_user_value_is_kept_when_asked
TEST_CASE("a user value is kept when asked", "[test_process_env]") {
    MapEnv env(ProcessEnv::kWindows, {{"QT_WAYLAND_USE_DATA_CONTROL", "0"}});
    ProcessEnv process(env);
    process.set_for_app("QT_WAYLAND_USE_DATA_CONTROL", "1", true);
    CHECK(env.get("QT_WAYLAND_USE_DATA_CONTROL") == "0");
    CHECK(process.external_env().at("QT_WAYLAND_USE_DATA_CONTROL") == "0");
}

// from tests/test_process_env.py::test_path_entries_the_app_prepended_are_removed
TEST_CASE("path entries the app prepended are removed", "[test_process_env]") {
    fv::testkit::TempDataRoot root;
    const char separator = ProcessEnv::kWindows ? ';' : ':';
    const auto at = [&](const char* name) {
        return fv::path_to_utf8(root.path() / name);
    };
    const std::string user = at("user-bin") + separator + at("more");
    MapEnv env(ProcessEnv::kWindows, {{"PATH", user}});
    ProcessEnv process(env);
    const std::vector<std::string> first{at("cuda"), at("icu")};
    const std::vector<std::string> late{at("late")};
    process.prepend_for_app("PATH", first);
    process.prepend_for_app("PATH", late);
    CHECK(env.get("PATH")->starts_with(at("late")));
    CHECK(process.external_env().at("PATH") == user);
}

// from tests/test_process_env.py::test_agents_hooks_and_actions_start_from_the_users_environment
TEST_CASE("agents hooks and actions start from the user's environment", "[test_process_env]") {
    MapEnv env(ProcessEnv::kWindows);
    ProcessEnv process(env);
    process.set_for_app("QT_QPA_PLATFORMTHEME", "");
    env.set(prefixed("LLM_API_KEY"), "internal");
    const auto result = fv::child_env(process, {{prefixed("EVENT_TYPE"), "x"}});
    CHECK_FALSE(result.contains("QT_QPA_PLATFORMTHEME"));
    CHECK_FALSE(result.contains(prefixed("LLM_API_KEY")));
    CHECK(result.at(prefixed("EVENT_TYPE")) == "x");
}

// -- tests/test_automation_r3c_child_env.py --------------------------------------------------

// from tests/test_automation_r3c_child_env.py::test_a_key_rotated_in_dotenv_never_reaches_a_child
TEST_CASE("a key rotated in dotenv never reaches a child", "[test_automation_r3c_child_env]") {
    DotEnvFixture f;
    f.load({{"ANTHROPIC_API_KEY", "sk-old-from-dotenv"}});
    CHECK(f.env.get("ANTHROPIC_API_KEY") == "sk-old-from-dotenv");

    // The user changes the key in .env while the app runs; the old value stays in this
    // process's environment.
    f.rewrite("ANTHROPIC_API_KEY=sk-new-from-dotenv-rotated\n");
    CHECK_FALSE(has_name(fv::child_env(f.process), "ANTHROPIC_API_KEY"));
}

// from tests/test_automation_r3c_child_env.py::test_a_key_deleted_from_dotenv_never_reaches_a_child
TEST_CASE("a key deleted from dotenv never reaches a child", "[test_automation_r3c_child_env]") {
    DotEnvFixture f;
    f.load({{"OPENAI_API_KEY", "sk-from-dotenv"}});
    f.rewrite("# no keys any more\n");
    CHECK_FALSE(has_name(fv::child_env(f.process), "OPENAI_API_KEY"));
}

// from tests/test_automation_r3c_child_env.py::test_the_app_still_reads_its_keys_from_dotenv
// The app reads .env itself (M0.7); adopting it changes nothing for the app's own lookups.
TEST_CASE("the app still reads its keys from dotenv", "[test_automation_r3c_child_env]") {
    DotEnvFixture f;
    const auto name = prefixed("TEST_KEY");
    f.load({{name, "file-value"}}, false);
    fv::CredentialStore store(std::make_shared<fv::MemoryCredentialBackend>(), fv::identity());
    fv::MapEnvReader environment;
    const fv::DotEnv dotenv(f.file, f.interpolation);
    const auto resolved = fv::resolve_credential(store, environment, dotenv, name);
    CHECK(resolved.value == "file-value");
    CHECK(fv::credential_source(store, environment, dotenv, name) == fv::CredentialSource::DotEnv);
}

// from
// tests/test_automation_r3c_child_env.py::test_a_dotenv_key_is_dropped_whatever_case_the_file_writes_it_in
TEST_CASE("a dotenv key is dropped whatever case the file writes it in",
          "[test_automation_r3c_child_env]") {
    if (!ProcessEnv::kWindows) {
        SKIP("Names differ by case outside Windows");
    }
    DotEnvFixture f;
    f.env.set("USER_OWN_VAR", "kept");
    // Windows ignores case in variable names: this .env entry is ANTHROPIC_API_KEY there, which
    // an editor that loaded the file set.
    f.env.set("ANTHROPIC_API_KEY", "sk-from-dotenv");
    f.load({{"Anthropic_Api_Key", "sk-from-dotenv"}}, false);

    const auto env = fv::child_env(f.process);
    CHECK_FALSE(has_name(env, "ANTHROPIC_API_KEY"));
    CHECK(env.at("USER_OWN_VAR") == "kept");
}

// from
// tests/test_automation_r3c_child_env.py::test_names_are_matched_the_way_the_system_matches_them
TEST_CASE("names are matched the way the system matches them", "[test_automation_r3c_child_env]") {
    MapEnv backing(ProcessEnv::kWindows);
    ProcessEnv process(backing);
    EnvMap env;
    env.emplace("ANTHROPIC_API_KEY", "a");
    env.emplace("Anthropic_Api_Key", "b"); // the same variable on Windows
    env.emplace("KEEP", "1");
    const std::vector<std::string> names{"anthropic_api_key"};
    process.drop(env, names);
    if (ProcessEnv::kWindows) {
        CHECK(sorted_pairs(env) == Pairs{{"KEEP", "1"}});
    } else { // distinct variables elsewhere
        CHECK(env.size() == 3);
    }
}

// from tests/test_automation_r3c_child_env.py::test_openwhispers_internal_names_are_dropped_too
TEST_CASE("the app's internal names are dropped too", "[test_automation_r3c_child_env]") {
    MapEnv env(ProcessEnv::kWindows);
    ProcessEnv process(env);
    env.set(prefixed("SIDECAR_TOKEN"), "internal");
    env.set(std::string(fv::legacy_identity().env_prefix) + "SIDECAR_TOKEN", "internal");
    const auto result = fv::child_env(process);
    CHECK_FALSE(result.contains(prefixed("SIDECAR_TOKEN")));
    CHECK_FALSE(result.contains(std::string(fv::legacy_identity().env_prefix) + "SIDECAR_TOKEN"));
}

// -- FioriVoce's own -----------------------------------------------------------------------

TEST_CASE("adopting .env records only values the environment holds as the file has them",
          "[process]") {
    DotEnvFixture f;
    f.env.set("USER_KEY", "the-users-own");
    f.env.set("SAME_KEY", "from-file");
    fv::process_test::write_file(f.file, "USER_KEY=from-file\nSAME_KEY=from-file\nNOT_SET=x\n");
    f.process.adopt_dotenv(fv::DotEnv(f.file, f.interpolation));

    const auto env = f.process.external_env();
    CHECK(env.at("USER_KEY") == "the-users-own");
    CHECK_FALSE(env.contains("SAME_KEY"));
    CHECK_FALSE(env.contains("NOT_SET"));
    // Nothing was loaded into the environment.
    CHECK_FALSE(f.env.get("NOT_SET").has_value());
}

TEST_CASE("set_for_app refuses what no environment can hold", "[process]") {
    MapEnv env(true);
    ProcessEnv process(env, true);
    CHECK_THROWS_AS(process.set_for_app("A=B", "1"), std::invalid_argument);
    CHECK_THROWS_AS(process.set_for_app("A", std::string("x\0y", 3)), std::invalid_argument);
    CHECK_NOTHROW(process.set_for_app("=HIDDEN", "1"));
    CHECK(process.external_env().empty());
}

TEST_CASE("the system environment is read and written through ProcessEnv", "[process]") {
    const std::string name = prefixed("PROCESS_ENV_PROBE");
    fv::process_test::ScopedEnv scoped(name, "user");
    auto& process = ProcessEnv::instance();
    process.forget();
    process.set_for_app(name, "app");
    CHECK(fv::testkit::get_env(name) == "app");
    CHECK(process.external_env().at(name) == "user");
    CHECK(fv::child_env().at(name) == "user");
    process.forget();
}
