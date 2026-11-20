// tests/test_settings.py: the single-key resolvers (now keys::<key>.read) of the Upload File
// batch, the record trigger, the font scale and the update preferences, and the enum tables.

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "pyport/text.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "settings/resolve.h"
#include "settings/store.h"
#include "settings_test_support.h"

using fv::Json;
namespace keys = fv::keys;

namespace {

Json with(std::string_view key, Json value) {
    Json settings = Json::object();
    settings[std::string(key)] = std::move(value);
    return settings;
}

} // namespace

TEST_CASE("restore clipboard is off until saved on", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_restore_clipboard_is_off_until_saved_on
    fv::settings_test::TempDir temp;
    const auto path = temp.path() / "test_settings.json";
    fv::SettingsStore store(path);
    CHECK(fv::setting_value("restore_clipboard", *store.snapshot()) == false);
    // Someone who turned it on before the default changed keeps it on.
    fv::settings_test::write_bytes(path, R"({"restore_clipboard": true})");
    CHECK(fv::setting_value("restore_clipboard", *store.snapshot()) == true);
}

TEST_CASE("relation defaults to separate", "[test_settings]") {
    // from tests/test_settings.py::TestTranscriptBatchSettings::test_relation_defaults_to_separate
    CHECK(keys::transcript_batch_relation.read(Json::object()) == "separate");
    CHECK(keys::transcript_batch_relation.default_value() == "separate");
}

TEST_CASE("relation accepts known presets only", "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptBatchSettings::test_relation_accepts_known_presets_only
    const auto& key = keys::transcript_batch_relation;
    CHECK(key.read(with(key.key(), "sequential")) == "sequential");
    CHECK(key.read(with(key.key(), "custom")) == "custom");
    CHECK(key.read(with(key.key(), "stitched")) == "separate");
    CHECK(key.read(with(key.key(), 3)) == "separate");
}

TEST_CASE("instructions are stripped and capped", "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptBatchSettings::test_instructions_are_stripped_and_capped
    const auto& key = keys::transcript_batch_custom_instructions;
    CHECK(key.read(with(key.key(), "  memo set  ")) == "memo set");
    CHECK(key.read(with(key.key(), 12)) == "");
    CHECK(key.read(Json::object()) == "");
    // config.MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS, counted in code points.
    CHECK(fv::py::len(key.read(with(key.key(), std::string(2050, 'x')))) == 2000);
    std::string accented;
    for (int i = 0; i < 2010; ++i) {
        accented += "\xc3\xa9";
    }
    CHECK(fv::py::len(key.read(with(key.key(), accented))) == 2000);
}

TEST_CASE("combine is a strict bool", "[test_settings]") {
    // from tests/test_settings.py::TestTranscriptBatchSettings::test_combine_is_a_strict_bool
    const auto& key = keys::transcript_batch_custom_combine;
    CHECK(key.read(with(key.key(), true)) == true);
    CHECK(key.read(with(key.key(), "yes")) == false);
    CHECK(key.read(Json::object()) == false);
}

TEST_CASE("values round trip through settings file", "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptBatchSettings::test_values_round_trip_through_settings_file
    fv::settings_test::TempDir temp;
    fv::SettingsStore store(temp.path() / "settings.json");
    store.save_setting(keys::transcript_batch_relation.key(), "custom");
    store.save_setting(keys::transcript_batch_custom_instructions.key(), "Two halves.");
    store.save_setting(keys::transcript_batch_custom_combine.key(), true);
    const auto loaded = store.snapshot();
    CHECK(keys::transcript_batch_relation.read(*loaded) == "custom");
    CHECK(keys::transcript_batch_custom_instructions.read(*loaded) == "Two halves.");
    CHECK(keys::transcript_batch_custom_combine.read(*loaded) == true);
}

TEST_CASE("missing key falls back to config default", "[test_settings]") {
    // from
    // tests/test_settings.py::TestRecordingTriggerMode::test_missing_key_falls_back_to_config_default
    CHECK(keys::recording_trigger_mode.default_value() == "toggle");
    CHECK(keys::recording_trigger_mode.read(Json::object()) == "toggle");
}

TEST_CASE("saved mode round trips", "[test_settings]") {
    // from tests/test_settings.py::TestRecordingTriggerMode::test_saved_mode_round_trips
    CHECK(keys::recording_trigger_mode.read(with("recording_trigger_mode", "push_hold")) ==
          "push_hold");
}

