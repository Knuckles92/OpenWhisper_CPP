// The Voice model page over fakes: its engine combo and the fields behind it, what it says about
// the engine on this computer, the rail value it reports, and its place in the Settings window.
// Ported from tests/test_settings_models.py (TestRail, TestOnDemandEngine, TestApiModelSelection,
// TestOptionalSpeechSummary) and tests/test_settings_unified.py (TestRouting). OpenWhisper's Local
// Whisper picker is not here (OpenWhisper_CPP has none), so the tests that drive it drive
// Parakeet's Model field instead.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardItemModel>

#include "appkit/destinations.h"
#include "base/utf8.h"
#include "engine/runtime.h"
#include "settings/model_selection.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads/downloads_page.h"
#include "ui_record/record_feature.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/dictation_languages_field.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/speech_backend_picker.h"
#include "ui_widgets/wrapped_label.h"
#include "voice_model_support.h"

namespace {

using fv::Json;
using fv::VoiceModelPage;
using fvtest::choose;
using fvtest::PageRig;

std::vector<QString> itemData(const QComboBox* combo) {
    std::vector<QString> data;
    for (int index = 0; index < combo->count(); ++index) {
        data.push_back(combo->itemData(index).toString());
    }
    return data;
}

QString q(const char* text) {
    return QString::fromUtf8(text);
}

Json parakeet() {
    return Json{{"selected_model", "parakeet"}};
}

Json api(Json more = Json::object()) {
    more["selected_model"] = "api";
    return more;
}

// Parakeet selected, on a device, with a runtime build chosen ("" for none).
Json withDevice(const char* device, const char* runtime = "") {
    Json settings = parakeet();
    settings["local_asr_devices"] = Json::object({{"parakeet", device}});
    if (*runtime != 0) {
        settings["fv_speech_runtime"] = runtime;
    }
    return settings;
}

std::string saved(PageRig& rig, const char* key) {
    return rig.settings.snapshot()->at(key).get<std::string>();
}

} // namespace

// --- The rail ---

// from
// tests/test_settings_models.py::TestRail::test_rail_items_show_the_value_each_destination_owns
TEST_CASE("rail items show the value each destination owns", "[test_settings_models][widget]") {
    PageRig rig;
    auto page = rig.page();
    page->refresh(false);
    // The voice row; OpenWhisper's Local Whisper size is the Parakeet model's label.
    CHECK(rig.host.rail == q("Parakeet TDT 0.6B v3"));
    // The runtime row: the saved device and which build of the runtime is chosen and loaded.
    const auto settings = rig.settings.snapshot();
    CHECK(fv::runtime_summary(*settings, false) == "Auto · no runtime yet");
    CHECK(fv::runtime_summary(withDevice("auto", "cpu"), true) == "Auto · CPU runtime loaded");
    CHECK(fv::runtime_summary(withDevice("cpu", "cpu"), false) == "CPU · CPU runtime not loaded");
    CHECK(fv::runtime_summary(withDevice("cuda", "cuda"), false) == "GPU · GPU runtime not loaded");
}

// from tests/test_settings_models.py::TestRail::test_rail_value_follows_a_new_assignment
TEST_CASE("rail value follows a new assignment", "[test_settings_models][widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    page->refresh(false);
    REQUIRE(rig.host.rail == q("Parakeet TDT 0.6B v3"));
    QSignalSpy reloaded(page.get(), &VoiceModelPage::engineSettingsChanged);

    // The Model field persists its own choice, then the page redraws the rail.
    choose(page->speechControls()->modelCombo(), "orukeet-v0.1");

    CHECK(reloaded.count() == 1);
    CHECK(rig.host.rail == q("Orukeet TDT 0.6B"));
    CHECK(VoiceModelPage::railValue(*rig.settings.snapshot()) == q("Orukeet TDT 0.6B"));
}

