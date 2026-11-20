// Settings → Meeting Mode → Voice & speakers (MeetingVoicePage) and the meeting summaries of
// model_assignments, over an in-memory settings store, a manual worker and a host that records
// what the page tells the window (tests/test_settings_models.py: TestMeetingDestinations,
// TestSharedRuntime, TestMeetingFollowsDictation, TestCleanupSettingsOwnership and the meeting
// half of TestOptionalSpeechSummary).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>

#include "appkit/registries.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "settings/testing/memory_store.h"
#include "ui_downloads/meeting_voice_page.h"
#include "ui_downloads/model_assignments.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/local_model_picker.h"
#include "ui_widgets/wrapped_label.h"
#include "voice_model_support.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using fv::Json;

namespace {

// The window as the page sees it; the rail under Voice & speakers is redrawn from the settings,
// as the dialog's rail is.
class MeetingHost final : public fv::VoiceModelPageHost {
public:
    explicit MeetingHost(fv::SettingsStore& settings) : settings_(settings) {}

    void showMessage(const QString& message) override { messages.push_back(message); }
    void notifyChanged(const QString&) override {}
    void refreshRailValues() override {
        ++redraws;
        rail = fv::MeetingVoicePage::railValue(*settings_.snapshot());
    }
    void focusApiKeys(const QString&) override {}

    std::vector<QString> messages;
    QString rail;
    int redraws = 0;

private:
    fv::SettingsStore& settings_;
};

// One page over fakes; what the speaker model answers is the test's.
class MeetingRig {
public:
    explicit MeetingRig(const Json& saved = Json::object())
        : settings(saved), clock(fvtest::at(2026, 11, 15)), host(settings) {}

    fv::MeetingVoicePageOptions options() {
        fv::MeetingVoicePageOptions made;
        made.speakerModelReady = [this] {
            return modelReady;
        };
        made.speakerDownloadBlocked = [this] {
            return blocked;
        };
        made.speakerModelError = [this] {
            return error;
        };
        made.ensureSpeakerModel = [this] {
            ++downloads;
            modelReady = true;
        };
        made.openRemoteEngine = [this] {
            ++remoteOpened;
        };
        made.clock = &clock;
        made.remoteMeetingSupported = remoteMeetingSupported;
        return made;
    }

    fv::MeetingVoicePage& page() {
        if (page_ == nullptr) {
            page_ = std::make_unique<fv::MeetingVoicePage>(settings, host, io, options());
            host.refreshRailValues();
        }
        return *page_;
    }

    QString rail() { return fv::MeetingVoicePage::railValue(*settings.snapshot()); }
    Json saved(std::string_view key) {
        const auto snapshot = settings.snapshot();
        const auto found = snapshot->find(key);
        return found == snapshot->end() ? Json() : *found;
    }

    bool remoteMeetingSupported = false;
    fv::MemorySettingsStore settings;
    fv::ManualExecutor io;
    fv::FakeClock clock;
    MeetingHost host;
    bool modelReady = false;
    std::string blocked;
    std::string error;
    int downloads = 0;
    int remoteOpened = 0;

private:
    std::unique_ptr<fv::MeetingVoicePage> page_;
};

// _make_meeting_dialog: meetings with an engine of their own.
const Json kSeparate{{"meeting_speech_separate", true}};

QComboBox* pickerCombo(fv::MeetingVoicePage& page) {
    return page.modelPicker()->modelCombo();
}

} // namespace

TEST_CASE("test_meeting_picker_persists_whisper_model", "[test_settings_models][widget]") {
    // OpenWhisper's picker chooses a Whisper size; the meeting picker here lists the catalog's
    // meeting models, saved under meeting_asr_model.
    MeetingRig rig(kSeparate);
    fv::MeetingVoicePage& page = rig.page();
    QComboBox* combo = pickerCombo(page);
    REQUIRE(combo->findData(QStringLiteral("orukeet-v0.1")) >= 0);
    combo->setCurrentIndex(combo->findData(QStringLiteral("orukeet-v0.1")));

    CHECK(rig.saved("meeting_asr_model") == "orukeet-v0.1");
    CHECK(rig.host.rail == QStringLiteral("Orukeet TDT 0.6B · Detect automatically"));
    REQUIRE_FALSE(rig.host.messages.empty());
    CHECK(rig.host.messages.back() ==
          QStringLiteral("Meeting transcription model set to \"orukeet-v0.1\""));
}

