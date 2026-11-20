// The Voice & speakers page against the references OpenWhisper rendered for it
// (tools/parity/builders/a15.py, tests/reference/settings_meeting_voice/<variant>/): the page
// body built inside the Settings window, laid out at the builder's width, then taken out of the
// window, as the builder takes OpenWhisper's out of its dialog. The widget tree first, then the
// pixels, with the application styled as OpenWhisper styles it.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage, MeetingVoicePage here.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QResizeEvent>
#include <QScrollArea>

#include "screenshot/screenshot.h"
#include "ui_downloads/meeting_voice_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "voice_model_support.h"

using fv::Json;

namespace {

constexpr int kPageWidth = 900;

// The application styled the way OpenWhisper's render_reference.py styles it.
class AppTheme {
public:
    AppTheme(const QString& theme, int scale) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scale, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = "MeetingVoicePage";
    }
    INFO(path);
    CHECK(actual.value("class", std::string()) == wanted);
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    const Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name));
    }
}

void settle(QWidget& widget) {
    widget.resize(kPageWidth, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

// The builder's data: the speaker model found or not, nothing blocked, no failure.
void pageShot(const std::string& theme, int scale, const char* name, const Json& settings,
              bool modelFound) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    fvtest::WindowRig window(settings);
    fv::registerMeetingVoicePage(window.context(), [modelFound] {
        fv::MeetingVoicePageOptions options;
        options.speakerModelReady = [modelFound] {
            return modelFound;
        };
        return options;
    });
    fv::SettingsDialog dialog(window.context());
    const std::string_view key = fv::destinations::meeting_voice;
    fv::SettingsPage* page = dialog.ensurePage(key);
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
    checkClasses(pins.at("tree"), fv::screenshot::dump_tree(*body), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(*body, name, variant, options);
}

const Json kFollowing{{"selected_model", "parakeet"}, {"meeting_speech_separate", false}};
const Json kSeparate{{"selected_model", "parakeet"},
                     {"meeting_speech_separate", true},
                     {"meeting_asr_model", "parakeet-v3"}};

} // namespace

TEST_CASE("reference screenshot settings_meeting_voice following dark", "[screenshot]") {
    pageShot("dark", 100, "settings_meeting_voice/following", kFollowing, true);
}

TEST_CASE("reference screenshot settings_meeting_voice following light", "[screenshot]") {
    pageShot("light", 100, "settings_meeting_voice/following", kFollowing, true);
}

TEST_CASE("reference screenshot settings_meeting_voice following dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_meeting_voice/following", kFollowing, true);
}

TEST_CASE("reference screenshot settings_meeting_voice separate dark", "[screenshot]") {
    pageShot("dark", 100, "settings_meeting_voice/separate", kSeparate, true);
}

TEST_CASE("reference screenshot settings_meeting_voice separate light", "[screenshot]") {
    pageShot("light", 100, "settings_meeting_voice/separate", kSeparate, true);
}

TEST_CASE("reference screenshot settings_meeting_voice separate dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_meeting_voice/separate", kSeparate, true);
}

TEST_CASE("reference screenshot settings_meeting_voice speaker-download dark", "[screenshot]") {
    pageShot("dark", 100, "settings_meeting_voice/speaker-download", kFollowing, false);
}

TEST_CASE("reference screenshot settings_meeting_voice speaker-download light", "[screenshot]") {
    pageShot("light", 100, "settings_meeting_voice/speaker-download", kFollowing, false);
}

TEST_CASE("reference screenshot settings_meeting_voice speaker-download dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_meeting_voice/speaker-download", kFollowing, false);
}