// from
// tests/test_settings_models.py::TestRail::test_cloud_engine_reports_itself_instead_of_a_whisper_size
TEST_CASE("cloud engine reports itself instead of a whisper size",
          "[test_settings_models][widget]") {
    PageRig rig;
    auto page = rig.page();
    choose(page->engineCombo(), "api");
    CHECK(rig.host.rail == q("OpenAI · gpt-transcribe"));
}

// from
// tests/test_settings_models.py::TestOptionalSpeechSummary::test_rail_reports_optional_family_and_meeting_model
TEST_CASE("rail reports optional family and meeting model", "[test_settings_models][widget]") {
    // The dictation half: the rail names the selected model; a model of the family chosen in the
    // Model field replaces it. (The meeting half, Moonshine and the engine combo's other
    // families are not OpenWhisper_CPP's.)
    PageRig rig(parakeet());
    auto page = rig.page();
    page->refresh(false);
    CHECK(rig.host.rail == q("Parakeet TDT 0.6B v3"));
    choose(page->speechControls()->modelCombo(), "orukeet-v0.1");
    CHECK(rig.host.rail == q("Orukeet TDT 0.6B"));
}

// --- Downloads ---

// from tests/test_settings_models.py::TestRail::test_manage_downloads_links_open_downloads_filtered
TEST_CASE("manage downloads links open downloads filtered", "[test_settings_models][widget]") {
    // OpenWhisper's page has a link per Whisper picker (filter "local_whisper", and "" for the
    // meeting picker); here the one button of the "On this computer" row, filtered to the engine
    // and to nothing for an engine that keeps nothing here.
    PageRig rig(parakeet());
    auto page = rig.page();
    QSignalSpy opened(page.get(), &VoiceModelPage::downloadsRequested);
    page->downloadsButton()->click();
    choose(page->engineCombo(), "api");
    page->downloadsButton()->click();
    REQUIRE(opened.count() == 2);
    CHECK(opened.at(0).at(0).toString() == q("parakeet"));
    CHECK(opened.at(1).at(0).toString() == QString());
}

// from
// tests/test_settings_models.py::TestRail::test_get_models_filters_downloads_to_the_selected_engine
TEST_CASE("get models filters downloads to the selected engine", "[test_settings_models][widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    QSignalSpy opened(page.get(), &VoiceModelPage::downloadsRequested);

    page->downloadsButton()->click();

    REQUIRE(opened.count() == 1);
    CHECK(opened.at(0).at(0).toString() == q("parakeet"));
    CHECK(page->downloadsButton()->text() == q("Get models and runtimes"));
}

// from
// tests/test_settings_models.py::TestRail::test_voice_page_says_what_the_engine_has_on_this_computer
TEST_CASE("voice page says what the engine has on this computer",
          "[test_settings_models][widget]") {
    PageRig rig;
    rig.downloaded = {"parakeet-v3"};
    rig.installed = {"asr-nvidia-cpu"};
    auto page = rig.page();
    page->refresh(true);
    // Until the worker answers, nothing is known to be downloaded and the runtime is unchecked:
    // both are read on the io executor.
    CHECK(page->inventoryLabel()->text() == q("0 of 2 Parakeet models on this computer."));

    rig.settle();

    CHECK(rig.scans == 1);
    CHECK(page->inventoryLabel()->text() ==
          q("1 of 2 Parakeet models on this computer · NVIDIA Speech CPU installed."));
    CHECK(page->inventoryRow()->isVisibleTo(page.get()));
    CHECK(page->inventoryTitle()->isVisibleTo(page.get()));
}

// A runtime that is not there says so, and a model count of one reads as singular.
TEST_CASE("the inventory names the runtime that is not installed", "[widget]") {
    PageRig rig(withDevice("cuda"));
    auto page = rig.page();
    page->refresh(true);
    rig.settle();
    CHECK(page->inventoryLabel()->text() ==
          q("0 of 2 Parakeet models on this computer · NVIDIA Speech GPU not installed."));
}