TEST_CASE("invalid value falls back to config default", "[test_settings]") {
    // from
    // tests/test_settings.py::TestRecordingTriggerMode::test_invalid_value_falls_back_to_config_default
    CHECK(keys::recording_trigger_mode.read(with("recording_trigger_mode", "pressy")) == "toggle");
}

TEST_CASE("missing key falls back to default", "[test_settings]") {
    // from tests/test_settings.py::TestUiFontScale::test_missing_key_falls_back_to_default
    CHECK(keys::ui_font_scale.default_value() == 100);
    CHECK(keys::ui_font_scale.read(Json::object()) == 100);
}

TEST_CASE("saved percent round trips", "[test_settings]") {
    // from tests/test_settings.py::TestUiFontScale::test_saved_percent_round_trips
    CHECK(keys::ui_font_scale.read(with("ui_font_scale", 115)) == 115);
}

TEST_CASE("invalid value falls back to default", "[test_settings]") {
    // from tests/test_settings.py::TestUiFontScale::test_invalid_value_falls_back_to_default
    CHECK(keys::ui_font_scale.read(with("ui_font_scale", 150)) == 100);
    CHECK(keys::ui_font_scale.read(with("ui_font_scale", "large")) == 100);
}

TEST_CASE("defaults are on", "[test_settings]") {
    // from tests/test_settings.py::TestUpdatePreferences::test_defaults_are_on
    CHECK(keys::update_check_enabled.default_value());
    CHECK(keys::update_notify_enabled.default_value());
    CHECK(keys::update_check_enabled.read(Json::object()));
    CHECK(keys::update_notify_enabled.read(Json::object()));
}

TEST_CASE("saved false round trips", "[test_settings]") {
    // from tests/test_settings.py::TestUpdatePreferences::test_saved_false_round_trips
    const Json saved{{"update_check_enabled", false}, {"update_notify_enabled", false}};
    CHECK_FALSE(keys::update_check_enabled.read(saved));
    CHECK_FALSE(keys::update_notify_enabled.read(saved));
}

TEST_CASE("notify off with check on", "[test_settings]") {
    // from tests/test_settings.py::TestUpdatePreferences::test_notify_off_with_check_on
    const Json saved{{"update_check_enabled", true}, {"update_notify_enabled", false}};
    CHECK(keys::update_check_enabled.read(saved));
    CHECK_FALSE(keys::update_notify_enabled.read(saved));
}

TEST_CASE("non bool falls back to default", "[test_settings]") {
    // from tests/test_settings.py::TestUpdatePreferences::test_non_bool_falls_back_to_default
    const Json saved{{"update_check_enabled", "yes"}, {"update_notify_enabled", 0}};
    CHECK(keys::update_check_enabled.read(saved));
    CHECK(keys::update_notify_enabled.read(saved));
}

TEST_CASE("an optional kind reads null unset and an empty container when malformed") {
    CHECK(keys::app_style_tones.read(Json::object()).is_null());
    CHECK(keys::app_style_tones.read(with("app_style_tones", "x")) == Json::object());
    CHECK(keys::text_transforms.read(with("text_transforms", 3)) == Json::array());
    CHECK(keys::text_transforms.stored(with("text_transforms", 3)) == Json::array());
    CHECK(keys::window_geometry.read(Json::object()).is_null());
    CHECK(keys::window_geometry.read(with("window_geometry", "1x2")) == "1x2");
}

TEST_CASE("selected model reads a dropped engine as the default") {
    // Architecture 7.2: OpenWhisper_CPP's choices are parakeet, api and remote.
    for (const char* dropped : {"local_whisper", "qwen_asr", "nemotron", "moonshine",
                                "parakeet_mlx", "parakeet_unified"}) {
        CHECK(keys::selected_model.read(with("selected_model", dropped)) == "parakeet");
        CHECK_FALSE(keys::selected_model.stored(with("selected_model", dropped)).has_value());
    }
    CHECK(keys::selected_model.read(with("selected_model", "api_gpt4o")) == "api");
    CHECK(keys::selected_model.read(with("selected_model", "remote")) == "remote");
}

