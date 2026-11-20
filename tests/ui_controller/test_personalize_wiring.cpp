// The controller's share of the M4 text features (application_controller.py's command keys,
// profile shortcuts, settings dispatch and transcribe_clip; ui_controller.py's learned-word notice,
// profile start and cleanup callbacks), over the real feature graph offscreen with fakes. No key
// is read from this computer, no hook is installed and no network is reached.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QLabel>
#include <QSignalSpy>
#include <QTest>

#include "appcore/activity.h"
#include "appcore/shutdown_plan.h"
#include "appkit/destinations.h"
#include "base/utf8.h"
#include "dictation/command_runtime.h"
#include "dictation/dictionary_learning.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/transcription_runtime.h"
#include "hotkeys/hotkey_manager.h"
#include "llm/transcript_cleanup.h"
#include "personalize_support.h"
#include "settings/keys.h"
#include "speech/service.h"
#include "text/cleanup_profiles.h"
#include "text/dictionary.h"
#include "ui_controller/backup_lifecycle.h"
#include "ui_controller/personalize_wiring.h"
#include "ui_controller/rule_transcriber.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/cleanup_rules_section.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_personalize/rule_dictation.h"
#include "ui_personalize/rules_page.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_settings/app_pages.h"
#include "ui_setup/cleanup_provider_row.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;
using Json = fv::Json;

namespace {

namespace dest = fv::destinations;

std::filesystem::path toneClip() {
    return ControllerRig::testkit_audio("tone-44k1-mono.wav");
}

// transcribeClip off the GUI thread: (text, error).
std::pair<std::string, std::string> transcribe(fv::RuleTranscriber& transcriber) {
    return fvtest::offGui([&transcriber]() -> std::pair<std::string, std::string> {
        try {
            return {transcriber.transcribeClip(toneClip()), {}};
        } catch (const std::exception& error) {
            return {{}, error.what()};
        }
    });
}

std::vector<std::string> lastStatuses(const ControllerRig& rig, std::size_t count) {
    const std::vector<std::string> all = rig.statuses();
    if (all.size() <= count) {
        return all;
    }
    return {all.end() - static_cast<std::ptrdiff_t>(count), all.end()};
}

} // namespace

// ---- tests/test_application_controller.py -------------------------------------------------

TEST_CASE("test_profile_shortcuts_refresh_and_capture_preserve_standard_hotkeys",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    REQUIRE(rig.startEngine());
    fv::text::CleanupProfile profile = fv::text::starter_profiles().front();
    profile.hotkey = "ctrl+alt+t";
    rig.settings().save_setting("transcript_cleanup_profiles",
                                Json::array({fv::text::profile_to_json(profile)}));
    rig.hotkeys().refresh_profile_hotkeys();

    auto& manager = rig.context().service<fv::hotkeys::HotkeyManager>();
    CHECK(manager.dynamic_hotkeys() ==
          std::map<std::string, std::string>{{"profile:support-ticket", "ctrl+alt+t"}});
    const std::map<std::string, std::string> standard = manager.hotkeys();
    CHECK(standard.at("record_toggle") ==
          fv::load_hotkey_settings(rig.settings()).at("record_toggle").get<std::string>());
    rig.hotkeys().set_capture_suspended(true);
    CHECK(manager.capture_suspended());
    rig.hotkeys().set_capture_suspended(false);

    // The profile's shortcut (the manager's family callback) starts a recording with it.
    rig.hotkeys().profile_record_requested.emit("support-ticket");
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    CHECK(rig.runtime().recording_profile() == "support-ticket");
    // Quick Record shows the profile the recording uses (on_dictation_started).
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("support-ticket"));

    rig.settings().save_setting("transcript_cleanup_profiles", Json::array());
    rig.hotkeys().refresh_profile_hotkeys();
    CHECK(manager.dynamic_hotkeys().empty());
    CHECK(manager.hotkeys() == standard);
    rig.app().cancel();
    CHECK(waitUntil([&rig] { return !rig.runtime().is_recording(); }));
}