// from tests/test_settings_models.py::TestRail::test_cloud_engine_hides_the_on_this_computer_row
TEST_CASE("cloud engine hides the on this computer row", "[test_settings_models][widget]") {
    PageRig rig(api());
    auto page = rig.page();
    CHECK_FALSE(page->inventoryRow()->isVisibleTo(page.get()));
    CHECK_FALSE(page->inventoryTitle()->isVisibleTo(page.get()));
    CHECK(page->inventoryLabel()->text().isEmpty());
    // A paired computer keeps nothing here either.
    choose(page->engineCombo(), "remote");
    CHECK_FALSE(page->inventoryRow()->isVisibleTo(page.get()));
    CHECK(page->inventoryLabel()->text().isEmpty());
    choose(page->engineCombo(), "parakeet");
    CHECK(page->inventoryRow()->isVisibleTo(page.get()));
    CHECK_FALSE(page->inventoryLabel()->text().isEmpty());
}

// from
// tests/test_settings_unified.py::TestRouting::test_runtime_result_cannot_overwrite_a_new_device_selection
TEST_CASE("runtime result cannot overwrite a new device selection",
          "[test_settings_unified][widget]") {
    // An answer is for one (backend, device) pair and is shown only while the page is on it,
    // and the page's newest check overtakes an older one still running. Another Settings
    // window's request needs no token (nothing is shared).
    PageRig rig(parakeet());
    rig.installed = {"asr-nvidia-cpu"};
    auto page = rig.page();
    page->refresh(false);
    rig.settings.save_setting("local_asr_devices", Json::object({{"parakeet", "cpu"}}));
    page->refresh(false);
    rig.settle();
    const QString current = page->inventoryLabel()->text();
    CHECK(current.endsWith(q(" · NVIDIA Speech CPU installed.")));

    // A result for the device that was chosen before.
    page->applyRuntimeLabel("parakeet", "auto", " · Old runtime.");
    CHECK(page->inventoryLabel()->text() == current);

    // A result that arrives while another engine is shown.
    rig.settings.save_setting("selected_model", "api");
    page->refreshEngineSelection();
    page->applyRuntimeLabel("parakeet", "cpu", " · Late result.");
    CHECK(page->inventoryLabel()->text().isEmpty());

    // The same pair's own result is shown.
    rig.settings.save_setting("selected_model", "parakeet");
    page->refreshEngineSelection();
    page->applyRuntimeLabel("parakeet", "cpu", " · Fresh result.");
    CHECK(page->inventoryLabel()->text().endsWith(q(" · Fresh result.")));
}

// A page given no way to look the runtime up says where to look.
TEST_CASE("the inventory says so when the runtime cannot be looked up", "[widget]") {
    fv::MemorySettingsStore settings(parakeet());
    fvtest::RecordingHost host(settings);
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
    VoiceModelPage page(settings, host, client, io, fv::speechEngineCatalog(), {});
    CHECK(page.inventoryLabel()->text() ==
          q("0 of 2 Parakeet models on this computer. Open Downloads to check its runtime."));
}
// The lookup of what is downloaded failing leaves the line saying where to look.
TEST_CASE("the inventory says so when the scan fails", "[widget]") {
    PageRig rig(parakeet());
    rig.scanFails = true;
    auto page = rig.page();
    page->refresh(true);
    rig.settle();
    CHECK(page->inventoryLabel()->text() ==
          q("Open Downloads to see which models are on this computer."));
    // A later scan that works replaces it.
    rig.scanFails = false;
    rig.downloaded = {"parakeet-v3", "orukeet-v0.1"};
    page->refresh(true);
    rig.settle();
    CHECK(page->inventoryLabel()->text().startsWith(q("2 of 2 Parakeet models on this computer")));
}

// from tests/test_settings_models.py::test_settings_closed_mid_scan_is_not_held_by_the_worker
TEST_CASE("settings closed mid scan is not held by the worker", "[test_settings_models][widget]") {
    // The worker used to emit on Settings from its own thread, which raced Settings being
    // destroyed. It holds a mailbox now, never the page: a page that is gone is not told.
    PageRig rig(parakeet());
    rig.downloaded = {"parakeet-v3"};
    auto page = rig.page();
    page->refresh(true);
    REQUIRE(rig.scans == 0);
    const int redraws = rig.host.railRedraws;
    const QPointer<VoiceModelPage> watch(page.get());

    page.reset();
    CHECK(watch.isNull());
    rig.io.run_all();
    QApplication::processEvents();

    CHECK(rig.scans == 1);
    CHECK(rig.host.railRedraws == redraws);
}