TEST_CASE("test_meeting_language_and_core_persist (language half)",
          "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    fv::MeetingVoicePage& page = rig.page();
    QComboBox* language = page.languageCombo();
    language->setCurrentIndex(language->findData(QStringLiteral("en")));
    CHECK(rig.saved("meeting_language") == "en");
    CHECK_THAT(rig.host.rail.toStdString(), EndsWith("· English"));
    CHECK_THAT(language->toolTip().toStdString(), StartsWith("Choose the meeting language"));
}

TEST_CASE("test_speaker_id_combo_includes_off_and_persists", "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    fv::MeetingVoicePage& page = rig.page();
    QComboBox* combo = page.speakerCombo();
    QStringList backends;
    for (int i = 0; i < combo->count(); ++i) {
        backends.append(combo->itemData(i).toString());
    }
    CHECK(backends ==
          QStringList{QStringLiteral("off"), QStringLiteral("local"), QStringLiteral("openai")});
    // The OpenAI pass is not in this build: listed, and not to be chosen.
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    REQUIRE(model != nullptr);
    CHECK(model->item(2)->text() == QStringLiteral("OpenAI (retired)"));
    CHECK_FALSE(model->item(2)->isEnabled());
    CHECK(model->item(0)->isEnabled());

    combo->setCurrentIndex(combo->findData(QStringLiteral("off")));
    CHECK(rig.saved("meeting_speaker_id_backend") == "off");
    const std::string status = page.speakerStatus()->text().toStdString();
    CHECK_THAT(status, ContainsSubstring("Me"));
    CHECK_THAT(status, ContainsSubstring("Others"));
    CHECK_THAT(status, ContainsSubstring("No on-device model"));
    CHECK(page.speakerDownloadButton()->isHidden());
}

TEST_CASE("test_openai_consent_decline_restores_previous_off", "[test_settings_models][widget]") {
    // OpenWhisper asks for consent and puts the previous choice back when it is declined; here
    // the retired choice is refused the same way, without asking.
    MeetingRig rig({{"meeting_speech_separate", true}, {"meeting_speaker_id_backend", "off"}});
    fv::MeetingVoicePage& page = rig.page();
    QComboBox* combo = page.speakerCombo();
    REQUIRE(combo->currentData().toString() == QStringLiteral("off"));
    const std::size_t writes = rig.settings.file().writes();

    combo->setCurrentIndex(combo->findData(QStringLiteral("openai")));

    CHECK(combo->currentData().toString() == QStringLiteral("off"));
    CHECK(rig.saved("meeting_speaker_id_backend") == "off");
    CHECK(rig.settings.file().writes() == writes);
}

TEST_CASE("a stored OpenAI speaker pass shows as on-device", "[widget]") {
    MeetingRig rig({{"meeting_speaker_id_backend", "openai"}});
    fv::MeetingVoicePage& page = rig.page();
    CHECK(page.speakerCombo()->currentData().toString() == QStringLiteral("local"));
    CHECK_THAT(page.speakerStatus()->text().toStdString(), ContainsSubstring("WeSpeaker"));
    // Loading wrote nothing back over the stored choice.
    CHECK(rig.saved("meeting_speaker_id_backend") == "openai");
}

TEST_CASE("test_speaker_id_status_explains_first_meeting_download",
          "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    fv::MeetingVoicePage& page = rig.page();
    page.refreshSpeakerStatus();
    const std::string status = page.speakerStatus()->text().toStdString();
    CHECK_THAT(status, ContainsSubstring("Hugging Face"));
    CHECK_THAT(status, ContainsSubstring("next meeting"));
    CHECK_THAT(status, !ContainsSubstring("Downloads"));
    CHECK_THAT(status, ContainsSubstring("system audio only"));
    CHECK_FALSE(page.speakerDownloadButton()->isHidden());
}

TEST_CASE("test_speaker_id_status_reports_a_cached_model", "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    rig.modelReady = true;
    fv::MeetingVoicePage& page = rig.page();
    page.refreshSpeakerStatus();
    CHECK_THAT(page.speakerStatus()->text().toStdString(), ContainsSubstring("is ready"));
    CHECK(page.speakerDownloadButton()->isHidden());
}