TEST_CASE("test_transcribe_clip_delegates_to_current_backend", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    fvtest::RigOptions options = fvtest::personalizeOptions(keys, nullptr);
    options.script.texts = {"clip transcript"};
    ControllerRig rig(std::move(options));
    REQUIRE(rig.startEngine());
    auto& transcriber = rig.context().service<fv::RuleTranscriber>();

    const auto [text, error] = transcribe(transcriber);
    CHECK(error.empty());
    CHECK(text == "clip transcript");
    // Through the speech service, on the dictation engine's Final lane.
    bool decoded = false;
    for (const fv::speech::FakeCall& call : rig.engine().calls()) {
        decoded = decoded || (call.op == "decode" && call.lane == fv::speech::Lane::Final);
    }
    CHECK(decoded);
}

TEST_CASE("test_transcribe_clip_raises_without_backend_or_when_busy", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    auto& transcriber = rig.context().service<fv::RuleTranscriber>();

    // Nothing is loaded before the window shows.
    CHECK(transcribe(transcriber).second == "No transcription engine is available");

    REQUIRE(rig.startEngine());
    // A job holds the engine.
    std::atomic<bool> busy{true};
    fv::RuleTranscriberPorts ports;
    ports.activity = &rig.activity();
    ports.speech = &rig.speech();
    ports.busy = [&busy] {
        return busy.load();
    };
    fv::ControllerRuleTranscriber held(ports);
    CHECK(transcribe(held).second == "Transcription engine is busy");
    busy = false;
    CHECK(transcribe(held).second.empty());
    // The controller's transcriber asks the application controller.
    CHECK_FALSE(rig.app().is_transcribing());
    CHECK(transcribe(transcriber).second.empty());
}

TEST_CASE("test_transcribe_clip_is_refused_during_a_meeting", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    REQUIRE(rig.startEngine());
    auto& transcriber = rig.context().service<fv::RuleTranscriber>();
    const std::size_t decodes = rig.engine().calls().size();

    rig.activity().set_meeting_active(true);
    CHECK(transcribe(transcriber).second ==
          "Meeting Mode is active — end the meeting to use dictation");
    // A host's update and a backup's preparation come first, in that order.
    rig.activity().set_host_update_active(true);
    CHECK(transcribe(transcriber).second ==
          std::string(fv::identity().product) + " is preparing an update. Try again shortly.");
    rig.activity().set_backup_in_progress(true);
    CHECK(transcribe(transcriber).second == "Wait for backup or restore preparation to finish");
    CHECK(rig.engine().calls().size() == decodes);
    rig.activity().set_backup_in_progress(false);
    rig.activity().set_host_update_active(false);
    rig.activity().set_meeting_active(false);
}

TEST_CASE("test_backup_waits_for_a_rule_dictation_in_settings", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(
        fvtest::personalizeOptions(keys, nullptr, Json{{"transcript_cleanup_enabled", true}}));
    REQUIRE(rig.startEngine());
    fv::BackupLifecycle& backup = *rig.feature().backupLifecycle();
    fv::SettingsDialog& dialog = fvtest::settingsDialog(rig);
    dialog.selectDestination(QString::fromUtf8(dest::cleanup_rules.data()));
    auto* page = dynamic_cast<fv::RulesPage*>(dialog.pageContent(dest::cleanup_rules));
    REQUIRE(page != nullptr);
    fv::CleanupRulesSection& rules = *page->section();
    rules.setDictationPath(rig.paths().data_root() / "rule_dictation.wav");

    rules.toggleDictation();
    REQUIRE(rules.dictationState() == fv::CleanupRulesSection::DictationState::Recording);
    const std::optional<std::string> refused = backup.acquire();
    REQUIRE(refused.has_value());
    CHECK(refused->find("cleanup rule") != std::string::npos);
    CHECK_FALSE(rig.activity().snapshot().backup_in_progress);

    rules.cancelDictation();
    REQUIRE(rules.dictationState() == fv::CleanupRulesSection::DictationState::Idle);
    CHECK_FALSE(backup.acquire().has_value());
    backup.release();
}

// ---- tests/test_personalize_foundation_controller.py ---------------------------------------