// A scan that is overtaken by a newer one is dropped.
TEST_CASE("a scan that a newer one overtook is dropped", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    rig.downloaded = {"parakeet-v3"};
    page->refresh(true);
    rig.downloaded = {"parakeet-v3", "orukeet-v0.1"};
    page->refresh(true);
    rig.settle();
    CHECK(rig.scans == 2);
    CHECK(page->inventoryLabel()->text().startsWith(q("2 of 2 Parakeet models")));
}

// --- The engine combo ---

// from
// tests/test_settings_models.py::TestOnDemandEngine::test_engine_combo_invokes_backend_callback
TEST_CASE("engine combo invokes backend callback", "[test_settings_models][widget]") {
    PageRig rig;
    auto page = rig.page();
    std::vector<QString> requested;
    page->setBackendHandler([&](const QString& name) { requested.push_back(name); });
    QSignalSpy changed(page.get(), &VoiceModelPage::backendChanged);

    choose(page->engineCombo(), "api");

    CHECK(requested == std::vector<QString>{q("API")});
    CHECK(changed.count() == 1);
    // The local fields give way to the server's; the controller saves, so the page does not.
    CHECK_FALSE(page->speechControls()->isVisibleTo(page.get()));
    CHECK(page->apiServer()->isVisibleTo(page.get()));
    CHECK(fv::load_model_selection(rig.settings) == "parakeet");
}

TEST_CASE("an engine is saved by the page when no controller hook is set", "[widget]") {
    PageRig rig;
    auto page = rig.page();
    QSignalSpy changed(page.get(), &VoiceModelPage::backendChanged);
    choose(page->engineCombo(), "api");
    CHECK(fv::load_model_selection(rig.settings) == "api");
}

// choose_backend ignores a value that is not an engine.
TEST_CASE("choose backend ignores an unknown id", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    std::vector<QString> requested;
    page->setBackendHandler([&](const QString& name) { requested.push_back(name); });
    CHECK_FALSE(page->chooseBackend(q("moonshine")));
    CHECK_FALSE(page->chooseBackend(QString()));
    CHECK(requested.empty());
    CHECK(page->currentEngine() == q("parakeet"));
    CHECK(rig.host.messages.empty());
}

// A hook that fails reverts the combo and says why.
TEST_CASE("a change of engine that fails is reverted and reported", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    page->setBackendHandler(
        [](const QString&) { throw std::runtime_error("The engine is busy."); });
    QSignalSpy changed(page.get(), &VoiceModelPage::backendChanged);

    choose(page->engineCombo(), "api");

    REQUIRE(rig.host.messages.size() == 1);
    CHECK(rig.host.messages[0] == q("Couldn't change voice model: The engine is busy."));
    CHECK(page->currentEngine() == q("parakeet"));
    CHECK(page->speechControls()->isVisibleTo(page.get()));
    CHECK_FALSE(page->apiServer()->isVisibleTo(page.get()));
    CHECK(changed.count() == 0);
    CHECK(fv::load_model_selection(rig.settings) == "parakeet");
}

// A local engine's caption is its model's purpose; the other engines have a sentence.
TEST_CASE("the caption under the engine combo follows the engine", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    CHECK(page->engineCaption()->text() == q("Fast dictation and files. Runs locally."));
    choose(page->engineCombo(), "api");
    CHECK(page->engineCaption()->text().startsWith(q("OpenAI, Groq, or an OpenAI-compatible")));
    CHECK(page->engineCaption()->text().endsWith(q("Settings → App → API keys.")));
    choose(page->engineCombo(), "remote");
    CHECK(page->engineCaption()->text() ==
          q("Another computer's engine, over your network. Pair with it in Settings → Remote "
            "engine."));
}

