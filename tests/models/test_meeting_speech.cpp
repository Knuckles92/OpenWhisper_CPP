// Which engine and model transcribe a meeting: OpenWhisper's
// tests/test_meeting_speech_follows_dictation.py, the meeting-speech tests of
// tests/test_settings.py (TestMeetingSettings and the separate-engine default), the
// speaker-identification half of tests/test_openai_retirement.py, and the services.settings_meeting
// vectors. Local Whisper is not an engine here: where OpenWhisper's tests use it or another dropped
// engine, the Parakeet family (Parakeet TDT v3 and Orukeet) stands in.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "models/meeting_speech.h"
#include "models_test_support.h"
#include "settings/keys.h"
#include "settings/store.h"

using namespace fv::models;
using fv::Json;
using fv::models::test::read_file;
using fv::models::test::TempDir;
using fv::models::test::write_file;
namespace fs = std::filesystem;
namespace golden = fv::golden;
namespace keys = fv::keys;

namespace {

constexpr std::string_view kSeparate = "meeting_speech_separate";

// A settings file in a temporary folder; `saved` is written before the store opens it, so the
// install is not new.
struct Install {
    TempDir temp;
    fs::path file = temp.path() / "settings.json";
    std::unique_ptr<fv::SettingsStore> store;

    explicit Install(const Json* saved) {
        if (saved != nullptr) {
            write_file(file, saved->dump());
        }
        store = std::make_unique<fv::SettingsStore>(file);
    }
};

Json speech_json(const MeetingSpeech& speech) {
    return Json{{"source", speech.source},
                {"model", speech.model},
                {"separate", speech.separate},
                {"note", speech.note}};
}

std::chrono::system_clock::time_point local_noon(std::chrono::year_month_day day) {
    return std::chrono::sys_days{day} + std::chrono::hours(12);
}

constexpr std::chrono::year_month_day kBeforeShutdown{std::chrono::year{2027},
                                                      std::chrono::month{2}, std::chrono::day{25}};
constexpr std::chrono::year_month_day kShutdown{std::chrono::year{2027}, std::chrono::month{2},
                                                std::chrono::day{26}};

} // namespace