TEST_CASE("test_speaker_id_status_shows_the_last_failure_with_a_retry",
          "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    rig.error = "Could not reach Hugging Face to download the speaker model (network error).";
    fv::MeetingVoicePage& page = rig.page();
    page.refreshSpeakerStatus();
    CHECK_THAT(page.speakerStatus()->text().toStdString(),
               ContainsSubstring("The last download failed. " + rig.error));
    CHECK_FALSE(page.speakerDownloadButton()->isHidden());
}

TEST_CASE("test_speaker_id_status_explains_a_blocked_download_without_a_retry",
          "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    rig.blocked =
        "Hugging Face access is set to Never connect, so the speaker model can't download.";
    fv::MeetingVoicePage& page = rig.page();
    page.refreshSpeakerStatus();
    CHECK_THAT(page.speakerStatus()->text().toStdString(), ContainsSubstring(rig.blocked));
    CHECK(page.speakerDownloadButton()->isHidden());
}

TEST_CASE("test_download_now_fetches_the_speaker_model_off_the_qt_thread",
          "[test_settings_models][widget]") {
    MeetingRig rig(kSeparate);
    fv::MeetingVoicePage& page = rig.page();
    page.refreshSpeakerStatus();
    REQUIRE_FALSE(page.speakerDownloadButton()->isHidden());
    page.speakerDownloadButton()->click();

    // Nothing ran on this thread: the download waits for the worker.
    CHECK(rig.downloads == 0);
    CHECK(rig.io.queued() == 1);
    CHECK_THAT(page.speakerStatus()->text().toStdString(), StartsWith("Downloading"));
    CHECK(page.isDownloading());

    rig.io.run_all();
    CHECK(rig.downloads == 1);
    QApplication::processEvents();
    CHECK_FALSE(page.isDownloading());
    CHECK_THAT(page.speakerStatus()->text().toStdString(), ContainsSubstring("is ready"));
    CHECK(page.speakerDownloadButton()->isHidden());
}

TEST_CASE("a download that answers after the page closed reaches nothing", "[widget]") {
    MeetingRig rig(kSeparate);
    auto page =
        std::make_unique<fv::MeetingVoicePage>(rig.settings, rig.host, rig.io, rig.options());
    page->downloadSpeakerModel();
    page.reset();
    rig.io.run_all();
    QApplication::processEvents();
    CHECK(rig.downloads == 1);
}

TEST_CASE("test_meeting_voice_names_the_shared_runtime_destination",
          "[test_settings_models][widget]") {
    // OpenWhisper's separate Whisper model names the Runtime page's device and quantization;
    // every meeting model here is a catalog model, whose own device preference is named.
    MeetingRig rig(kSeparate);
    fv::MeetingVoicePage& page = rig.page();
    CHECK(page.runtimeLabel()->text() ==
          QStringLiteral("Uses Parakeet TDT 0.6B v3 with its Auto device preference. Set that "
                         "engine's device on Dictation → Voice model. Install its runtime and "
                         "model in Downloads."));
    rig.settings.save_setting("local_asr_devices", Json{{"parakeet", "cuda"}});
    QApplication::processEvents();
    CHECK_THAT(page.runtimeLabel()->text().toStdString(), ContainsSubstring("its GPU device"));
}

TEST_CASE("test_following_shows_one_choice_and_names_the_engine",
          "[test_settings_models][widget]") {
    MeetingRig rig({{"selected_model", "parakeet"}});
    fv::MeetingVoicePage& page = rig.page();
    CHECK(page.engineCombo()->currentData().toBool() == false);
    CHECK(page.sourceField()->isHidden());
    CHECK(page.localModelField()->isHidden());
    CHECK(page.remoteControls()->isHidden());
    CHECK(page.runtimeLabel()->text() ==
          QStringLiteral("Meetings use Parakeet TDT 0.6B v3, the same as dictation. Change the "
                         "dictation engine on Dictation → Voice model."));
}

