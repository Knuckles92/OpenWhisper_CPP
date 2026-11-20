// The App pages against the references OpenWhisper rendered for them (tools/parity/builders/a11.py,
// tests/reference/settings_<page>/<variant>/): each page body built inside the Settings window,
// laid out at the builder's width, then taken out of the window, as the builder takes
// OpenWhisper's out of its dialog. The widget tree first, then the pixels, with the application
// styled as OpenWhisper styles it.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage, the page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <string>

#include <QCoreApplication>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>

#include "app_pages_support.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_overlay/recording_look_preview.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_settings/api_keys_section.h"
#include "ui_settings/appearance_page.h"
#include "ui_settings/recording_page.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/particles.h"
#include "uimath/sample_voice.h"
#include "uimath/voice_ribbon.h"

namespace fv {

// The overlay's private state OpenWhisper's builder sets (its friend; tests/ui_overlay has the
// full one).
struct WaveformOverlayTestAccess {
    explicit WaveformOverlayTestAccess(WaveformOverlay& overlay) : o(overlay) {}

    double now() const { return o.now(); }
    double& listenStarted() { return o.listenStarted_; }
    QTimer& hiddenTimer() { return *o.hiddenTimer_; }
    QTimer& captionTimer() { return *o.captionTimer_; }
    ParticleStyle& style() { return *o.style_; }
    VoiceRibbon& ribbon() { return o.ribbon_; }

    WaveformOverlay& o;
};

} // namespace fv

using fv::Json;
using namespace fvtest;

namespace {

constexpr int kPageWidth = 900;

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void checkClasses(const Json& expected, const Json& actual, const std::string& path,
                  const std::string& pageClass) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = pageClass;
    } else if (wanted.starts_with('_')) {
        // OpenWhisper's private classes (_PreferredRow) drop the underscore here.
        wanted.erase(0, 1);
    }
    INFO(path);
    const std::string got = actual.value("class", std::string());
    // The rows a rebuild replaced are hidden and wait for deleteLater; PyQt has let go of their
    // Python objects by then, so OpenWhisper's dump names the Qt class of their labels.
    const bool replaced = !expected.value("visible", true) && wanted == "QLabel" &&
                          (got == "WrappedLabel" || got == "ElidingLabel");
    CHECK((got == wanted || replaced));
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    const Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name),
                     pageClass);
    }
}

// The tree without the "entry" property of the microphone rows' buttons.
Json stripEntries(Json tree) {
    if (auto found = tree.find("properties"); found != tree.end() && found->is_object()) {
        found->erase("entry");
    }
    if (auto children = tree.find("children"); children != tree.end()) {
        for (Json& child : *children) {
            child = stripEntries(std::move(child));
        }
    }
    return tree;
}

