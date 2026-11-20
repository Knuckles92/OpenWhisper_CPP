// tests/test_typed_settings.py: the declarations and the typed read path, and the golden replay
// of every declaration's read and stored (tests/golden/services.setting_types.json).

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <set>
#include <string>
#include <string_view>

#include "golden/golden.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "settings/resolve.h"
#include "settings_test_support.h"

using fv::Json;
using fv::settings_test::same;
using fv::settings_test::untag;

namespace {

// Backends OpenWhisper's selected_model accepts and OpenWhisper_CPP does not (architecture 7.2).
const std::set<std::string> kDroppedBackends{"local_whisper", "qwen_asr",     "nemotron",
                                             "moonshine",     "parakeet_mlx", "parakeet_unified"};

// An IntSetting's read is always a Python int; one the vectors hold as a float was beyond
// uint64 (nlohmann parses such an integer as a double).
bool beyond_int64(const Json& value) {
    return value.is_number_float() ||
           (value.is_number_unsigned() &&
            value.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

// What OpenWhisper_CPP reads where it differs from OpenWhisper on purpose: a dropped engine's name
// in selected_model reads as the default, and an int Python holds beyond int64 (a huge number with
// no bound to clamp it to) is not representable and reads as the default too.
void apply_deliberate_differences(const fv::SettingBase& setting, Json& read, Json& stored) {
    if (setting.key() == "selected_model" && read.is_string() &&
        kDroppedBackends.contains(read.get<std::string>())) {
        read = setting.default_json();
        stored = nullptr;
    }
    if (setting.kind() == fv::SettingKind::Int && beyond_int64(read)) {
        read = setting.default_json();
        stored = nullptr;
    }
}

} // namespace

TEST_CASE("every key is a declared setting with its old name", "[test_typed_settings][golden]") {
    // from tests/test_typed_settings.py::test_every_key_is_a_declared_setting_with_its_old_name
    // The vectors list OpenWhisper's declarations; each is declared here, in the same order,
    // with the same kind, followed only by OpenWhisper_CPP's fv_ keys.
    std::vector<std::string> python;
    fv::settings_test::for_each_case(
        "services.setting_types", "unset", [&](const fv::golden::Case& c) {
            const auto key = c.arg<std::string>(0);
            python.push_back(key);
            const fv::SettingBase* setting = fv::keys::by_key(key);
            REQUIRE(setting != nullptr);
            CHECK(setting->key() == key);
            CHECK(fv::kind_name(setting->kind()) == c.result<Json>().at("kind").get<std::string>());
            CHECK(setting->in_table() == c.result<Json>().at("in_table").get<bool>());
        });
    const auto all = fv::keys::all();
    REQUIRE(all.size() >= python.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i < python.size()) {
            CHECK(all[i]->key() == python[i]);
        } else {
            CHECK(all[i]->key().starts_with("fv_"));
            CHECK_FALSE(all[i]->in_table());
        }
    }
    CHECK(python.size() == 186);
}

TEST_CASE("every table default is what a read of nothing returns", "[test_typed_settings]") {
    // from tests/test_typed_settings.py::test_every_table_default_is_what_a_read_of_nothing_returns
    const Json empty = Json::object();
    for (const fv::SettingBase* setting : fv::keys::all()) {
        INFO(setting->key());
        if (setting->in_table()) {
            CHECK(same(fv::keys::defaults_table().at(std::string(setting->key())),
                       setting->read_json(empty)));
        }
        CHECK(same(setting->default_json(), setting->read_json(empty)));
    }
}

TEST_CASE("defaults are fresh copies", "[test_typed_settings]") {
    // from tests/test_typed_settings.py::test_defaults_are_fresh_copies
    Json first = fv::keys::actions.read(Json::object());
    first.push_back("changed");
    CHECK(fv::keys::actions.read(Json::object()) == Json::array());
    CHECK(fv::keys::defaults_table().at("actions") == Json::array());
    Json fresh = fv::keys::actions.default_value();
    fresh.push_back("x");
    CHECK(fv::keys::actions.default_value() == Json::array());
}

TEST_CASE("settings by key finds each declaration from its bare name", "[test_typed_settings]") {
    // from
    // tests/test_typed_settings.py::test_settings_by_key_finds_each_declaration_from_its_bare_name
    for (const fv::SettingBase* setting : fv::keys::all()) {
        CHECK(fv::keys::by_key(setting->key()) == setting);
    }
    CHECK(fv::keys::by_key("not_a_setting") == nullptr);
    CHECK(fv::keys::by_key("AUTO_PASTE") == nullptr);
}

TEST_CASE("stored tells an unset or invalid value from the default", "[test_typed_settings]") {
    // from
    // tests/test_typed_settings.py::test_stored_tells_an_unset_or_invalid_value_from_the_default
    const auto& key = fv::keys::meeting_asr_model;
    CHECK_FALSE(key.stored(Json::object()).has_value());
    CHECK_FALSE(key.stored(Json{{"meeting_asr_model", 3}}).has_value());
    CHECK(key.stored(Json{{"meeting_asr_model", "x"}}) == "x");
}

TEST_CASE("kinds validate as documented", "[test_typed_settings]") {
    // from tests/test_typed_settings.py::test_kinds_validate_as_documented
    static constexpr std::string_view kLooks[] = {"a", "b"};
    static constexpr fv::ChoiceAlias kOld[] = {{"old", "b"}};

    const fv::BoolSetting flag{"flag", fv::spec::boolean(true)};
    CHECK(flag.parse(false) == false);
    CHECK(flag.parse(0) == true);
    CHECK(flag.parse("no") == true);
    CHECK(flag.parse(nullptr) == true);

    const fv::IntSetting port{"port", fv::spec::integer(80).strict().valid(1, 65535)};
    CHECK(port.parse(8080) == 8080);
    CHECK(port.parse("8080") == 80);
    CHECK(port.parse(true) == 80);
    CHECK(port.parse(0) == 80);
    CHECK(port.parse(70000) == 80);

    const fv::IntSetting size{"size", fv::spec::integer(12).clamp(10, 48)};
    CHECK(size.parse("60") == 48);
    CHECK(size.parse(3.9) == 10);
    CHECK(size.parse("x") == 12);

    const fv::ChoiceSetting look{"look", fv::spec::choice("a", kLooks).with_aliases(kOld)};
    CHECK(look.parse("b") == "b");
    CHECK(look.parse("old") == "b");
    CHECK(look.parse("c") == "a");
    CHECK(look.parse(Json::array({"b"})) == "a");

    static constexpr std::string_view kOnlyA[] = {"a"};
    CHECK_THROWS_AS(fv::ChoiceSetting("bad", fv::spec::choice("z", kOnlyA)), std::invalid_argument);
    try {
        fv::ChoiceSetting("bad", fv::spec::choice("z", kOnlyA));
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "bad: default 'z' is not one of its choices");
    }
}