TEST_CASE("test_choosing_a_different_engine_reveals_and_saves_it",
          "[test_settings_models][widget]") {
    MeetingRig rig({{"selected_model", "parakeet"}});
    fv::MeetingVoicePage& page = rig.page();
    QComboBox* combo = page.engineCombo();
    combo->setCurrentIndex(combo->findData(true));

    CHECK(rig.saved("meeting_speech_separate") == true);
    // Where a meeting runs is not a choice in this build (remote meeting speech is M9's).
    CHECK(page.sourceField()->isHidden());
    CHECK_FALSE(page.localModelField()->isHidden());
    CHECK(rig.host.rail == QStringLiteral("Parakeet TDT 0.6B v3 · Detect automatically"));

    QComboBox* picker = pickerCombo(page);
    picker->setCurrentIndex(picker->findData(QStringLiteral("orukeet-v0.1")));
    CHECK(rig.host.rail == QStringLiteral("Orukeet TDT 0.6B · Detect automatically"));

    // Back to following: the saved choice stays for next time.
    combo->setCurrentIndex(combo->findData(false));
    CHECK(rig.saved("meeting_speech_separate") == false);
    CHECK(rig.saved("meeting_asr_model") == "orukeet-v0.1");
    CHECK(page.localModelField()->isHidden());
    CHECK(rig.host.rail == QStringLiteral("Parakeet TDT 0.6B v3 · Detect automatically"));
}

TEST_CASE("test_a_following_meeting_tracks_a_change_of_dictation_engine",
          "[test_settings_models][widget]") {
    MeetingRig rig({{"selected_model", "parakeet"}});
    fv::MeetingVoicePage& page = rig.page();
    // The Voice model page saves dictation's model; the meeting follows it.
    rig.settings.save_setting("local_asr_models", Json{{"parakeet", "orukeet-v0.1"}});
    QApplication::processEvents();
    CHECK_THAT(rig.rail().toStdString(), StartsWith("Orukeet"));
    CHECK_THAT(page.runtimeLabel()->text().toStdString(),
               ContainsSubstring("the same as dictation"));
    CHECK_THAT(page.runtimeLabel()->text().toStdString(), StartsWith("Meetings use Orukeet"));
}

TEST_CASE("test_an_engine_that_cannot_run_meetings_says_what_meetings_use",
          "[test_settings_models][widget]") {
    MeetingRig rig({{"selected_model", "api"}});
    fv::MeetingVoicePage& page = rig.page();
    CHECK_THAT(page.runtimeLabel()->text().toStdString(),
               StartsWith("The API engine can't transcribe meetings, so meetings use "));
}

TEST_CASE("test_saving_meeting_settings_preserves_model_keys", "[test_settings_models][widget]") {
    const Json profiles = Json::array({Json{{"id", "custom_abcd1234"},
                                            {"name", "LM Studio"},
                                            {"base_url", "http://127.0.0.1:1234/v1"},
                                            {"api_key_env", ""}}});
    MeetingRig rig({{"meeting_speech_separate", true},
                    {"meeting_asr_model", "orukeet-v0.1"},
                    {"meeting_llm_provider", "openai"},
                    {"meeting_llm_model", "gpt-4o-mini"},
                    {"meeting_language", "fr"},
                    {"meeting_agent_core", "pi"},
                    {"meeting_speaker_id_backend", "openai"},
                    {"text_llm_profiles", profiles}});
    fv::MeetingVoicePage& page = rig.page();
    CHECK(rig.host.rail == QStringLiteral("Orukeet TDT 0.6B · French"));
    // A stored OpenAI pass reads as on-device until that pass exists (M7.21).
    CHECK_FALSE(fv::speaker_id_is_remote(*rig.settings.snapshot(), rig.clock));
    page.languageCombo()->setCurrentIndex(page.languageCombo()->findData(QStringLiteral("de")));

    const Json saved = rig.settings.contents();
    CHECK(saved["meeting_asr_model"] == "orukeet-v0.1");
    CHECK(saved["meeting_llm_provider"] == "openai");
    CHECK(saved["meeting_llm_model"] == "gpt-4o-mini");
    CHECK(saved["meeting_language"] == "de");
    CHECK(saved["meeting_agent_core"] == "pi");
    CHECK(saved["meeting_speaker_id_backend"] == "openai");
    CHECK(saved["text_llm_profiles"][0]["id"] == "custom_abcd1234");
}

TEST_CASE("test_meeting_voice_detail_shows_off_speaker_id", "[test_settings_models]") {
    CHECK_THAT(fv::meeting_voice_detail({{"meeting_speaker_id_backend", "off"}}),
               ContainsSubstring("Me / Others labels"));
}

