// The After the meeting and Dashboard pages against the references OpenWhisper rendered for them
// (tools/parity/builders/a31.py, tests/reference/settings_meeting_after/<variant>/ and
// settings_meeting_dashboard/<variant>/): each page body built inside the Settings window, laid
// out at the builder's width, then taken out of the window, as the builder takes OpenWhisper's
// out of its dialog. The widget tree first, then the pixels, with the application styled as
// OpenWhisper styles it.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage, the page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QResizeEvent>
#include <QScrollArea>

#include "app_pages_support.h"
#include "screenshot/screenshot.h"
#include "ui_meeting/after_end_page.h"
#include "ui_settings/settings_page.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr int kPageWidth = 900;

void checkClasses(const Json& expected, const Json& actual, const std::string& path,
                  const std::string& pageClass) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = pageClass;
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
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name),
                     pageClass);
    }
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

void pageShot(const std::string& theme, int scale, std::string_view key, const char* name,
              const char* pageClass, const Json& settings = Json::object()) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    AppPagesRig rig(settings,
                    [](AppPagesRig& r) { fv::registerMeetingSettingsPages(r.context()); });
    fv::SettingsDialog& dialog = rig.dialog();
    fv::SettingsPage* page = dialog.ensurePage(key);
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
    checkClasses(pins.at("tree"), fv::screenshot::dump_tree(*body), name, pageClass);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(*body, name, variant, options);
}

void afterOn(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::meeting_after, "settings_meeting_after/report-on", "AfterEndPage");
}

void afterOff(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::meeting_after, "settings_meeting_after/report-off", "AfterEndPage",
             Json{{"meeting_end_report", false}});
}

void dashboardLocal(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::meeting_dashboard, "settings_meeting_dashboard/localhost",
             "DashboardPage");
}

void dashboardLan(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::meeting_dashboard, "settings_meeting_dashboard/lan",
             "DashboardPage", Json{{"meeting_server_bind", "lan"}});
}

} // namespace

TEST_CASE("reference screenshot settings_meeting_after report-on dark", "[screenshot]") {
    afterOn("dark", 100);
}

TEST_CASE("reference screenshot settings_meeting_after report-on light", "[screenshot]") {
    afterOn("light", 100);
}

TEST_CASE("reference screenshot settings_meeting_after report-on dark 130", "[screenshot]") {
    afterOn("dark", 130);
}

TEST_CASE("reference screenshot settings_meeting_after report-off dark", "[screenshot]") {
    afterOff("dark", 100);
}

TEST_CASE("reference screenshot settings_meeting_after report-off light", "[screenshot]") {
    afterOff("light", 100);
}

TEST_CASE("reference screenshot settings_meeting_after report-off dark 130", "[screenshot]") {
    afterOff("dark", 130);
}

TEST_CASE("reference screenshot settings_meeting_dashboard localhost dark", "[screenshot]") {
    dashboardLocal("dark", 100);
}

TEST_CASE("reference screenshot settings_meeting_dashboard localhost light", "[screenshot]") {
    dashboardLocal("light", 100);
}

TEST_CASE("reference screenshot settings_meeting_dashboard localhost dark 130", "[screenshot]") {
    dashboardLocal("dark", 130);
}

TEST_CASE("reference screenshot settings_meeting_dashboard lan dark", "[screenshot]") {
    dashboardLan("dark", 100);
}

TEST_CASE("reference screenshot settings_meeting_dashboard lan light", "[screenshot]") {
    dashboardLan("light", 100);
}

TEST_CASE("reference screenshot settings_meeting_dashboard lan dark 130", "[screenshot]") {
    dashboardLan("dark", 130);
}