TEST_CASE("the keys of dropped engines are declared, dropped and still read as Python does") {
    for (const auto* setting :
         {static_cast<const fv::SettingBase*>(&keys::whisper_model),
          static_cast<const fv::SettingBase*>(&keys::custom_whisper_models),
          static_cast<const fv::SettingBase*>(&keys::whisper_device),
          static_cast<const fv::SettingBase*>(&keys::whisper_compute_type),
          static_cast<const fv::SettingBase*>(&keys::whisper_gpu_offer_declined),
          static_cast<const fv::SettingBase*>(&keys::meeting_whisper_model)}) {
        CHECK(setting->dropped());
        CHECK(setting->in_table());
    }
    std::size_t dropped = 0;
    for (const auto* setting : keys::all()) {
        dropped += setting->dropped() ? 1 : 0;
    }
    CHECK(dropped == 6);
}

TEST_CASE("the OpenWhisper_CPP keys are declared after OpenWhisper's and outside the table") {
    CHECK(keys::fv_speech_runtime.read(Json::object()) == "");
    CHECK(keys::fv_speech_runtime.read(with("fv_speech_runtime", "cuda")) == "cuda");
    CHECK(keys::fv_speech_runtime.read(with("fv_speech_runtime", "gpu")) == "");
    CHECK(keys::fv_runtime_dir.read(with("fv_runtime_dir", "C:/nemo")) == "C:/nemo");
    CHECK(keys::fv_import.read(Json::object()).is_null());
    CHECK(keys::fv_cleanup_agent_consent.read(Json::object()).empty());
    for (std::string_view key :
         {"fv_speech_runtime", "fv_runtime_dir", "fv_import", "fv_cleanup_agent_consent"}) {
        CHECK_FALSE(fv::keys::defaults_table().contains(std::string(key)));
    }
}

TEST_CASE("legacy streaming keys are OpenWhisper's four") {
    std::vector<std::string_view> legacy(std::begin(keys::legacy_streaming),
                                         std::end(keys::legacy_streaming));
    CHECK(legacy ==
          std::vector<std::string_view>{"streaming_overlay_enabled", "streaming_paste_enabled",
                                        "streaming_tiny_model_enabled", "live_typing_enabled"});
}

TEST_CASE("the enum tables are OpenWhisper's") {
    using fv::enums::EnumId;
    CHECK(fv::enums::label(EnumId::UiFontScale, 115) == "Large");
    CHECK(fv::enums::label(EnumId::UiTheme, "system") == "Match system");
    CHECK(fv::enums::label(EnumId::RecordingOverlayLook, "legacy") == "Classic");
    CHECK(fv::enums::details(EnumId::StreamingOverlayLayout).at(0).text ==
          "Clock and a small meter above the words");
    CHECK(fv::enums::aliases(EnumId::RecordingOverlayLook).size() == 3);
    CHECK(fv::enums::labels(EnumId::MeetingLanguage).size() == 29);
    CHECK(fv::enums::labels(EnumId::MeetingLanguage).at(0).text == "Detect automatically");
    CHECK(fv::enums::legacy(EnumId::MeetingAgentCore) == Json::array({"direct"}));
    CHECK(fv::enums::installed(EnumId::TranscriptCleanupProvider) ==
          Json::array({"claude_code", "codex", "opencode_cli"}));
    CHECK_FALSE(fv::enums::label(EnumId::UiTheme, "neon").has_value());
    // Each choice setting's values are its class's ALL.
    CHECK(fv::enums::values(EnumId::RecordingOverlayLook).size() ==
          std::size(keys::choices::recording_overlay_look));
    CHECK(fv::enums::values(EnumId::MeetingLanguage).size() ==
          std::size(keys::choices::meeting_language));
}

TEST_CASE("the module constants match the generated table") {
    const Json& constants = fv::enums::constants();
    CHECK(constants.at("MCP_DEFAULT_PORT") == fv::enums::kMcpDefaultPort);
    CHECK(constants.at("REMOTE_HOST_DEFAULT_PORT") == fv::enums::kRemoteHostDefaultPort);
    CHECK(constants.at("MEETING_LINUX_PREVIEW_ACK_VERSION") ==
          fv::enums::kMeetingLinuxPreviewAckVersion);
    CHECK(constants.at("INSIGHT_REVIEW_CONSENT") == std::string(fv::enums::kInsightReviewConsent));
    Json delays = Json::array();
    for (const auto delay : fv::enums::kWalkthroughCaptureDelays) {
        delays.push_back(delay);
    }
    CHECK(constants.at("WALKTHROUGH_CAPTURE_DELAYS") == delays);
    CHECK(fv::enums::default_voice_command_names().size() == 5);
    CHECK(fv::enums::default_voice_command_names().front() == "note taker");
}
