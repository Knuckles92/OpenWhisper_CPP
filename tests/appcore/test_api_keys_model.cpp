// The API keys page's lists and words (ui_qt/dialogs/settings_api_keys.py), replayed from the
// golden vectors of ui_qt.dialogs.settings_api_keys, and the status line per source.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appcore/api_keys_model.h"
#include "base/json.h"
#include "golden/golden.h"

using fv::CredentialSource;
using fv::CredentialStatus;
using fv::Json;
using fv::appcore::CredentialEntry;
using fv::appcore::CredentialOwner;

namespace {

constexpr const char* kModule = "ui_qt.dialogs.settings_api_keys";

std::vector<CredentialOwner> owners_from(const Json& rows) {
    std::vector<CredentialOwner> owners;
    for (const auto& row : rows) {
        owners.push_back(
            {row[0].get<std::string>(), row[1].get<std::string>(), row[2].get<std::string>()});
    }
    return owners;
}

std::vector<CredentialEntry> entries_from(const Json& rows) {
    std::vector<CredentialEntry> entries;
    for (const auto& row : rows) {
        entries.push_back({row[0].get<std::string>(), row[1].get<std::string>()});
    }
    return entries;
}

CredentialSource source_from(std::string_view name) {
    if (name == "stored") {
        return CredentialSource::Store;
    }
    if (name == "environment") {
        return CredentialSource::Environment;
    }
    if (name == "dotenv") {
        return CredentialSource::DotEnv;
    }
    return CredentialSource::None;
}

} // namespace

TEST_CASE("API key lists replay the golden vectors", "[golden][api_keys_model]") {
    fv::golden::for_each(kModule, "key_entries", [](const fv::golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(fv::appcore::key_entries(owners_from(expected.at("owners"))) ==
              entries_from(expected.at("entries")));
    });
    fv::golden::for_each(kModule, "rail_value", [](const fv::golden::Case& c) {
        const Json sources = c.arg<Json>(1);
        const Json expected = c.result<Json>();
        const auto entries = entries_from(expected.at("entries"));
        const auto source = [&](std::string_view name) {
            const auto found = sources.find(std::string(name));
            return found == sources.end() ? CredentialSource::None
                                          : source_from(found->get<std::string>());
        };
        CHECK(fv::appcore::api_keys_rail_value(entries, source) ==
              expected.at("value").get<std::string>());
    });
}

TEST_CASE("API key texts replay the golden vectors", "[golden][api_keys_model]") {
    fv::golden::for_each(kModule, "store_copy", [](const fv::golden::Case& c) {
        const CredentialStatus status{c.arg<bool>(0), c.arg<std::string>(1), c.arg<std::string>(2)};
        const Json expected = c.result<Json>();
        CHECK(fv::golden::equal_rebranded(expected.at("copy").get<std::string>(),
                                          fv::appcore::store_caption(status)));
        CHECK(fv::appcore::store_detail(status) == expected.at("detail").get<std::string>());
    });
    fv::golden::for_each(kModule, "ApiKeysSection._render_status", [](const fv::golden::Case& c) {
        const auto key = c.arg<std::optional<std::string>>(2);
        const auto status =
            fv::appcore::status_text(source_from(c.arg<std::string>(1)), c.arg<std::string>(4), key,
                                     c.arg<std::string>(0), c.arg<bool>(3));
        const Json expected = c.result<Json>();
        CHECK(status.text == expected.at("text").get<std::string>());
        CHECK(status.tone == expected.at("tone").get<std::string>());
        CHECK(status.icon == expected.at("icon").get<std::string>());
    });
    fv::golden::for_each(kModule, "ApiKeysSection._render_uses", [](const fv::golden::Case& c) {
        const Json expected = c.result<Json>();
        const std::string text =
            fv::appcore::uses_caption(c.arg<std::string>(0), owners_from(expected.at("owners")));
        CHECK(text == expected.at("text").get<std::string>());
        CHECK(!text.empty() == expected.at("visible").get<bool>());
    });
}

TEST_CASE("the status line names where the active key comes from", "[api_keys_model]") {
    const std::optional<std::string> key = "sk-proj-abcdefgh1234";
    const auto stored = fv::appcore::status_text(CredentialSource::Store,
                                                 "Windows Credential Manager", key, "GROQ_API_KEY");
    CHECK(stored.text == "Saved in Windows Credential Manager · ••••1234");
    CHECK(stored.tone == "success");
    CHECK(stored.icon == "check-green.svg");

    const auto shadowed =
        fv::appcore::status_text(CredentialSource::Store, "Keychain", key, "GROQ_API_KEY", true);
    CHECK(shadowed.text == "Saved in Keychain · ••••1234. The GROQ_API_KEY variable in your "
                           "environment or .env file is ignored while a key is saved here.");

    const auto environment =
        fv::appcore::status_text(CredentialSource::Environment, "", key, "GROQ_API_KEY");
    CHECK(environment.text == "Using the GROQ_API_KEY environment variable · ••••1234. Save a "
                              "key here to use it instead.");
    CHECK(environment.tone == "success");

    const auto dotenv =
        fv::appcore::status_text(CredentialSource::DotEnv, "", "short", "GROQ_API_KEY");
    CHECK(dotenv.text == "Using the .env file · ••••. Save a key here to use it instead.");

    const auto none =
        fv::appcore::status_text(CredentialSource::None, "", std::nullopt, "GROQ_API_KEY");
    CHECK(none.text == "No key set.");
    CHECK(none.tone == "warning");
    CHECK(none.icon == "info-warning.svg");

    CHECK(fv::appcore::status_text(CredentialSource::None, "", std::nullopt, "").text ==
          "No endpoint needs an API key.");
}

TEST_CASE("speech-endpoint owners share one row per variable", "[api_keys_model]") {
    const std::vector<CredentialOwner> owners{
        {"OPENAI_API_KEY", "OpenAI", "speech"},    {"GROQ_API_KEY", "Groq", "speech"},
        {"SHARED_KEY", "Custom server", "speech"}, {"SHARED_KEY", "Speaches", "speech"},
        {"SHARED_KEY", "Custom server", "speech"},
    };
    const auto entries = fv::appcore::key_entries(owners);
    REQUIRE(entries.size() == 3);
    CHECK(entries[2] == CredentialEntry{"SHARED_KEY", "Custom server / Speaches · SHARED_KEY"});
    CHECK(fv::appcore::api_keys_rail_value(
              {}, [](std::string_view) { return CredentialSource::Store; }) == "None needed");
    CHECK(fv::appcore::api_keys_rail_value(entries, [](std::string_view name) {
              return name == "GROQ_API_KEY" ? CredentialSource::DotEnv : CredentialSource::None;
          }) == "1 of 3 set");
    CHECK(fv::appcore::uses_caption("SHARED_KEY", owners) ==
          "Sent as the API key to Custom server and Speaches and Custom server for API "
          "transcription.");
    CHECK(fv::appcore::uses_caption("", owners).empty());
}