// A change of the engine from anywhere else shows on the page.
TEST_CASE("the page shows the engine saved elsewhere", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    rig.settings.save_setting("selected_model", "api");
    page->refreshEngineSelection();
    CHECK(page->currentEngine() == q("api"));
    CHECK(page->apiServer()->isVisibleTo(page.get()));
    CHECK_FALSE(page->speechControls()->isVisibleTo(page.get()));
    CHECK(rig.host.rail == q("OpenAI · gpt-transcribe"));
}

// A pre-2.0 API name is rewritten when the page reads it, and the page shows the API engine.
TEST_CASE("the page reads a legacy api selection as the api engine", "[widget]") {
    PageRig rig(Json{{"selected_model", "api_gpt4o"}});
    auto page = rig.page();
    CHECK(page->currentEngine() == q("api"));
    CHECK(saved(rig, "selected_model") == "api");
    CHECK(saved(rig, "api_transcription_model") == "gpt-4o-transcribe");
}

// The rewrite is judged by the page's clock: from OpenAI's shutdown day the model it meant is
// gone, and the default stands in.
TEST_CASE("a legacy api selection read after the shutdown day takes the default model",
          "[widget]") {
    PageRig rig(Json{{"selected_model", "api_gpt4o"}}, fvtest::at(2027, 3, 1));
    auto page = rig.page();
    CHECK(page->currentEngine() == q("api"));
    CHECK(saved(rig, "selected_model") == "api");
    CHECK(saved(rig, "api_transcription_model") == "gpt-transcribe");
}

// Loading the saved values never saves anything.
TEST_CASE("loading the page's saved values saves nothing", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    QSignalSpy backend(page.get(), &VoiceModelPage::backendChanged);
    QSignalSpy reload(page.get(), &VoiceModelPage::engineSettingsChanged);
    const std::string before = rig.settings.snapshot()->dump();
    page->loadSettings(*rig.settings.snapshot());
    page->refresh(false);
    CHECK(backend.count() == 0);
    CHECK(reload.count() == 0);
    CHECK(rig.settings.snapshot()->dump() == before);
}

// --- The API server ---

// from
// tests/test_settings_models.py::TestApiModelSelection::test_model_selection_persists_and_updates_rail
TEST_CASE("model selection persists and updates rail", "[test_settings_models][widget]") {
    PageRig rig(api(Json{{"api_transcription_model", "whisper-1"}}));
    auto page = rig.page();
    std::vector<QString> requested;
    page->setBackendHandler([&](const QString& name) { requested.push_back(name); });
    CHECK_FALSE(page->apiServer()->isHidden());
    CHECK(page->speechControls()->isHidden());
    CHECK(page->apiServer()->serverCombo()->currentData().toString() == q("openai"));
    QComboBox* combo = page->apiServer()->modelCombo();
    CHECK(combo->currentData().toString() == q("whisper-1"));
    CHECK(combo->currentText() == q("whisper-1 (retiring Feb 26, 2027)"));

    choose(combo, "gpt-transcribe");

    CHECK(saved(rig, "api_transcription_model") == "gpt-transcribe");
    CHECK(requested == std::vector<QString>{q("API")});
    CHECK(rig.host.rail == q("OpenAI · gpt-transcribe"));
    const Json settings = *rig.settings.snapshot();
    CHECK(fv::voice_detail(settings) == "Sent to OpenAI (api.openai.com) for transcription");
    CHECK(fv::voice_is_cloud(settings));
    CHECK(fv::voice_is_remote(settings));
    page->refresh(false);
    CHECK(requested == std::vector<QString>{q("API")});
}