TEST_CASE("test_rail_reports_optional_family_and_meeting_model (meeting half)",
          "[test_settings_models]") {
    const Json settings{{"selected_model", "parakeet"},
                        {"meeting_speech_separate", true},
                        {"meeting_asr_model", "orukeet-v0.1"}};
    CHECK(fv::MeetingVoicePage::railValue(settings) ==
          QStringLiteral("Orukeet TDT 0.6B · Detect automatically"));
}

TEST_CASE("the meeting summaries name the model, language and speaker labels", "[unit]") {
    CHECK(fv::speaker_id_label("off") == "Me / Others labels");
    CHECK(fv::speaker_id_label("local") == "On-device speaker labels");
    CHECK(fv::speaker_id_label("openai") == "OpenAI speaker labels");
    CHECK(fv::meeting_language_label("auto") == "Detect automatically");
    CHECK(fv::meeting_language_label("pt") == "Portuguese");
    CHECK(fv::meeting_language_label("tlh") == "tlh");
    CHECK(fv::meeting_model_label({{"selected_model", "remote"}}) == "Parakeet TDT 0.6B v3");
    CHECK(fv::meeting_voice_detail({{"selected_model", "remote"}}) ==
          "Detect automatically · On-device speaker labels");
    CHECK(fv::meeting_voice_summary({{"meeting_language", "ja"}}) ==
          "Parakeet TDT 0.6B v3 · Japanese");
    const Json paired{{"selected_model", "remote"},
                      {"remote_engine_client", Json{{"host", "192.168.1.20"},
                                                    {"port", 47821},
                                                    {"fingerprint", std::string(64, 'a')},
                                                    {"host_name", "Studio PC"}}}};
    CHECK_THAT(fv::meeting_runtime_sentence(paired),
               StartsWith("Meeting speech stays on this computer"));
    auto enabled = paired;
    enabled["meeting_speech_separate"] = true;
    enabled["meeting_asr_source"] = "remote";
    enabled["fv_meeting_remote_fingerprint"] = std::string(64, 'a');
    CHECK_THAT(fv::meeting_runtime_sentence(enabled),
               StartsWith("Studio PC is your preferred host for meeting speech"));
    CHECK_THAT(fv::meeting_runtime_sentence(
                   {{"meeting_speech_separate", true}, {"meeting_asr_source", "remote"}}),
               StartsWith("Confirm the paired host for meeting speech"));
}

TEST_CASE("the speaker model status says what the model is doing", "[unit]") {
    CHECK(fv::speaker_model_status(true, true, "blocked", "error").text ==
          "Downloading on-device WeSpeaker (voxceleb_resnet34_LM.onnx, about 26 MB) from Hugging "
          "Face…");
    CHECK_FALSE(fv::speaker_model_status(false, true, "", "").offerDownload);
    CHECK(fv::speaker_model_status(false, false, "Blocked.", "x").text ==
          "Blocked. Meetings use Me/Others channel labels.");
    CHECK(fv::speaker_model_status(false, false, "", "Failed.").offerDownload);
    CHECK(fv::speaker_model_status(false, false, "", "").offerDownload);
}

TEST_CASE("a failed save of the meeting engine says so and shows the saved choice", "[widget]") {
    MeetingRig rig({{"selected_model", "parakeet"}});
    fv::MeetingVoicePage& page = rig.page();
    rig.settings.file().fail_next_replace("Disk full");
    page.engineCombo()->setCurrentIndex(page.engineCombo()->findData(true));
    REQUIRE_FALSE(rig.host.messages.empty());
    CHECK(rig.host.messages.back() ==
          QStringLiteral("Couldn't save the meeting speech engine: Disk full"));
    CHECK(page.engineCombo()->currentData().toBool() == false);
    CHECK(page.localModelField()->isHidden());
}

TEST_CASE("the remote rows stay hidden whatever is saved", "[widget]") {
    MeetingRig rig({{"meeting_speech_separate", true}, {"meeting_asr_source", "remote"}});
    fv::MeetingVoicePage& page = rig.page();
    CHECK(page.remoteControls()->isHidden());
    CHECK(page.remoteStatus()->isHidden());
    CHECK(page.localModelField()->isHidden());
    CHECK_THAT(page.runtimeLabel()->text().toStdString(),
               StartsWith("Confirm the paired host for meeting speech"));
    CHECK(rig.rail() == QStringLiteral("Remote computer · Detect automatically"));

    // A saved remote source (an imported OpenWhisper setting) is not a dead end: "Runs on" shows
    // so the meetings can come back to this computer, and then it goes again.
    CHECK_FALSE(page.sourceField()->isHidden());
    page.sourceCombo()->setCurrentIndex(page.sourceCombo()->findData(QStringLiteral("local")));
    CHECK(rig.saved("meeting_asr_source") == "local");
    CHECK(page.sourceField()->isHidden());
    CHECK_FALSE(page.localModelField()->isHidden());
    CHECK(rig.host.rail == QStringLiteral("Parakeet TDT 0.6B v3 · Detect automatically"));
}