TEST_CASE("resolve bool setting refuses a key that is not on off", "[test_typed_settings]") {
    // from tests/test_typed_settings.py::test_resolve_bool_setting_refuses_a_key_that_is_not_on_off
    CHECK_THROWS_AS(fv::resolve_bool_setting("whisper_model", Json::object()),
                    std::invalid_argument);
    try {
        fv::resolve_bool_setting("whisper_model", Json::object());
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "whisper_model is not an on/off setting");
    }
}

TEST_CASE("an integer setting saved as infinity reads as its default", "[test_typed_settings]") {
    // from
    // tests/test_typed_settings.py::test_an_integer_setting_saved_as_infinity_reads_as_its_default
    Json saved = Json::object();
    saved["max_saved_recordings"] = std::numeric_limits<double>::infinity();
    saved["meeting_server_port"] = -std::numeric_limits<double>::infinity();
    saved["walkthrough_capture_delay"] = std::numeric_limits<double>::infinity();
    CHECK(fv::keys::max_saved_recordings.read(saved) == 20);
    CHECK(fv::keys::meeting_server_port.read(saved) == 0);
    CHECK(fv::keys::walkthrough_capture_delay.read(saved) ==
          fv::keys::walkthrough_capture_delay.default_value());
}

TEST_CASE("port defaults match their servers", "[test_typed_settings]") {
    // from tests/test_typed_settings.py::test_port_defaults_match_their_servers
    // The servers (M5 MCP, the remote engine) take their default from these constants.
    CHECK(fv::keys::mcp_port.default_value() == fv::enums::kMcpDefaultPort);
    CHECK(fv::enums::kMcpDefaultPort == 8767);
    CHECK(fv::keys::remote_host_port.default_value() == fv::enums::kRemoteHostDefaultPort);
    CHECK(fv::enums::kRemoteHostDefaultPort == 47821);
}