TEST_CASE("test_a_meeting_follows_a_catalog_engine_and_its_model",
          "[test_meeting_speech_follows_dictation]") {
    const auto speech = resolve_meeting_speech(
        Json{{"selected_model", "parakeet"}, {"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}});
    CHECK(speech == MeetingSpeech{"local", "orukeet-v0.1", false, ""});
}

TEST_CASE("Remote dictation never chooses the paired computer for meeting speech",
          "[test_meeting_speech_follows_dictation]") {
    const auto speech = resolve_meeting_speech(Json{{"selected_model", "remote"}});
    CHECK(speech.source == "local");
    CHECK_FALSE(speech.separate);
}

TEST_CASE("test_saved_meeting_choices_wait_unused_while_following",
          "[test_meeting_speech_follows_dictation]") {
    const Json settings{{"selected_model", "parakeet"},
                        {"meeting_asr_model", "orukeet-v0.1"},
                        {"meeting_asr_source", "remote"}};
    CHECK(resolve_meeting_speech(settings).model == "parakeet-v3");
    CHECK(resolve_meeting_speech(settings).source == "local");
    const auto own = own_meeting_speech(settings);
    CHECK(own.source == "remote");
    CHECK(own.model == "orukeet-v0.1");
    CHECK(own.separate);
}

TEST_CASE("test_a_separate_meeting_engine_ignores_dictation",
          "[test_meeting_speech_follows_dictation]") {
    const auto speech = resolve_meeting_speech(Json{{"selected_model", "remote"},
                                                    {"meeting_speech_separate", true},
                                                    {"meeting_asr_model", "orukeet-v0.1"}});
    CHECK(speech.source == "local");
    CHECK(speech.model == "orukeet-v0.1");
    CHECK(speech.separate);
}

TEST_CASE("test_an_engine_that_cannot_run_meetings_falls_back_and_says_so",
          "[test_meeting_speech_follows_dictation]") {
    const auto speech = resolve_meeting_speech(Json{{"selected_model", "api"}});
    CHECK(speech.source == "local");
    CHECK(speech.model == "parakeet-v3");
    CHECK_FALSE(speech.separate);
    CHECK(speech.note == "The API engine can't transcribe meetings, so meetings use Parakeet TDT "
                         "0.6B v3.");
    // OpenWhisper's other engines (Parakeet Unified, Qwen3-ASR) are not engines here: a saved one
    // reads as Parakeet, which runs meetings.
    for (const char* dropped : {"parakeet_unified", "qwen_asr", "local_whisper"}) {
        const auto other = resolve_meeting_speech(Json{{"selected_model", dropped}});
        CHECK(other == MeetingSpeech{"local", "parakeet-v3", false, ""});
    }
}

TEST_CASE("test_a_legacy_api_selection_counts_as_the_api_engine",
          "[test_meeting_speech_follows_dictation]") {
    for (const char* legacy : {"api_whisper", "api_gpt4o", "api_gpt4o_mini"}) {
        CHECK(resolve_meeting_speech(Json{{"selected_model", legacy}})
                  .note.starts_with("The API engine"));
    }
}

TEST_CASE("test_a_new_install_follows_dictation", "[test_meeting_speech_follows_dictation]") {
    Install install(nullptr);
    CHECK(keep_separate_meeting_speech(*install.store));
    CHECK(install.store->value(kSeparate) == Json(false));
    CHECK_FALSE(keep_separate_meeting_speech(*install.store));
}

TEST_CASE("test_an_install_with_a_different_meeting_engine_keeps_it",
          "[test_meeting_speech_follows_dictation]") {
    const Json saved = GENERATE(
        // Dictation on the paired computer, meetings on a local model.
        Json{{"selected_model", "remote"}, {"meeting_asr_model", "parakeet-v3"}},
        // Meetings on the paired computer, dictation here.
        Json{{"selected_model", "parakeet"}, {"meeting_asr_source", "remote"}},
        // Two different local models.
        Json{{"selected_model", "parakeet"},
             {"local_asr_models", {{"parakeet", "parakeet-v3"}}},
             {"meeting_asr_model", "orukeet-v0.1"}});
    Install install(&saved);
    const auto before = own_meeting_speech(*install.store->snapshot());

    CHECK(keep_separate_meeting_speech(*install.store));

    const auto after = resolve_meeting_speech(*install.store->snapshot());
    CHECK(after.separate);
    CHECK(after.source == before.source);
    CHECK(after.model == before.model);
}

TEST_CASE("test_an_install_whose_meetings_matched_dictation_follows_it",
          "[test_meeting_speech_follows_dictation]") {
    const Json saved =
        GENERATE(Json::object(), Json{{"selected_model", "parakeet"}},
                 Json{{"selected_model", "parakeet"}, {"meeting_asr_model", "parakeet-v3"}},
                 Json{{"selected_model", "parakeet"},
                      {"local_asr_models", {{"parakeet", "orukeet-v0.1"}}},
                      {"meeting_asr_model", "orukeet-v0.1"}},
                 Json{{"selected_model", "remote"}, {"meeting_asr_source", "remote"}});
    Install install(&saved);
    CHECK(keep_separate_meeting_speech(*install.store));
    CHECK(install.store->value(kSeparate) == Json(false));
}

TEST_CASE("test_the_choice_is_made_once", "[test_meeting_speech_follows_dictation]") {
    const Json saved{{"selected_model", "parakeet"}};
    Install install(&saved);
    CHECK(keep_separate_meeting_speech(*install.store));
    // A later change of dictation model must not make the meeting separate.
    install.store->save_setting("local_asr_models", Json{{"parakeet", "orukeet-v0.1"}});
    const std::string bytes = read_file(install.file);
    const auto stamp = fs::last_write_time(install.file);

    CHECK_FALSE(keep_separate_meeting_speech(*install.store));
    CHECK(fs::last_write_time(install.file) == stamp);
    CHECK(read_file(install.file) == bytes);
    const auto speech = resolve_meeting_speech(*install.store->snapshot());
    CHECK(speech.model == "orukeet-v0.1");
    CHECK_FALSE(speech.separate);
}

TEST_CASE("TestMeetingSettings::test_defaults_come_from_config", "[test_settings]") {
    // A meeting engine of its own with nothing saved takes the default meeting model.
    CHECK(resolve_meeting_whisper_model(Json{{"selected_model", "local_whisper"},
                                             {"meeting_speech_separate", true}}) == "parakeet-v3");
    const Json empty = Json::object();
    CHECK(keys::meeting_language.read(empty) == "auto");
    CHECK(keys::meeting_llm_provider.read(empty) == "openrouter");
    CHECK(keys::meeting_llm_model.read(empty) == "openrouter/free");
    CHECK(keys::meeting_agent_core.read(empty) == "pi");
    CHECK(resolve_meeting_speaker_id_backend(empty, fv::FakeClock()) == "local");
    CHECK_FALSE(keys::meeting_audio_upload_consent_given.read(empty));
    CHECK(keys::meeting_linux_preview_ack_version.read(empty) != 1);
    CHECK_FALSE(keys::meeting_unsupported_platform_ack.read(empty));
    CHECK_FALSE(keys::meeting_mode_intro_seen.read(empty));
    CHECK_FALSE(keys::meeting_past_recall_enabled.read(empty));
    CHECK_FALSE(keys::meeting_context_folder_enabled.read(empty));
    CHECK(keys::meeting_context_folder_path.read(empty).empty());
    CHECK(keys::meeting_server_bind.read(empty) == "localhost");
    CHECK(keys::meeting_server_port.read(empty) == 0);
    CHECK_FALSE(keys::meeting_end_redecode.read(empty));
    CHECK(keys::meeting_end_polish.read(empty));
    CHECK(keys::meeting_end_report.read(empty));
    CHECK(keys::meeting_report_ribbon.read(empty));
    CHECK(keys::meeting_report_brief.read(empty));
    CHECK(keys::meeting_report_signal.read(empty));
    CHECK_FALSE(keys::developer_mode.read(empty));
}

TEST_CASE("TestMeetingSettings::test_saved_choices_round_trip", "[test_settings]") {
    const Json saved{
        {"meeting_speech_separate", true},
        {"meeting_asr_model", "orukeet-v0.1"},
        {"meeting_language", "en"},
        {"meeting_llm_provider", "openai"},
        {"meeting_agent_core", "pi"},
        {"meeting_speaker_id_backend", "openai"},
        {"meeting_audio_upload_consent_given", true},
        {"meeting_linux_preview_ack_version", 1},
        {"meeting_unsupported_platform_ack", true},
        {"meeting_mode_intro_seen", true},
        {"meeting_past_recall_enabled", true},
        {"meeting_context_folder_enabled", true},
        {"meeting_server_bind", "lan"},
        {"meeting_server_port", 8099},
        {"meeting_end_redecode", true},
        {"meeting_end_polish", false},
        {"meeting_end_report", false},
        {"meeting_report_ribbon", true},
        {"meeting_report_brief", false},
        {"meeting_report_signal", false},
    };
    CHECK(resolve_meeting_whisper_model(saved) == "orukeet-v0.1");
    CHECK(keys::meeting_language.read(saved) == "en");
    CHECK(keys::meeting_llm_provider.read(saved) == "openai");
    CHECK(keys::meeting_agent_core.read(saved) == "pi");
    // OpenWhisper keeps "openai" until OpenAI's shutdown day; FioriVoce has no OpenAI pass.
    CHECK(resolve_meeting_speaker_id_backend(saved, fv::FakeClock()) == "local");
    CHECK(resolve_meeting_speaker_id_backend(Json{{"meeting_speaker_id_backend", "off"}},
                                             fv::FakeClock()) == "off");
    CHECK(keys::meeting_audio_upload_consent_given.read(saved));
    CHECK(keys::meeting_linux_preview_ack_version.read(saved) == 1);
    CHECK(keys::meeting_unsupported_platform_ack.read(saved));
    CHECK(keys::meeting_mode_intro_seen.read(saved));
    CHECK(keys::meeting_past_recall_enabled.read(saved));
    CHECK(keys::meeting_context_folder_enabled.read(saved));
    CHECK(keys::meeting_server_bind.read(saved) == "lan");
    CHECK(keys::meeting_server_port.read(saved) == 8099);
    CHECK(keys::meeting_end_redecode.read(saved));
    CHECK_FALSE(keys::meeting_end_polish.read(saved));
    CHECK_FALSE(keys::meeting_end_report.read(saved));
    CHECK(keys::meeting_report_ribbon.read(saved));
    CHECK_FALSE(keys::meeting_report_brief.read(saved));
    CHECK_FALSE(keys::meeting_report_signal.read(saved));
    CHECK(keys::developer_mode.read(Json{{"developer_mode", true}}));
    CHECK_FALSE(keys::developer_mode.read(Json{{"developer_mode", "yes"}}));
}

TEST_CASE("TestMeetingSettings::test_retired_direct_agent_resolves_to_pi", "[test_settings]") {
    const Json saved{{"meeting_agent_core", "direct"}};
    CHECK(keys::meeting_agent_core.read(saved) == "pi");
    const auto choices = keys::meeting_agent_core.spec().choices;
    CHECK(std::ranges::find(choices, std::string_view("direct")) == choices.end());
    CHECK(saved.at("meeting_agent_core") == "direct");
}

TEST_CASE("TestMeetingSettings::test_unknown_values_fall_back_to_defaults", "[test_settings]") {
    const Json saved{
        {"meeting_whisper_model", "not-a-model"},
        {"meeting_asr_model", "not-a-model"},
        {"meeting_language", "klingon"},
        {"meeting_agent_core", "not-a-core"},
        {"meeting_speaker_id_backend", "not-a-backend"},
        {"meeting_server_bind", "everywhere"},
    };
    CHECK(resolve_meeting_whisper_model(saved) == resolve_meeting_whisper_model(Json::object()));
    CHECK(keys::meeting_language.read(saved) == "auto");
    CHECK(keys::meeting_agent_core.read(saved) == "pi");
    CHECK(resolve_meeting_speaker_id_backend(saved, fv::FakeClock()) == "local");
    CHECK(keys::meeting_server_bind.read(saved) == "localhost");
    CHECK_FALSE(
        keys::meeting_context_folder_enabled.read(Json{{"meeting_context_folder_enabled", "yes"}}));
    CHECK_FALSE(keys::meeting_unsupported_platform_ack.read(
        Json{{"meeting_unsupported_platform_ack", "yes"}}));
    for (const Json& value : {Json(true), Json("1"), Json(0), Json(2)}) {
        CHECK(keys::meeting_linux_preview_ack_version.read(
                  Json{{"meeting_linux_preview_ack_version", value}}) != 1);
    }
    CHECK_FALSE(keys::meeting_mode_intro_seen.read(Json{{"meeting_mode_intro_seen", "yes"}}));
    CHECK(
        keys::meeting_context_folder_path.read(Json{{"meeting_context_folder_path", 12}}).empty());
}

TEST_CASE("TestMeetingSettings::test_port_is_clamped_and_tolerates_junk", "[test_settings]") {
    CHECK(keys::meeting_server_port.read(Json{{"meeting_server_port", -5}}) == 0);
    CHECK(keys::meeting_server_port.read(Json{{"meeting_server_port", 99999}}) == 65535);
    CHECK(keys::meeting_server_port.read(Json{{"meeting_server_port", "nope"}}) == 0);
}

TEST_CASE("test_a_separate_meeting_engine_with_no_model_saved_takes_dictation_parakeet",
          "[test_settings]") {
    const auto resolve = [](const Json& settings) {
        return own_meeting_speech(settings).model;
    };
    CHECK(resolve(Json::object()) == "parakeet-v3");
    Json choices{{"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}};
    CHECK(resolve(choices) == "orukeet-v0.1");
    // OpenWhisper's Whisper meeting model is not read here, nor a model the catalog lacks.
    Json whisper = choices;
    whisper["meeting_whisper_model"] = "auto";
    CHECK(resolve(whisper) == "orukeet-v0.1");
    Json unknown = choices;
    unknown["meeting_asr_model"] = "nemotron-3.5";
    CHECK(resolve(unknown) == "orukeet-v0.1");
    Json other_engine = choices;
    other_engine["selected_model"] = "parakeet_unified";
    CHECK(resolve(other_engine) == "parakeet-v3");
}

TEST_CASE("meeting_model_label names catalog models and passes other names through",
          "[test_settings]") {
    CHECK(meeting_model_label("parakeet-v3") == "Parakeet TDT 0.6B v3");
    CHECK(meeting_model_label("orukeet-v0.1") == "Orukeet TDT 0.6B");
    CHECK(meeting_model_label("small.en") == "small.en");
}

TEST_CASE("resolve_meeting_asr_source and resolve_meeting_whisper_model view the resolution",
          "[test_settings]") {
    Json settings{{"selected_model", "remote"}, {"meeting_asr_model", "orukeet-v0.1"}};
    CHECK(resolve_meeting_asr_source(settings) == "local");
    CHECK(resolve_meeting_whisper_model(settings) == "orukeet-v0.1");
    settings["meeting_speech_separate"] = true;
    settings["meeting_asr_source"] = "remote";
    CHECK(resolve_meeting_asr_source(settings) == "remote");
}

TEST_CASE("TestSpeakerIdBackend::test_openai_becomes_on_device_after_shutdown",
          "[test_openai_retirement]") {
    const Json settings{{"meeting_speaker_id_backend", "openai"}};
    CHECK(resolve_meeting_speaker_id_backend(settings, fv::FakeClock(local_noon(kShutdown))) ==
          "local");
}

TEST_CASE("TestSpeakerIdBackend::test_off_stays_off_after_shutdown", "[test_openai_retirement]") {
    const Json settings{{"meeting_speaker_id_backend", "off"}};
    CHECK(resolve_meeting_speaker_id_backend(settings, fv::FakeClock(local_noon(kShutdown))) ==
          "off");
}

TEST_CASE("a stored openai speaker backend reads as on-device before the shutdown too",
          "[test_openai_retirement]") {
    const Json settings{{"meeting_speaker_id_backend", "openai"}};
    CHECK(resolve_meeting_speaker_id_backend(
              settings, fv::FakeClock(local_noon(kBeforeShutdown))) == "local");
    CHECK(resolve_meeting_speaker_id_backend(Json{{"meeting_speaker_id_backend", "local"}},
                                             fv::FakeClock(local_noon(kBeforeShutdown))) ==
          "local");
}

TEST_CASE("a failed write leaves the meeting engine undecided", "[test_settings]") {
    Install install(nullptr);
    // A settings file that cannot be parsed blocks writes.
    write_file(install.file, "{not json");
    CHECK_FALSE(keep_separate_meeting_speech(*install.store));
    CHECK(read_file(install.file) == "{not json");
}

TEST_CASE("meeting speech matches Python's", "[golden][test_settings]") {
    golden::for_each("services.settings_meeting", "resolve_meeting_speech",
                     [](const golden::Case& c) {
                         const Json saved = c.arg<Json>(0);
                         const auto actual = resolve_meeting_speech(saved);
                         if (!keys::meeting_speech_separate.read(saved) &&
                             keys::selected_model.read(saved) == "remote") {
                             CHECK(actual.source == "local");
                             CHECK_FALSE(actual.separate);
                         } else {
                             CHECK(golden::equal(c.result<Json>(), speech_json(actual)));
                         }
                     });
    golden::for_each("services.settings_meeting", "own_meeting_speech", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), speech_json(own_meeting_speech(c.arg<Json>(0)))));
    });
    golden::for_each("services.settings_meeting", "meeting_model_label", [](const golden::Case& c) {
        CHECK(meeting_model_label(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("keep_separate_meeting_speech decides as Python does", "[golden][test_settings]") {
    golden::for_each(
        "services.settings_meeting", "keep_separate_meeting_speech", [](const golden::Case& c) {
            const Json saved = c.arg<Json>(0);
            const bool new_install = c.arg<bool>(1);
            // A new install has no settings file when the store opens; what it holds is saved
            // afterwards, as the bootstrap's earlier steps save theirs.
            Install install(new_install ? nullptr : &saved);
            if (new_install && !saved.empty()) {
                install.store->save_all(saved);
            }
            REQUIRE(install.store->is_new_install() == new_install);
            const Json expected = c.result<Json>();
            CHECK(keep_separate_meeting_speech(*install.store) ==
                  expected.at("written").get<bool>());
            CHECK(golden::equal(expected.at("saved"), install.store->value(kSeparate)));
            const auto settings = install.store->snapshot();
            const auto after = resolve_meeting_speech(*settings);
            if (!keys::meeting_speech_separate.read(*settings) &&
                keys::selected_model.read(*settings) == "remote") {
                CHECK(after.source == "local");
                CHECK_FALSE(after.separate);
            } else {
                CHECK(golden::equal(expected.at("after"), speech_json(after)));
            }
        });
}

TEST_CASE("meeting_model_in_use is the meeting model, and none when a paired computer serves it",
          "[test_meeting_speech_follows_dictation]") {
    CHECK(meeting_model_in_use(Json{{"selected_model", "parakeet"}}) == "parakeet-v3");
    const Json remote{{"meeting_speech_separate", true},
                      {"meeting_asr_source", "remote"},
                      {"meeting_asr_model", "orukeet-v0.1"}};
    CHECK(meeting_model_in_use(remote).empty());
    Json local = remote;
    local["meeting_asr_source"] = "local";
    CHECK(meeting_model_in_use(local) == "orukeet-v0.1");
}