TEST_CASE("test_command_keys_and_transforms_reach_the_command_runtime", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    auto http = fv::http::make_curl_client(fv::http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    // The endpoint's key is in no environment, so Command Mode is not set up.
    ControllerRig rig(fvtest::personalizeOptions(
        keys, http.get(),
        fvtest::customEndpoint("http://127.0.0.1:9/v1", "m", "FIORIVOCE_TEST_ABSENT_KEY")));
    REQUIRE(rig.startEngine());
    fv::PersonalizeWiring& wiring = *rig.feature().personalizeWiring();
    REQUIRE(wiring.commandRuntime() != nullptr);
    CHECK(rig.context().find<fv::dictation::CommandRuntime>() == wiring.commandRuntime());

    // The hook's Command Mode press and release go through the dispatcher to the runtime.
    rig.hotkeys().queue_command_press(1.5);
    rig.hotkeys().queue_command_release(2.0);
    CHECK(waitUntil([&rig] { return rig.hasStatus("Set up AI cleanup to use Command Mode"); }));
    CHECK_FALSE(rig.runtime().is_recording());
}

TEST_CASE("test_commands_and_transforms_without_a_provider_say_how_to_start",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    auto http = fv::http::make_curl_client(fv::http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    ControllerRig rig(fvtest::personalizeOptions(
        keys, http.get(),
        fvtest::customEndpoint("http://127.0.0.1:9/v1", "m", "FIORIVOCE_TEST_ABSENT_KEY")));
    REQUIRE(rig.startEngine());

    rig.hotkeys().transform_requested.emit("polish");
    REQUIRE(waitUntil([&rig] { return rig.hasStatus("Set up AI cleanup to use transforms"); }));
    rig.hotkeys().queue_command_press(1.0);
    REQUIRE(waitUntil([&rig] { return rig.hasStatus("Set up AI cleanup to use Command Mode"); }));

    CHECK(lastStatuses(rig, 2) ==
          std::vector<std::string>{"Set up AI cleanup to use transforms",
                                   "Set up AI cleanup to use Command Mode"});
    CHECK_FALSE(rig.runtime().is_recording());
    fv::dictation::CommandRuntime& command = *rig.feature().personalizeWiring()->commandRuntime();
    try {
        command.complete_recording("  ", nullptr);
        FAIL("an empty instruction was accepted");
    } catch (const fv::dictation::CommandRefused& refused) {
        CHECK(std::string(refused.what()).find("Didn't catch an instruction") != std::string::npos);
    }
}

TEST_CASE("test_settings_changes_refresh_shortcuts_for_transforms_and_hotkeys",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    int refreshes = 0;
    rig.hotkeys().set_transform_loader([&refreshes](const Json&) {
        ++refreshes;
        return std::vector<fv::text::Transform>{};
    });

    for (const char* kind :
         {"transforms", "hotkeys", "dictionary", "snippets", "styles", "languages"}) {
        rig.ui().settingsPageChanged(QString::fromLatin1(kind));
    }
    CHECK(refreshes == 2);
    // "actions" (M5's saved actions) registers as global shortcuts too.
    rig.ui().settingsPageChanged(QStringLiteral("actions"));
    CHECK(refreshes == 3);
}

// ---- tests/test_personalize_foundation_ui_shell.py -----------------------------------------

TEST_CASE("test_learned_term_says_so_and_refreshes_the_dictionary_page", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    fv::dictation::DictionaryLearner& learner =
        rig.context().service<fv::dictation::DictionaryLearner>();

    // The learner emits on the persistence strand; the notice is the UI thread's status line.
    learner.term_learned.emit("Kubernetes", false);
    REQUIRE(waitUntil([&rig] {
        return rig.window().status() == QStringLiteral("Added \"Kubernetes\" to your dictionary");
    }));
    learner.term_learned.emit("Alex Rivera", true);
    REQUIRE(waitUntil([&rig] {
        return rig.window().status() ==
               QStringLiteral("AI cleanup will keep \"Alex Rivera\" as you said it");
    }));

    // With Settings open, the Dictionary page reads the library again.
    fv::SettingsDialog& dialog = fvtest::settingsDialog(rig);
    dialog.selectDestination(QString::fromUtf8(dest::dictionary.data()));
    auto* page = dynamic_cast<fv::DictionaryPage*>(dialog.pageContent(dest::dictionary));
    REQUIRE(page != nullptr);
    const QString before = page->count()->text();
    Json words = Json::array({Json{{"id", "kubernetes"},
                                   {"term", "Kubernetes"},
                                   {"starred", false},
                                   {"heard", Json::array()},
                                   {"learned", true},
                                   {"new", true}}});
    rig.settings().save_setting("dictation_dictionary", words);
    learner.term_learned.emit("Kubernetes", false);
    REQUIRE(waitUntil([page, &before] { return page->count()->text() != before; }));
    CHECK(page->count()->text().contains(QStringLiteral("1")));
}

// ---- tests/test_cleanup_profiles.py --------------------------------------------------------

TEST_CASE("test_capture_suspends_only_until_release_or_cancel", "[widget][personalize]") {
    fv::ProfileHotkeyInput field;
    QList<bool> states;
    QStringList shortcuts;
    QObject::connect(&field, &fv::ProfileHotkeyInput::captureChanged,
                     [&states](bool capturing) { states << capturing; });
    QObject::connect(&field, &fv::ProfileHotkeyInput::captured,
                     [&shortcuts](const QString& hotkey) { shortcuts << hotkey; });
    field.beginCapture();
    const Qt::KeyboardModifiers modifiers = Qt::ControlModifier | Qt::AltModifier;
    QTest::keyPress(&field, Qt::Key_T, modifiers);
    CHECK(states == QList<bool>{true});
    QTest::keyRelease(&field, Qt::Key_T, modifiers);
    CHECK(states == QList<bool>{true, false});
    CHECK(shortcuts == QStringList{QStringLiteral("ctrl+alt+t")});
    field.beginCapture();
    QTest::keyClick(&field, Qt::Key_Escape);
    CHECK(states.mid(states.size() - 2) == QList<bool>{true, false});
    CHECK(shortcuts == QStringList{QStringLiteral("ctrl+alt+t")});
}

TEST_CASE("test_hotkey_dictation_routes_back_from_upload_to_quick_record",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    REQUIRE(rig.startEngine());
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    auto& upload = rig.context().service<fv::UploadPresenter>();
    // The last transcription was the Upload tab's.
    upload.requestUpload(QString::fromStdString(fv::base_utf8::path_to_utf8(toneClip())), 1.0);
    REQUIRE(upload.isUploadSource());
    waitUntil([&rig] { return rig.runtime().has_active_job(); }, std::chrono::milliseconds(2000));
    REQUIRE(waitUntil([&rig] { return !rig.runtime().has_active_job(); }));
    rig.ui().onDictationStarted(QStringLiteral("email"));
    // The results are Quick Record's, and while it records the tab shows the recording's profile.
    CHECK_FALSE(upload.isUploadSource());
    rig.window().setRecordingActive(true);
    tab->refreshCleanupProfiles();
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("email"));
    rig.window().setRecordingActive(false);
}

// ---- the wiring itself ---------------------------------------------------------------------

TEST_CASE("the AI cleanup settings reach Quick Record and the profile shortcuts",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    auto& cleanup = rig.context().service<fv::CleanupSettings>();
    int refreshes = 0;
    rig.hotkeys().set_transform_loader([&refreshes](const Json&) {
        ++refreshes;
        return std::vector<fv::text::Transform>{};
    });

    // A saved profile refreshes the profile shortcuts (on_cleanup_profiles_changed).
    Q_EMIT cleanup.cleanupProfilesChanged();
    CHECK(refreshes == 1);
    // A page capturing a shortcut pauses the global ones.
    auto& manager = rig.context().service<fv::hotkeys::HotkeyManager>();
    Q_EMIT cleanup.profileHotkeyCapture(true);
    CHECK(manager.capture_suspended());
    Q_EMIT cleanup.profileHotkeyCapture(false);
    CHECK_FALSE(manager.capture_suspended());

    // Turning cleanup on shows on Quick Record (refresh_cleanup_controls).
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    cleanup.setCleanupEnabled(true, nullptr);
    CHECK(fv::keys::transcript_cleanup_enabled.read(*rig.settings().snapshot()));
    CHECK(tab->cleanupCheck()->isChecked());
    // The rule dictation knows when a meeting holds the microphone.
    REQUIRE(cleanup.services().meetingActive);
    CHECK_FALSE(cleanup.services().meetingActive());
    rig.activity().set_meeting_active(true);
    CHECK(cleanup.services().meetingActive());
    rig.activity().set_meeting_active(false);
}

TEST_CASE("Quick Record's profile starts through the profile start and Manage opens Profiles",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    REQUIRE(rig.startEngine());
    // A profile start skips the readiness check; a meeting still refuses it.
    rig.activity().set_meeting_active(true);
    CHECK_FALSE(rig.ui().startRecording(QStringLiteral("ui"), QStringLiteral("email")));
    rig.activity().set_meeting_active(false);
    REQUIRE(rig.ui().startRecording(QStringLiteral("ui"), QStringLiteral("email")));
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    CHECK(rig.runtime().recording_profile() == "email");
    rig.app().cancel();
    REQUIRE(waitUntil([&rig] { return !rig.runtime().is_recording(); }));

    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    Q_EMIT tab->profilesRequested();
    fv::SettingsDialog* dialog = rig.context().service<fv::SettingsWindow>().dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->currentDestination() == QString::fromUtf8(dest::cleanup_profiles.data()));
    dialog->hide();
}