TEST_CASE("config is where the defaults come from", "[test_typed_settings]") {
    // from tests/test_typed_settings.py::test_config_is_where_the_defaults_come_from
    // config.STREAMING_CHUNK_DURATION_SEC and config.DEFAULT_BACKEND on Windows x64, as the
    // generator evaluated them.
    CHECK(fv::keys::streaming_chunk_duration.default_value() == 3.0);
    CHECK(fv::keys::selected_model.default_value() == "parakeet");
}

TEST_CASE("the on off resolvers were all found", "[test_typed_settings][golden]") {
    // from tests/test_typed_settings.py::test_the_on_off_resolvers_were_all_found
    // Every on/off key the vectors hold is a Bool declaration here, at least 45 of them in
    // the table, as OpenWhisper's generated resolvers were.
    std::size_t on_off = 0;
    fv::settings_test::for_each_case(
        "services.setting_types", "unset", [&](const fv::golden::Case& c) {
            if (c.result<Json>().at("kind") == "BoolSetting") {
                const fv::SettingBase* setting = fv::keys::by_key(c.arg<std::string>(0));
                REQUIRE(setting != nullptr);
                CHECK(setting->kind() == fv::SettingKind::Bool);
                on_off += c.result<Json>().at("in_table").get<bool>() ? 1 : 0;
            }
        });
    CHECK(on_off >= 45);
}

TEST_CASE("a declaration of nothing reads its default and stores nothing",
          "[test_typed_settings][golden]") {
    // from tests/test_typed_settings.py::test_single_key_resolver_matches_the_old_one (MISSING)
    fv::settings_test::for_each_case(
        "services.setting_types", "unset", [](const fv::golden::Case& c) {
            const fv::SettingBase* setting = fv::keys::by_key(c.arg<std::string>(0));
            REQUIRE(setting != nullptr);
            const Json expected = untag(c.result<Json>());
            CHECK(same(expected.at("read"), setting->read_json(Json::object())));
            CHECK(same(expected.at("default"), setting->default_json()));
            CHECK_FALSE(setting->stored_json(Json::object()).has_value());
        });
}

TEST_CASE("each declaration reads and stores what OpenWhisper's does",
          "[test_typed_settings][golden]") {
    // from tests/test_typed_settings.py::test_single_key_resolver_matches_the_old_one,
    // test_the_declaration_reads_what_its_resolver_returns and
    // test_multi_key_resolver_matches_the_old_one: the read every resolver is built on, for
    // every declared key over the JUNK battery and the kind's edges.
    fv::settings_test::for_each_case(
        "services.setting_types", "read_stored", [](const fv::golden::Case& c) {
            const fv::SettingBase* setting = fv::keys::by_key(c.arg<std::string>(0));
            REQUIRE(setting != nullptr);
            const Json raw = untag(c.arg<Json>(1));
            Json snapshot = Json::object();
            snapshot[std::string(setting->key())] = raw;
            const Json expected = untag(c.result<Json>());
            Json read = expected.at("read");
            Json stored = expected.at("stored");
            apply_deliberate_differences(*setting, read, stored);
            CHECK(same(read, setting->read_json(snapshot)));
            const auto actual = setting->stored_json(snapshot);
            CHECK(same(stored, actual ? *actual : Json(nullptr)));
        });
}