// from
// tests/test_settings_models.py::TestApiModelSelection::test_a_server_on_this_computer_keeps_audio_here
TEST_CASE("a server on this computer keeps audio here", "[test_settings_models][widget]") {
    PageRig rig(api(Json{{"api_transcription_endpoint", "speaches"}}));
    auto page = rig.page();
    std::vector<QString> requested;
    page->setBackendHandler([&](const QString& name) { requested.push_back(name); });
    page->refresh(false);
    CHECK(page->apiServer()->serverCombo()->currentData().toString() == q("speaches"));
    CHECK(rig.host.rail == q("Speaches · Systran/faster-whisper-small"));
    Json settings = *rig.settings.snapshot();
    CHECK(fv::voice_detail(settings) == "On this computer · Speaches");
    CHECK_FALSE(fv::voice_is_cloud(settings));
    CHECK_FALSE(fv::voice_is_remote(settings));

    choose(page->apiServer()->serverCombo(), "groq");

    CHECK(saved(rig, "api_transcription_endpoint") == "groq");
    CHECK(requested == std::vector<QString>{q("API")});
    CHECK(rig.host.rail == q("Groq · whisper-large-v3-turbo"));
    settings = *rig.settings.snapshot();
    CHECK(fv::voice_is_cloud(settings));
    CHECK(fv::voice_detail(settings) == "Sent to Groq (api.groq.com) for transcription");
}

// from
// tests/test_settings_models.py::TestApiModelSelection::test_retired_models_leave_the_picker_after_shutdown
TEST_CASE("retired models leave the picker after shutdown", "[test_settings_models][widget]") {
    // The speaker-identification half is the meeting page's (M7).
    PageRig rig(api(Json{{"api_transcription_model", "whisper-1"}}), fvtest::at(2027, 2, 26));
    auto page = rig.page();
    QComboBox* combo = page->apiServer()->modelCombo();
    CHECK(itemData(combo) == std::vector<QString>{q("gpt-transcribe")});
    CHECK(combo->currentData().toString() == q("gpt-transcribe"));
}

// The hook failing after a change of server is shown, never thrown through the signal.
TEST_CASE("a change of server the controller refuses is reported", "[widget]") {
    PageRig rig(api(Json{{"api_transcription_endpoint", "speaches"}}));
    auto page = rig.page();
    page->setBackendHandler(
        [](const QString&) { throw std::runtime_error("The engine is busy."); });
    QSignalSpy changed(page.get(), &VoiceModelPage::backendChanged);

    choose(page->apiServer()->serverCombo(), "groq");

    // The server is the fields' own save; only the engine reload failed.
    CHECK(saved(rig, "api_transcription_endpoint") == "groq");
    REQUIRE(rig.host.messages.size() == 1);
    CHECK(rig.host.messages[0] == q("Couldn't change voice model: The engine is busy."));
    CHECK(changed.count() == 0);
    CHECK(rig.host.rail == q("Groq · whisper-large-v3-turbo"));
}

// The API server's link to its key opens API keys on the key the selected server uses.
TEST_CASE("the api server's key link opens api keys on its credential", "[widget]") {
    PageRig rig(api(Json{{"api_transcription_endpoint", "groq"}}));
    auto page = rig.page();
    QSignalSpy help(page.get(), &VoiceModelPage::helpRequested);
    Q_EMIT page->apiServer()->helpRequested(q("api_keys"));
    REQUIRE(rig.host.apiKeys.size() == 1);
    CHECK(rig.host.apiKeys[0] == q("GROQ_API_KEY"));
    CHECK(help.count() == 1);
    // A server that needs no key has no credential to show.
    rig.settings.save_setting("api_transcription_endpoint", "speaches");
    page->refresh(false);
    Q_EMIT page->apiServer()->helpRequested(q("api_keys"));
    REQUIRE(rig.host.apiKeys.size() == 2);
    CHECK(rig.host.apiKeys[1].isEmpty());
}

// --- Languages ---

// A change of the dictation languages tells the window, and the Language field follows.
TEST_CASE("changing the dictation languages tells the window", "[widget]") {
    PageRig rig(parakeet());
    auto page = rig.page();
    page->dictationLanguages()->add(q("en"));
    page->dictationLanguages()->add(q("ru"));
    CHECK(rig.host.changes == std::vector<QString>{q("languages"), q("languages")});
    // Two chosen languages decide the dictation language, so the engine's own Language field
    // steps aside.
    CHECK(page->speechControls()->languageField()->isHidden());
}