// The builder's measurement: shown at its width, events processed, hidden again; a grab lays out
// what the tree dump then reads, as render_reference.py's grab does.
void settle(QWidget& widget) {
    widget.resize(kPageWidth, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

// Run on the body once measured, before it is compared: shown at the reference's size and held
// still (the Appearance preview).
using Hold = std::function<void(QWidget& body, QSize size)>;

void pageShot(const std::string& theme, int scale, std::string_view key, const char* name,
              const char* pageClass, const std::function<void(AppPagesRig&)>& prepare = {},
              const Json& settings = Json::object(), const Hold& hold = {}) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    AppPagesRig rig(settings);
    if (prepare) {
        prepare(rig);
    }
    fv::SettingsDialog& dialog = rig.dialog();
    fv::SettingsPage* page = dialog.ensurePage(key);
    // What the page reads on the worker has arrived.
    if (auto* keys = dynamic_cast<fv::ApiKeysSection*>(dialog.pageContent(key))) {
        REQUIRE(pumpUntil([keys] { return keys->stateCurrent(); }));
    }
    if (auto* recording = dynamic_cast<fv::RecordingPage*>(dialog.pageContent(key))) {
        REQUIRE(pumpUntil([recording] {
            return recording->microphones().devices().has_value() &&
                   !recording->usageLabel()->text().isEmpty();
        }));
    }
    // The page's tile grids take their columns from the page's width, so the page leaves the
    // window's scroll area first (as the builder takes it out), where a pending layout would
    // give it the viewport's width again.
    QScrollArea* scroll = dialog.pageScroll(key);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    const QSize old = page->size();
    page->resize(kPageWidth, 4000);
    QResizeEvent resized(page->size(), old);
    QCoreApplication::sendEvent(page, &resized);
    std::unique_ptr<QWidget> body(dialog.pageContent(key));
    REQUIRE(body != nullptr);
    body->setParent(nullptr);
    settle(*body);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    body->ensurePolished();
    const Json pins = fv::screenshot::load_reference(name, variant);
    if (hold) {
        const auto& size = pins.at("logical_size");
        hold(*body, QSize(size[0].get<int>(), size[1].get<int>()));
    }
    const Json reference = pins.at("tree");
    checkClasses(reference, fv::screenshot::dump_tree(*body), name, pageClass);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    if (stripEntries(reference) != reference) {
        // OpenWhisper keeps each backup row's entry token as a dynamic "entry" property on its
        // buttons; FioriVoce keeps it in the section (no theme rule reads it, and dynamic
        // properties are only the theme's). Every other property is compared here, and the
        // tree compared by expect_matches leaves properties out.
        const Json expected = stripEntries(reference);
        // Shown, as the reference was dumped.
        body->show();
        QCoreApplication::processEvents();
        const Json shown = fv::screenshot::dump_tree(*body);
        body->hide();
        for (const auto& difference : fv::screenshot::compare_trees(expected, shown, {"class"})) {
            if (difference.find(": geometry ") == std::string::npos) {
                FAIL_CHECK(name << "/" << variant << " tree: " << difference);
            }
        }
        options.ignore_keys = {"class", "properties"};
    }
    fv::screenshot::expect_matches(*body, name, variant, options);
}

void general(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::general, "settings_general/default", "GeneralPage");
}

void developer(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::advanced, "settings_developer/default", "DeveloperPage");
}

// The builder's saved OpenAI key; masked, it shows its last four characters.
void apiKeys(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::api_keys, "settings_api_keys/saved", "ApiKeysSection",
             [](AppPagesRig& rig) {
                 auto saved = std::make_shared<std::promise<void>>();
                 rig.pool().post([&rig, saved] {
                     rig.store().set("OPENAI_API_KEY", "sk-test-key-0000001234");
                     saved->set_value();
                 });
                 saved->get_future().wait();
             });
}

// The builder's text-model owners: OpenRouter's key saved and its row selected, and a custom
// endpoint, "Team gateway", that sends TEAM_KEY.
void apiKeysTextOwners(const std::string& theme, int scale) {
    const Json settings{
        {"text_llm_profiles", Json::array({{{"id", "custom_team"},
                                            {"name", "Team gateway"},
                                            {"base_url", "https://llm.example.test/v1"},
                                            {"api_key_env", "TEAM_KEY"}}})}};
    pageShot(
        theme, scale, dest::api_keys, "settings_api_keys/text-owners", "ApiKeysSection",
        [](AppPagesRig& rig) {
            auto saved = std::make_shared<std::promise<void>>();
            rig.pool().post([&rig, saved] {
                rig.store().set("OPENROUTER_API_KEY", "sk-or-test-key-0000005678");
                saved->set_value();
            });
            saved->get_future().wait();
            auto& keys = rig.page<fv::ApiKeysSection>(dest::api_keys);
            REQUIRE(pumpUntil([&keys] { return keys.stateCurrent(); }));
            keys.combo()->setCurrentIndex(
                keys.combo()->findData(QStringLiteral("OPENROUTER_API_KEY")));
        },
        settings);
}

// The builder's microphones (the dumped Windows device list; the USB microphone preferred, Blue
// Snowball and a DirectSound headset as backups) and two saved recordings of 806 KB.
void recording(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::recording, "settings_recording/backups", "RecordingPage",
             [](AppPagesRig& rig) {
                 rig.backend().load(
                     Json::parse(readText(fv::testkit::reference_path().parent_path() / "fixtures" /
                                          "audio" / "devices-windows.json")));
                 rig.settings().save_setting(
                     "audio_input_priority",
                     Json::array({entry("Microphone (2- USB Audio Device"),
                                  entry("Microphone (Blue Snowball )"),
                                  entry("Headset (soundcore V20i)", kDsound)}));
                 rig.pages().hooks().recordingsUsage = [] {
                     return std::pair<std::int64_t, std::int64_t>(2, 825344);
                 };
             });
}