namespace {

struct SignalProbe : QObject {
    using QObject::isSignalConnected;
};

bool connected(const QObject& object, const char* signature) {
    const int index = object.metaObject()->indexOfSignal(signature);
    return index >= 0 &&
           (object.*(&SignalProbe::isSignalConnected))(object.metaObject()->method(index));
}

} // namespace

TEST_CASE("controller connects every signal of the AI cleanup settings", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    const auto& cleanup = rig.context().service<fv::CleanupSettings>();
    for (const char* signal :
         {"cleanupChanged()", "cleanupProfilesChanged()", "profileHotkeyCapture(bool)"}) {
        INFO(signal);
        CHECK(connected(cleanup, signal));
    }
    fv::KeyPresence* presence = rig.feature().personalizeWiring()->keyPresence();
    REQUIRE(presence != nullptr);
    CHECK(connected(*presence, "changed()"));
    // Every M4 service the controller provides is in the context.
    CHECK(rig.context().find<fv::RuleTranscriber>() != nullptr);
    CHECK(rig.context().find<fv::dictation::DictionaryLearner>() != nullptr);
    CHECK(rig.context().find<fv::dictation::CommandRuntime>() != nullptr);
    CHECK(rig.context().find<fv::llm::TranscriptCleanup>() != nullptr);
}