TEST_CASE("Runs on stays hidden for a following meeting with a saved remote source", "[widget]") {
    MeetingRig rig({{"meeting_speech_separate", false}, {"meeting_asr_source", "remote"}});
    fv::MeetingVoicePage& page = rig.page();
    CHECK(page.sourceField()->isHidden());
    CHECK(page.localModelField()->isHidden());
}

TEST_CASE("the Voice & speakers page registers as DownloadsFeature registers it", "[widget]") {
    fvtest::WindowRig window({{"meeting_language", "it"}});
    fv::registerMeetingVoicePage(window.context(), [] { return fv::MeetingVoicePageOptions{}; });
    const fv::SettingsPageSpec* spec =
        window.context().settingsPages().page(fv::destinations::meeting_voice);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("Voice & speakers"));
    CHECK(spec->railValue(*window.settings().snapshot()) ==
          QStringLiteral("Parakeet TDT 0.6B v3 · Italian"));
    CHECK(spec->searchFields.size() == 5);
    CHECK(window.context().settingsPages().controlDestination("speaker_id_status") ==
          fv::destinations::meeting_voice);
    fv::SettingsDialog dialog(window.context());
    fv::SettingsPage* page = dialog.ensurePage(fv::destinations::meeting_voice);
    REQUIRE(page != nullptr);
    auto* content =
        dynamic_cast<fv::MeetingVoicePage*>(dialog.pageContent(fv::destinations::meeting_voice));
    REQUIRE(content != nullptr);
    CHECK(content->languageCombo()->currentData().toString() == QStringLiteral("it"));
    CHECK(dialog.control("meeting_speaker_id_combo") == content->speakerCombo());
}

TEST_CASE("Meeting audio consent belongs to one paired host and can be cleared", "[widget]") {
    const std::string fingerprint(64, 'a');
    const Json pairing{{"host", "127.0.0.1"},
                       {"port", 47821},
                       {"fingerprint", fingerprint},
                       {"host_name", "Synthetic studio"}};
    MeetingRig rig({{"remote_engine_client", pairing}, {"selected_model", "remote"}});
    rig.remoteMeetingSupported = true;
    auto& page = rig.page();
    auto* choose = page.findChild<QPushButton*>(QStringLiteral("meetingChoosePairedHost"));
    auto* local = page.findChild<QPushButton*>(QStringLiteral("meetingUseLocalSpeech"));
    REQUIRE(choose);
    REQUIRE(local);
    CHECK_FALSE(choose->isHidden());
    CHECK(rig.saved("fv_meeting_remote_fingerprint").is_null());

    page.runRemoteConfirmation = [](QMessageBox& box) {
        CHECK(box.text().contains(QStringLiteral("captured microphone and system audio")));
        CHECK(box.text().contains(QStringLiteral("Synthetic studio")));
        CHECK(box.defaultButton()->text().contains(QStringLiteral("Cancel")));
        return QMessageBox::Cancel;
    };
    choose->click();
    CHECK(rig.saved("fv_meeting_remote_fingerprint").is_null());

    page.runRemoteConfirmation = [](QMessageBox&) {
        return QMessageBox::Yes;
    };
    choose->click();
    CHECK(rig.saved("meeting_speech_separate") == true);
    CHECK(rig.saved("meeting_asr_source") == "remote");
    CHECK(rig.saved("fv_meeting_remote_fingerprint") == fingerprint);

    Json replacement = pairing;
    replacement["fingerprint"] = std::string(64, 'b');
    rig.settings.save_setting("remote_engine_client", replacement);
    page.refreshPage();
    CHECK_FALSE(choose->isHidden());
    CHECK(page.runtimeLabel()->text().contains(QStringLiteral("Confirm the paired host")));
    CHECK(rig.saved("fv_meeting_remote_fingerprint") == fingerprint);

    local->click();
    CHECK(rig.saved("meeting_asr_source") == "local");
    CHECK(rig.saved("fv_meeting_remote_fingerprint").is_null());
}