// --- The page's own controls ---

TEST_CASE("the page exposes its controls by name", "[widget]") {
    PageRig rig;
    auto page = rig.page();
    for (const std::string& name : VoiceModelPage::controlNames()) {
        INFO(name);
        CHECK(page->namedControl(name) != nullptr);
    }
    CHECK(page->namedControl("engine_combo") == page->engineCombo());
    CHECK(page->namedControl("speech_controls") == page->speechControls());
    CHECK(page->namedControl("dictation_languages") == page->dictationLanguages());
    CHECK(page->namedControl("ondemand_whisper_picker") == nullptr);
    CHECK(page->engineCombo()->objectName() == q("ondemandEngineCombo"));
    CHECK(page->engineCombo()->minimumHeight() == 40);
    CHECK(itemData(page->engineCombo()) ==
          std::vector<QString>{q("api"), q("parakeet"), q("remote")});
}

// from tests/test_settings_models.py::test_display_name_falls_back_to_the_default_backend
TEST_CASE("display name falls back to the default backend", "[test_settings_models]") {
    CHECK(fv::backendDisplayName("parakeet") == q("Parakeet"));
    CHECK(fv::backendDisplayName("api") == q("API"));
    CHECK(fv::backendDisplayName("not-a-backend") == q("Parakeet"));
    CHECK(fv::backendDisplayName("") == q("Parakeet"));
}

// --- In the Settings window ---

namespace {

// Registers the page through its feature, which registers the Downloads page beside it.
void registerPages(fv::AppContext& context) {
    fv::DownloadsFeature feature;
    feature.install(context);
}

} // namespace

// from
// tests/test_settings_unified.py::TestRouting::test_voice_page_opens_downloads_filtered_to_its_engine
TEST_CASE("voice page opens downloads filtered to its engine", "[test_settings_unified][widget]") {
    fvtest::WindowRig rig(parakeet());
    registerPages(rig.context());
    fv::SettingsDialog dialog(rig.context());
    dialog.selectDestination(QString::fromUtf8(fv::destinations::voice_model));
    auto* page = qobject_cast<VoiceModelPage*>(dialog.pageContent(fv::destinations::voice_model));
    REQUIRE(page != nullptr);

    page->downloadsButton()->click();

    CHECK(dialog.currentDestination() == q("downloads"));
    auto* downloads = qobject_cast<fv::DownloadsPage*>(dialog.pageContent("downloads"));
    REQUIRE(downloads != nullptr);
    CHECK(downloads->backendFilterCombo()->currentData().toString() == q("parakeet"));
}

TEST_CASE("the downloads feature registers the voice model page", "[widget]") {
    fvtest::WindowRig rig;
    registerPages(rig.context());
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page("voice_model");
    REQUIRE(spec != nullptr);
    CHECK(spec->title == q("Voice model"));
    CHECK(spec->subtitle ==
          q("The transcription engine used by Quick Record, hotkey dictation, and Upload File."));
    REQUIRE(spec->railValue);
    CHECK(spec->railValue(Json::object()) == q("Parakeet TDT 0.6B v3"));
    CHECK(
        spec->railValue(Json{{"selected_model", "api"}, {"api_transcription_endpoint", "groq"}}) ==
        q("Groq · whisper-large-v3-turbo"));
    // Search finds the page's fields before it is built, and Home projects into its engine combo
    // through the control names.
    REQUIRE(spec->searchFields.size() == 3);
    CHECK(spec->searchFields[1].label == q("API server and model"));
    CHECK(spec->searchFields[2].label == q("Recording engine"));
    REQUIRE(spec->helpTexts.size() == 1);
    CHECK(spec->helpTexts[0].startsWith(q("For dictation previews, Parakeet")));
    CHECK(rig.context().settingsPages().controlDestination("engine_combo") == "voice_model");
    CHECK(rig.context().settingsPages().controlDestination("speech_download_button") ==
          "voice_model");
    CHECK(rig.context().settingsPages().controlDestination("ondemand_whisper_picker").empty());
}