// The builder's PREVIEW_NOW, PREVIEW_HUE_TIME and PREVIEW_T (as builders/a07.py's
// recording_look_preview/bars, which tests/ui_overlay replays on its own).
constexpr double kPreviewNow = 1000.0;
constexpr double kPreviewHueTime = 3.0;
constexpr double kPreviewT = 2.0;

// settings_appearance_bars: the Bars look; once measured, the page is shown again at its size
// (the preview replays its entry), every timer stopped and the sample voice fed frame by frame up
// to PREVIEW_T.
void appearance(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::appearance, "settings_appearance/bars", "AppearancePage", {},
             Json{{"recording_overlay_look", "bars"}}, [](QWidget& body, QSize size) {
                 auto* page = dynamic_cast<fv::AppearancePage*>(&body);
                 REQUIRE(page != nullptr);
                 fv::RecordingLookPreview& preview = *page->lookPreview();
                 fv::WaveformOverlayTestAccess access(preview.overlay());
                 REQUIRE(access.now() == kPreviewNow);
                 access.style().set_animation_time(kPreviewHueTime);
                 body.resize(size);
                 body.show();
                 preview.timer().stop();
                 preview.overlay().timer().stop();
                 access.hiddenTimer().stop();
                 access.captionTimer().stop();
                 const int frames = static_cast<int>(std::lround(kPreviewT * 30));
                 for (int frame = 0; frame < frames; ++frame) {
                     access.ribbon().hear(fv::sample_level(frame / 30.0));
                     access.ribbon().advance(1.0 / 30);
                 }
                 access.listenStarted() = kPreviewNow - kPreviewT;
             });
}

} // namespace

TEST_CASE("reference screenshot settings_appearance bars dark", "[screenshot]") {
    appearance("dark", 100);
}

TEST_CASE("reference screenshot settings_appearance bars light", "[screenshot]") {
    appearance("light", 100);
}

TEST_CASE("reference screenshot settings_appearance bars dark 130", "[screenshot]") {
    appearance("dark", 130);
}

TEST_CASE("reference screenshot settings_recording backups dark", "[screenshot]") {
    recording("dark", 100);
}

TEST_CASE("reference screenshot settings_recording backups light", "[screenshot]") {
    recording("light", 100);
}

TEST_CASE("reference screenshot settings_recording backups dark 130", "[screenshot]") {
    recording("dark", 130);
}

TEST_CASE("reference screenshot settings_general default dark", "[screenshot]") {
    general("dark", 100);
}

TEST_CASE("reference screenshot settings_general default light", "[screenshot]") {
    general("light", 100);
}

TEST_CASE("reference screenshot settings_general default dark 130", "[screenshot]") {
    general("dark", 130);
}

TEST_CASE("reference screenshot settings_developer default dark", "[screenshot]") {
    developer("dark", 100);
}

TEST_CASE("reference screenshot settings_developer default light", "[screenshot]") {
    developer("light", 100);
}

TEST_CASE("reference screenshot settings_developer default dark 130", "[screenshot]") {
    developer("dark", 130);
}

TEST_CASE("reference screenshot settings_api_keys saved dark", "[screenshot]") {
    apiKeys("dark", 100);
}

TEST_CASE("reference screenshot settings_api_keys saved light", "[screenshot]") {
    apiKeys("light", 100);
}

TEST_CASE("reference screenshot settings_api_keys saved dark 130", "[screenshot]") {
    apiKeys("dark", 130);
}

TEST_CASE("reference screenshot settings_api_keys text-owners dark", "[screenshot]") {
    apiKeysTextOwners("dark", 100);
}

TEST_CASE("reference screenshot settings_api_keys text-owners light", "[screenshot]") {
    apiKeysTextOwners("light", 100);
}

TEST_CASE("reference screenshot settings_api_keys text-owners dark 130", "[screenshot]") {
    apiKeysTextOwners("dark", 130);
}