namespace {

// A computer paired with "Synthetic studio".
Json pairedStudio() {
    return Json{{"host", "127.0.0.1"},
                {"port", 47821},
                {"fingerprint", std::string(64, 'a')},
                {"host_name", "Synthetic studio"}};
}

} // namespace

// from
// tests/test_settings_models.py::TestMeetingFollowsDictation::test_use_for_meetings_gives_meetings_the_paired_computer
TEST_CASE("test_use_for_meetings_gives_meetings_the_paired_computer",
          "[test_settings_models][widget]") {
    MeetingRig rig({{"remote_engine_client", pairedStudio()}, {"selected_model", "parakeet"}});
    rig.remoteMeetingSupported = true;
    auto& page = rig.page();
    // FioriVoce: the paired computer receives meeting audio only once its owner confirms it for
    // this host (a25-remote-meeting-consent).
    page.runRemoteConfirmation = [](QMessageBox&) {
        return QMessageBox::Yes;
    };
    page.findChild<QPushButton*>(QStringLiteral("meetingChoosePairedHost"))->click();
    CHECK(rig.saved("meeting_speech_separate") == true);
    CHECK(rig.saved("meeting_asr_source") == "remote");
    CHECK(page.engineCombo()->currentData().toBool() == true);
    CHECK(page.sourceCombo()->currentData().toString() == QStringLiteral("remote"));
    CHECK(rig.rail().startsWith(QStringLiteral("Remote computer")));
}

// from
// tests/test_settings_models.py::test_meeting_remote_source_is_independent_and_preserves_local_model
TEST_CASE("test_meeting_remote_source_is_independent_and_preserves_local_model",
          "[test_settings_models][widget]") {
    const auto models = fv::LocalModelPicker::catalogModels(true);
    REQUIRE_FALSE(models.empty());
    const std::string own = models.back().key.toStdString();
    MeetingRig rig({{"remote_engine_client", pairedStudio()},
                    {"selected_model", "parakeet"},
                    {"meeting_speech_separate", true},
                    {"meeting_asr_model", own}});
    rig.remoteMeetingSupported = true;
    auto& page = rig.page();
    CHECK(page.sourceCombo()->currentData().toString() == QStringLiteral("local"));
    CHECK_FALSE(page.localModelField()->isHidden());
    page.runRemoteConfirmation = [](QMessageBox&) {
        return QMessageBox::Yes;
    };
    page.findChild<QPushButton*>(QStringLiteral("meetingChoosePairedHost"))->click();
    CHECK(page.localModelField()->isHidden());
    CHECK_FALSE(page.remoteControls()->isHidden());
    // Dictation's engine and the meetings' own model are left as they were.
    CHECK(rig.saved("selected_model") == "parakeet");
    CHECK(rig.saved("meeting_asr_model") == own);

    // Reopened, the page shows the paired computer; back on this computer, the model is there.
    fv::MeetingVoicePage reopened(rig.settings, rig.host, rig.io, rig.options());
    CHECK(reopened.sourceCombo()->currentData().toString() == QStringLiteral("remote"));
    reopened.findChild<QPushButton*>(QStringLiteral("meetingUseLocalSpeech"))->click();
    CHECK(rig.saved("meeting_asr_source") == "local");
    CHECK(reopened.modelPicker()->currentModel().toStdString() == own);
}

// from
// tests/test_settings_models.py::test_meeting_remote_settings_link_and_unpaired_connection_check
TEST_CASE("test_meeting_remote_settings_link_and_unpaired_connection_check",
          "[test_settings_models][widget]") {
    MeetingRig rig({{"meeting_speech_separate", true}, {"meeting_asr_source", "remote"}});
    rig.remoteMeetingSupported = true;
    auto& page = rig.page();
    page.findChild<QPushButton*>(QStringLiteral("meetingConfigureRemoteButton"))->click();
    CHECK(rig.remoteOpened == 1);
    // Unpaired, the page says so at once: FioriVoce offers Test connection only for a confirmed
    // host, so there is no handshake to run first.
    CHECK(page.remoteTestButton()->isHidden());
    CHECK(page.remoteStatus()->text().contains(QStringLiteral("Pair a computer")));
    CHECK_FALSE(page.remoteStatus()->isHidden());
}