TEST_CASE("the window reaches the page's controls and redraws the rail from it", "[widget]") {
    fvtest::WindowRig rig(parakeet());
    registerPages(rig.context());
    fv::SettingsDialog dialog(rig.context());
    auto* combo = qobject_cast<QComboBox*>(dialog.control("engine_combo"));
    REQUIRE(combo != nullptr);
    CHECK(dialog.isBuilt("voice_model"));
    choose(combo, "api");
    // The page saved the engine and the window's rail follows the saved value.
    CHECK(rig.settings().snapshot()->at("selected_model").get<std::string>() == "api");
    CHECK(dialog.rail()->value(QStringLiteral("voice_model")) == q("OpenAI · gpt-transcribe"));
    // A dictation-language edit goes through the window's notification.
    QSignalSpy changed(&dialog, &fv::SettingsDialog::settingsChanged);
    auto* page = qobject_cast<VoiceModelPage*>(dialog.pageContent("voice_model"));
    REQUIRE(page != nullptr);
    page->dictationLanguages()->add(q("en"));
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == q("languages"));
}

TEST_CASE("the page follows an engine saved from Quick Record", "[widget]") {
    fvtest::WindowRig rig(parakeet());
    rig.context().provide(std::make_shared<fv::RecordWidgets>(rig.settings(), rig.ui()));
    registerPages(rig.context());
    fv::SettingsDialog dialog(rig.context());
    dialog.ensurePage("voice_model");
    auto* page = qobject_cast<VoiceModelPage*>(dialog.pageContent("voice_model"));
    REQUIRE(page != nullptr);
    REQUIRE(page->currentEngine() == q("parakeet"));

    // Another view of the engine saves its change.
    rig.settings().save_setting("selected_model", "api");
    QApplication::processEvents();

    CHECK(page->currentEngine() == q("api"));
    CHECK(page->apiServer()->isVisibleTo(page));
}

// Registered without options, the page reads the folders the app installs into: a runtime counts
// as installed once its sentinel file is there, and a model only when its folder is complete.
TEST_CASE("the page reads this computer's folders when registered without options", "[widget]") {
    fvtest::WindowRig rig(Json{{"selected_model", "parakeet"},
                               {"local_asr_devices", Json::object({{"parakeet", "cpu"}})}});
    const std::filesystem::path runtime = rig.paths().components_root() / "asr-nvidia-cpu";
    std::filesystem::create_directories(runtime);
    std::ofstream(runtime / ".installed") << "0.1.0";
    std::filesystem::create_directories(rig.paths().speech_models_root() / "parakeet-v3");
    fv::registerVoiceModelPage(rig.context());
    fv::SettingsDialog dialog(rig.context());
    dialog.ensurePage("voice_model");
    auto* page = qobject_cast<VoiceModelPage*>(dialog.pageContent("voice_model"));
    REQUIRE(page != nullptr);
    page->refresh(true);
    rig.io().run_all();
    QApplication::processEvents();
    // A model folder without its pinned files is not a model on this computer.
    CHECK(page->inventoryLabel()->text() ==
          q("0 of 2 Parakeet models on this computer · NVIDIA Speech CPU installed."));

    // Without the component, a runtime folder of the user's own stands in for it, as it does when
    // the engine loads.
    std::filesystem::remove(runtime / ".installed");
    page->refresh(false);
    rig.io().run_all();
    QApplication::processEvents();
    CHECK(page->inventoryLabel()->text().endsWith(q(" · NVIDIA Speech CPU not installed.")));
    const std::filesystem::path chosen = rig.paths().components_root().parent_path() / "nemo";
    std::filesystem::create_directories(chosen / "bin");
    std::ofstream(chosen / "bin" / fv::runtime_library_name()) << "runtime";
    rig.settings().save_setting("fv_runtime_dir", fv::base_utf8::path_to_utf8(chosen));
    page->refresh(false);
    rig.io().run_all();
    QApplication::processEvents();
    CHECK(page->inventoryLabel()->text().endsWith(q(" · NVIDIA Speech CPU installed.")));
}