TEST_CASE("the guided setup's provider row finds a key saved on the API keys page",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    fv::KeyPresence* presence = rig.feature().personalizeWiring()->keyPresence();
    REQUIRE(presence != nullptr);
    QSignalSpy changed(presence, &fv::KeyPresence::changed);
    const auto summary = [&rig, presence] {
        return fv::providerSummary(*rig.settings().snapshot(), [presence](const std::string& env) {
            return presence->saved(env);
        });
    };
    // OpenRouter, the default provider, has no key in the credential store, the (empty) test
    // environment or the data folder's .env.
    CHECK(summary().status == QStringLiteral("API key needed"));

    // Saved off the GUI thread, as the API keys page saves; the page says keys changed.
    fvtest::offGui([&keys] { keys.store.set("OPENROUTER_API_KEY", "sk-or-test"); });
    Q_EMIT rig.context().service<fv::SettingsPagesSignals>().apiKeysChanged();
    REQUIRE(waitUntil([presence] { return presence->saved("OPENROUTER_API_KEY"); }));
    CHECK(changed.count() >= 1);
    CHECK(summary().status == QStringLiteral("API key saved · Connection not tested"));
    CHECK_FALSE(summary().needsKey);
}

TEST_CASE("Quit stops Command Mode and transforms in the command runtime's step",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    auto http = fv::http::make_curl_client(fv::http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    // The endpoint's key is in no environment, so a transform says how to start.
    ControllerRig rig(fvtest::personalizeOptions(
        keys, http.get(),
        fvtest::customEndpoint("http://127.0.0.1:9/v1", "m", "FIORIVOCE_TEST_ABSENT_KEY")));
    REQUIRE(rig.startEngine());
    fv::dictation::CommandRuntime& command = *rig.feature().personalizeWiring()->commandRuntime();
    const auto refusals = [&rig] {
        const std::vector<std::string> said = rig.statuses();
        return std::count(said.begin(), said.end(), "Set up AI cleanup to use transforms");
    };
    command.run_transform("polish");
    REQUIRE(waitUntil([&refusals] { return refusals() == 1; }));

    // Automation's step follows the command runtime's ("Error during command runtime
    // cleanup"), while the strands still run: a transform asked for there does nothing.
    bool answered = true;
    rig.plan().add(fv::ShutdownStep::Automation, "test step", [&command, &refusals, &answered] {
        command.run_transform("polish");
        answered =
            waitUntil([&refusals] { return refusals() > 1; }, std::chrono::milliseconds(1000));
    });
    rig.plan().run();
    CHECK_FALSE(answered);
}
