// Replays tests/golden/services.installed_agents.json (tools/parity/cases/
// services.installed_agents.py): version parsing, JSONC, the sign-in parsers, the configuration
// readers and the model names, as OpenWhisper computes them.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "agents/agent_config.h"
#include "agents/installed_agents.h"
#include "agents_test_support.h"
#include "base/utf8.h"
#include "golden/golden.h"

namespace golden = fv::golden;
using fv::Json;
using fv::agents_test::TempDir;
using fv::agents_test::write_file;

namespace {

constexpr std::string_view kModule = "services.installed_agents";

// The case's files under a fresh folder, with CLAUDE_CONFIG_DIR, CODEX_HOME and
// XDG_CONFIG_HOME pointing at its claude/, codex/ and xdg/ (as the case file's _with_config).
struct ConfigFolder {
    TempDir temp;
    fv::agents::AgentConfig config;

    explicit ConfigFolder(const Json& files)
        : config(
              [root = temp.path()](std::string_view name) -> std::optional<std::string> {
                  static const std::map<std::string, std::string, std::less<>> folders{
                      {"CLAUDE_CONFIG_DIR", "claude"},
                      {"CODEX_HOME", "codex"},
                      {"XDG_CONFIG_HOME", "xdg"}};
                  const auto it = folders.find(name);
                  if (it == folders.end()) {
                      return std::nullopt;
                  }
                  return fv::path_to_utf8(root / it->second);
              },
              temp.path() / "home") {
        for (const char* folder : {"claude", "codex", "xdg"}) {
            std::filesystem::create_directories(temp.path() / folder);
        }
        for (const auto& [relative, text] : files.items()) {
            write_file(temp.path() / fv::path_from_utf8(relative), text.get<std::string>());
        }
    }
};

std::optional<int> code_arg(const golden::Case& c) {
    return c.arg<std::optional<int>>(0);
}

Json sign_in_json(const std::pair<std::optional<bool>, std::string>& result) {
    return Json::array({result.first ? Json(*result.first) : Json(nullptr), result.second});
}

Json models_json(const std::vector<fv::agents::AgentModel>& models) {
    Json out = Json::array();
    for (const auto& model : models) {
        out.push_back(Json::array({model.value, model.label}));
    }
    return out;
}

} // namespace

TEST_CASE("parse_version replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "parse_version", [](const golden::Case& c) {
        const auto parsed = fv::agents::parse_version(c.arg<std::string>(0));
        Json key = Json::array();
        if (parsed.key) {
            for (const auto part : *parsed.key) {
                key.push_back(part);
            }
        }
        CHECK(golden::equal(c.result<Json>(), Json::array({key, parsed.text})));
    });
}

TEST_CASE("_strip_jsonc replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "_strip_jsonc", [](const golden::Case& c) {
        CHECK(fv::agents::strip_jsonc(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("_claude_sign_in replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "_claude_sign_in", [](const golden::Case& c) {
        const auto result = fv::agents::claude_sign_in_from(code_arg(c), c.arg<std::string>(1));
        CHECK(golden::equal(c.result<Json>(), sign_in_json(result)));
    });
}

TEST_CASE("_codex_sign_in replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "_codex_sign_in", [](const golden::Case& c) {
        const auto result = fv::agents::codex_sign_in_from(code_arg(c), c.arg<std::string>(1));
        CHECK(golden::equal(c.result<Json>(), sign_in_json(result)));
    });
}

TEST_CASE("sign_in_hint replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "sign_in_hint", [](const golden::Case& c) {
        CHECK(fv::agents::sign_in_hint(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("configured_default_model replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "configured_default_model", [](const golden::Case& c) {
        ConfigFolder folder(c.args()[0]);
        CHECK(folder.config.configured_default_model(c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("default_model_label replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "default_model_label", [](const golden::Case& c) {
        ConfigFolder folder(c.args()[0]);
        CHECK(folder.config.default_model_label(c.arg<std::string>(1)) == c.result<std::string>());
    });
}

TEST_CASE("_codex_models replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "_codex_models", [](const golden::Case& c) {
        ConfigFolder folder(c.args()[0]);
        CHECK(golden::equal(c.result<Json>(), models_json(folder.config.codex_models())));
    });
}

TEST_CASE("codex_model_efforts replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "codex_model_efforts", [](const golden::Case& c) {
        ConfigFolder folder(c.args()[0]);
        const auto efforts = folder.config.codex_model_efforts();
        Json actual = Json::object();
        for (const auto& [slug, levels] : efforts) {
            actual[slug] = levels;
        }
        CHECK(golden::equal(c.result<Json>(), actual));
        // The catalog's order, as the dict keeps it.
        std::vector<std::string> expected_order;
        const auto expected_efforts = c.result<Json>();
        for (const auto& [slug, levels] : expected_efforts.items()) {
            expected_order.push_back(slug);
        }
        std::vector<std::string> actual_order;
        for (const auto& [slug, levels] : efforts) {
            actual_order.push_back(slug);
        }
        CHECK(actual_order == expected_order);
    });
}

TEST_CASE("model_display_name replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "model_display_name", [](const golden::Case& c) {
        ConfigFolder folder(c.args()[0]);
        std::vector<fv::agents::AgentModel> models;
        if (!c.args()[3].is_null()) {
            for (const auto& entry : c.args()[3]) {
                models.push_back({entry[0].get<std::string>(), entry[1].get<std::string>()});
            }
        }
        CHECK(fv::agents::model_display_name(folder.config, c.arg<std::string>(1),
                                             c.arg<std::string>(2),
                                             models) == c.result<std::string>());
    });
}

TEST_CASE("describe_choice replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "describe_choice", [](const golden::Case& c) {
        ConfigFolder folder(c.args()[0]);
        CHECK(fv::agents::describe_choice(folder.config, c.arg<std::string>(1),
                                          c.arg<std::string>(2)) == c.result<std::string>());
    });
}

TEST_CASE("AGENT_SPECS replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "AGENT_SPECS", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const auto& spec : fv::agents::agent_specs()) {
            Json commands = Json::array();
            for (const auto command : spec.commands) {
                commands.push_back(std::string(command));
            }
            const auto s = [](std::string_view text) {
                return std::string(text);
            };
            actual.push_back(Json{{"id", s(spec.id)},
                                  {"name", s(spec.name)},
                                  {"vendor", s(spec.vendor)},
                                  {"commands", commands},
                                  {"transport", s(spec.transport)},
                                  {"install_url", s(spec.install_url)},
                                  {"install_hint", s(spec.install_hint)},
                                  {"monogram", s(spec.monogram)},
                                  {"accent", s(spec.accent)},
                                  {"min_version", spec.min_version},
                                  {"update_hint", s(spec.update_hint)}});
        }
        CHECK(golden::equal(c.result<Json>(), actual));
        std::vector<std::string> order;
        for (const auto id : fv::agents::agent_order()) {
            order.emplace_back(id);
        }
        CHECK(order == std::vector<std::string>{"claude_code", "codex", "opencode_cli"});
    });
}

TEST_CASE("InstalledAgent.usable replays OpenWhisper's vectors", "[installed_agents][golden]") {
    golden::for_each(kModule, "usable", [](const golden::Case& c) {
        fv::agents::InstalledAgent agent{.id = "codex",
                                         .path = "codex",
                                         .version = "0.158.0",
                                         .signed_in = c.arg<std::optional<bool>>(1),
                                         .problem = c.arg<std::string>(0)};
        CHECK(agent.usable() == c.result<bool>());
    });
}
