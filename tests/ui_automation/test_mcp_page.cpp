// Settings > Automate > API & MCP, this computer's page (tests/test_mcp_settings_page.py): the
// listener's state and the saved choices on one page, over ScriptedMcpControl (OpenWhisper's
// FakeServer) and an in-memory settings file. Where Python asserts through tomllib on the
// ChatGPT configuration, the configuration is compared with agentapi::chatgpt_config's, which
// tests/agentapi pins against OpenWhisper's.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QScrollBar>
#include <QTest>
#include <QToolButton>

#include "agentapi/agent_controls.h"
#include "agentapi/setup.h"
#include "automation_support.h"
#include "mcp_support.h"
#include "settings/keys.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_theme/font_scale.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::agentapi::ServerStatus;
using namespace fvtest;
namespace keys = fv::keys;

namespace {

const char* const kTag = "[test_mcp_settings_page][widget]";

const std::string kHidden(fv::mcp_page::HIDDEN_TOKEN);

QString qs8(const std::string& value) {
    return QString::fromStdString(value);
}

std::string plain(const fv::McpSettingsPage& page) {
    return text(page.setupText()->toPlainText());
}

bool has(const QString& haystack, const char* needle) {
    return haystack.contains(QString::fromUtf8(needle));
}

// ServerStatus("running", "Ready", port, remote).
ServerStatus status(const char* state, const char* message, int port = 8767,
                    const char* remote = "") {
    return ServerStatus{state, message, port, remote};
}

// A copy of the setup the page made: the preview with the real token in place of the mask.
std::string withToken(std::string preview) {
    const std::size_t at = preview.find(kHidden);
    if (at != std::string::npos) {
        preview.replace(at, kHidden.size(), kMcpToken);
    }
    return preview;
}

} // namespace

TEST_CASE("test_enable_status_setup_copy_and_stop", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    CHECK_FALSE(page->enabled()->isChecked());
    CHECK_FALSE(page->connection()->isEnabled());
    CHECK(rig.server.starts.empty());
    page->port()->setValue(9123);
    page->enabled()->setChecked(true);
    CHECK(rig.saved("mcp_enabled") == Json(true));
    CHECK(rig.server.starts == std::vector<int>{9123});
    CHECK(has(page->statusLabel()->text(), "Starting"));
    CHECK_FALSE(page->connection()->isEnabled());

    rig.server.current = status("stopping", "Stopping MCP…", 9123);
    page->refresh();
    CHECK_FALSE(page->enableTile()->isEnabled());

    rig.server.current = status("running", "Ready for agent connections.", 9123);
    page->refresh();
    CHECK(page->connection()->isEnabled());
    CHECK(has(page->statusLabel()->text(), "Running"));
    CHECK(page->enableTile()->isEnabled());
    CHECK(text(page->url()->text()) == "http://127.0.0.1:9123/mcp");
    CHECK(page->token()->echoMode() == QLineEdit::Password);
    CHECK(plain(*page).find(kMcpToken) == std::string::npos);
    CHECK(has(page->setupHint()->text(), "already filled in"));
    const std::string url = text(page->url()->text());

    page->copySetup()->click();
    const std::string prompt = text(rig.clipboard.last());
    CHECK(prompt.find("get_status") != std::string::npos);
    CHECK(prompt.find("Authorization: Bearer " + kMcpToken + ".") != std::string::npos);
    CHECK(prompt.find("Ask me for the token") == std::string::npos);
    page->setupKind()->setCurrentIndex(1);
    CHECK(plain(*page).find("claude mcp add") != std::string::npos);
    CHECK(plain(*page).find("<PASTE_TOKEN>") == std::string::npos);
    page->copySetup()->click();
    CHECK(text(rig.clipboard.last()).find("\"Authorization: Bearer " + kMcpToken + "\"") !=
          std::string::npos);
    page->setupKind()->setCurrentIndex(2);
    const Json preview = Json::parse(plain(*page));
    CHECK(preview.at("mcpServers").at("fiorivoce").at("url") == url);
    page->copySetup()->click();
    const Json copied = Json::parse(text(rig.clipboard.last()));
    CHECK(copied.at("mcpServers").at("fiorivoce").at("headers") ==
          Json{{"Authorization", "Bearer " + kMcpToken}});
    page->setupKind()->buttons()[3]->click();
    CHECK(plain(*page).find(kMcpToken) == std::string::npos);
    page->copySetup()->click();
    CHECK(text(rig.clipboard.last()) == fv::agentapi::chatgpt_config(url, kMcpToken));
    page->copyText(qs8(rig.server.token()));
    CHECK(text(rig.clipboard.last()) == kMcpToken);
    // The token is never in the settings file.
    CHECK(rig.store.file().bytes().value().find(kMcpToken) == std::string::npos);

    page->enabled()->setChecked(false);
    CHECK(rig.saved("mcp_enabled") == Json(false));
    CHECK_FALSE(page->connection()->isEnabled());
    CHECK(page->token()->text().isEmpty());
    CHECK(page->port()->isEnabled());
    const qsizetype copies = rig.clipboard.copied.size();
    page->copyText(QStringLiteral("should not copy when stopped"));
    CHECK(rig.clipboard.copied.size() == copies);
    CHECK(text(rig.clipboard.last()) == kMcpToken);
}

TEST_CASE("test_failed_start_can_retry_and_failed_save_rolls_back", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    page->enabled()->setChecked(true);
    rig.server.current = status("error", "Port is occupied.", fv::agentapi::DEFAULT_PORT);
    page->refresh();
    CHECK(has(page->statusLabel()->text(), "Port is occupied"));
    CHECK_FALSE(page->retry()->isHidden());
    CHECK_FALSE(page->connection()->isEnabled());
    page->retry()->click();
    CHECK(rig.server.starts.size() == 2);
    rig.server.stop(false);
    page->enabled()->setChecked(false);

    rig.store.file().fail_next_replace("read only");
    page->enabled()->setChecked(true);
    CHECK_FALSE(page->enabled()->isChecked());
    CHECK(has(page->notice()->text(), "Could not save"));
    CHECK(rig.server.starts.size() == 2);
}

TEST_CASE("test_restore_is_opt_in_and_validates_port", kTag) {
    // McpRuntime::restore starts with what the saved settings say (tests/agentapi runs it
    // against the real listener); these are the values it reads: off until turned on, and a
    // saved port that is not a port reads as the default.
    fv::MemorySettingsStore store(Json::object());
    CHECK_FALSE(keys::mcp_enabled.read(*store.snapshot()));
    store.update(Json{{"mcp_enabled", true}, {"mcp_port", "bad"}});
    const auto saved = store.snapshot();
    CHECK(keys::mcp_enabled.read(*saved));
    CHECK(keys::mcp_port.read(*saved) == fv::agentapi::DEFAULT_PORT);
}

TEST_CASE("test_permissions_are_individual_persistent_and_configurable_while_off", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    for (const auto& [key, check] : page->permissionChecks()) {
        CHECK_FALSE(check->isChecked());
    }
    for (const auto& [key, check] : page->settingChecks()) {
        CHECK_FALSE(check->isChecked());
    }
    CHECK_FALSE(page->settingsPermissions()->isEnabled());
    page->permissionCheck("mcp_retitle_transcriptions")->setChecked(true);
    page->permissionCheck("mcp_settings_access")->setChecked(true);
    page->settingCheck("auto_paste")->setChecked(true);
    page->settingCheck("ui_theme")->setChecked(true);
    CHECK(rig.server.starts.empty());
    CHECK(rig.saved("mcp_retitle_transcriptions") == Json(true));
    CHECK(rig.saved("mcp_retitle_meetings").is_null());
    CHECK(rig.saved("mcp_writable_settings") == Json{{"auto_paste", true}, {"ui_theme", true}});
    page->settingCheck("auto_paste")->setChecked(false);
    CHECK(rig.saved("mcp_writable_settings").at("ui_theme") == Json(true));
    page->permissionCheck("mcp_settings_access")->setChecked(false);
    CHECK_FALSE(page->settingsPermissions()->isEnabled());
    CHECK(page->settingCheck("ui_theme")->isChecked());
    page->permissionCheck("mcp_settings_access")->setChecked(true);
    CHECK(page->settingsPermissions()->isEnabled());
}

TEST_CASE("test_failed_permission_save_rolls_back_ui_and_preserves_other_grants", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    page->permissionCheck("mcp_settings_access")->setChecked(true);
    page->settingCheck("auto_paste")->setChecked(true);

    rig.store.file().fail_next_replace("private-marker");
    page->settingCheck("ui_theme")->setChecked(true);
    CHECK_FALSE(page->settingCheck("ui_theme")->isChecked());
    CHECK(page->settingCheck("auto_paste")->isChecked());
    CHECK(has(page->notice()->text(), "Could not save"));
    CHECK_FALSE(has(page->notice()->text(), "private-marker"));
    rig.store.file().fail_next_replace("private-marker");
    page->permissionCheck("mcp_retitle_meetings")->setChecked(true);
    CHECK_FALSE(page->permissionCheck("mcp_retitle_meetings")->isChecked());
}

TEST_CASE("test_setup_falls_back_to_the_placeholder_without_a_token", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    rig.server.current = status("running", "Ready", fv::agentapi::DEFAULT_PORT);
    rig.server.tokenOverride = [] {
        return std::string();
    };
    page->refresh();
    page->copySetup()->click();
    CHECK(has(rig.clipboard.last(), "Ask me for the token"));
    page->setupKind()->setCurrentIndex(1);
    CHECK(has(page->setupHint()->text(), "Replace <PASTE_TOKEN>"));
    page->copySetup()->click();
    CHECK(text(rig.clipboard.last()) == plain(*page));
    CHECK(has(rig.clipboard.last(), "\"Authorization: Bearer <PASTE_TOKEN>\""));
}

TEST_CASE("test_tailscale_setup_uses_host_url_and_masks_token_on_screen", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    CHECK_FALSE(page->tailscaleEnabled()->isChecked());
    page->tailscaleEnabled()->setChecked(true);
    CHECK(rig.saved("mcp_tailscale_enabled") == Json(true));
    page->enabled()->setChecked(true);
    CHECK(rig.server.tailscaleStarts == std::vector<bool>{true});
    const std::string remote = "http://100.82.22.3:8767/mcp";
    rig.server.current = status("running", "Ready", fv::agentapi::DEFAULT_PORT, remote.c_str());
    page->refresh();
    CHECK_FALSE(page->tailscaleTile()->isEnabled());
    CHECK(text(page->url()->text()) == remote);
    CHECK(plain(*page).find("same Tailscale network") != std::string::npos);
    CHECK(plain(*page).find(kMcpToken) == std::string::npos);
    page->copySetup()->click();
    CHECK(has(rig.clipboard.last(), remote.c_str()));
    CHECK(has(rig.clipboard.last(), kMcpToken.c_str()));
    page->setupKind()->setCurrentIndex(2);
    CHECK(Json::parse(plain(*page)).at("mcpServers").at("fiorivoce").at("url") == remote);
    page->setupKind()->setCurrentIndex(3);
    CHECK(plain(*page) == fv::agentapi::chatgpt_config(remote, kHidden));
    CHECK(plain(*page).find(kMcpToken) == std::string::npos);
    page->agentLocation()->setCurrentIndex(0);
    const std::string local = rig.server.current.url();
    CHECK(text(page->url()->text()) == local);
    CHECK(plain(*page) == fv::agentapi::chatgpt_config(local, kHidden));
    page->setupKind()->setCurrentIndex(0);
    CHECK(plain(*page).find("Do not connect to or enable a different") != std::string::npos);
}

TEST_CASE("test_other_computer_setup_is_not_copyable_until_access_enabled", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    rig.server.current = status("running", "Ready", fv::agentapi::DEFAULT_PORT);
    page->agentLocation()->setCurrentIndex(1);
    page->refresh();
    CHECK_FALSE(page->copySetup()->isEnabled());
    CHECK_FALSE(page->copyUrl()->isEnabled());
    CHECK(plain(*page).find("Allow agents over Tailscale") != std::string::npos);
    CHECK(plain(*page).find("Register a server") == std::string::npos);
}

TEST_CASE("test_disclosures_keep_connection_formats_and_copy_actions_available", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    page->show();
    CHECK_FALSE(page->advanced()->body()->isVisible());
    CHECK(page->url()->isVisible());
    CHECK(page->copyUrl()->isVisible());
    page->advanced()->toggle()->click();
    CHECK(page->port()->isEnabled());
    CHECK(page->tailscaleEnabled()->isEnabled());
    page->enabled()->click();
    rig.server.current = status("running", "Ready", fv::agentapi::DEFAULT_PORT);
    page->refresh();
    CHECK_FALSE(page->port()->isEnabled());
    CHECK_FALSE(page->tailscaleEnabled()->isEnabled());
    page->advanced()->toggle()->click();
    CHECK_FALSE(page->advanced()->body()->isVisible());
    CHECK((page->url()->isVisible() && page->url()->isEnabled()));
    CHECK((page->copyUrl()->isVisible() && page->copyUrl()->isEnabled()));
    page->copyUrl()->click();
    CHECK(text(rig.clipboard.last()) == rig.server.current.url());
    const char* const labels[] = {"setup prompt", "Claude Code command", "Cursor JSON",
                                  "ChatGPT config"};
    for (int index = 0; index < 4; ++index) {
        page->setupKind()->buttons()[static_cast<std::size_t>(index)]->click();
        page->preview()->toggle()->setChecked(true);
        CHECK(page->setupText()->isVisible());
        CHECK(has(page->copySetup()->text(), labels[index]));
        CHECK(plain(*page).find(kMcpToken) == std::string::npos);
        page->copySetup()->click();
        // The copy is the preview with the real token in place of the mask.
        CHECK(text(rig.clipboard.last()) == withToken(plain(*page)));
    }
    page->copyToken()->click();
    CHECK(text(rig.clipboard.last()) == kMcpToken);
    CHECK(page->notice()->isVisible());
    CHECK(text(page->notice()->text()) == "Copied to clipboard.");
}

TEST_CASE("test_status_bar_state_drives_styling_and_escapes_messages", kTag) {
    McpRig rig;
    auto page = rig.makePage();
    CHECK(page->enableTile()->property("state").toString() == "off");
    rig.server.current = status("running", "Ready", fv::agentapi::DEFAULT_PORT);
    page->refresh();
    CHECK(page->enableTile()->property("state").toString() == "running");
    rig.server.current = status("error", "<b>Port</b> is busy.", fv::agentapi::DEFAULT_PORT);
    page->refresh();
    CHECK(page->enableTile()->property("state").toString() == "error");
    CHECK(has(page->statusLabel()->text(), "&lt;b&gt;Port"));
}

TEST_CASE("test_page_is_two_columns_at_settings_width_and_chips_wrap_in_balanced_rows", kTag) {
    // A 1,400 px single column was the complaint: keep the cards side by side.
    AppTheme theme;
    McpRig rig;
    auto page = rig.makePage();
    McpColumn column(*page);
    column.resize(1000, 900);
    CHECK(page->permissionsCard()->x() > page->connectionCard()->x());
    CHECK(page->sizeHint().height() < 800);
    column.resize(300, 900);
    std::set<int> rows;
    std::set<int> columns;
    for (const fv::SegmentButton* button : page->setupKind()->buttons()) {
        rows.insert(button->y());
        columns.insert(button->x());
    }
    CHECK(rows.size() == 2);
    CHECK(columns.size() == 2);
}

TEST_CASE("test_split_layout_and_individual_grants_survive_narrow_windows", kTag) {
    // Python also runs this with OPENWHISPER_UI=omarchy, a mode that is not ported.
    for (const int fontPercent : {100, 130}) {
        DYNAMIC_SECTION("font " << fontPercent << " %") {
            AppTheme theme("dark", fontPercent);
            McpRig rig;
            auto page = rig.makePage();
            McpColumn column(*page);
            column.resize(1300, 900);
            REQUIRE(page->permissionsCard()->x() > page->connectionCard()->x());
            CHECK_FALSE(page->preview()->body()->isVisible());
            page->preferences()->toggle()->click();
            page->permissionCheck("mcp_settings_access")->click();
            for (const auto& [name, group] : page->settingGroups()) {
                group->toggle()->click();
            }
            std::set<std::string> keysShown;
            for (const auto& [key, check] : page->settingChecks()) {
                keysShown.insert(key);
            }
            std::set<std::string> controls;
            for (const fv::agentapi::SettingControl& control : fv::agentapi::SETTING_CONTROLS()) {
                controls.insert(std::string(control.key));
            }
            CHECK(keysShown == controls);
            for (const auto& [key, check] : page->settingChecks()) {
                CHECK((check->isVisible() && check->isEnabled()));
            }
            page->settingCheck("ui_theme")->click();
            CHECK(has(page->permissionSummary()->text(), "1 allowed to change"));
            page->advanced()->toggle()->click();
            for (const int width : {680, 1300}) {
                column.resize(width, 900);
                CHECK(column.scroll->horizontalScrollBar()->maximum() == 0);
                if (width < 1000) {
                    CHECK(page->permissionsCard()->y() > page->connectionCard()->y());
                } else {
                    CHECK(page->permissionsCard()->x() > page->connectionCard()->x());
                }
                for (QAbstractButton* button : page->findChildren<QAbstractButton*>()) {
                    if (!button->isVisible()) {
                        continue;
                    }
                    const QPoint position = button->mapTo(page.get(), QPoint(0, 0));
                    CHECK(position.x() >= 0);
                    CHECK(position.x() + button->width() <= page->width());
                }
            }
            CHECK(rig.saved("mcp_writable_settings") == Json{{"ui_theme", true}});
            CHECK(rig.server.starts.empty());
            page->permissionCheck("mcp_settings_access")->click();
            CHECK_FALSE(page->settingCheck("ui_theme")->isEnabled());
            CHECK(page->settingCheck("ui_theme")->isChecked());
        }
    }
}

TEST_CASE("test_connection_text_fits_in_settings_container_at_all_font_sizes", kTag) {
    // A stretch below the page used to compress buttons below their text height. Python also
    // runs this with OPENWHISPER_UI=omarchy, a mode that is not ported.
    AppTheme theme;
    McpRig rig;
    rig.server.current = status("running", "Ready", fv::agentapi::DEFAULT_PORT);
    auto page = rig.makePage();
    page->refresh();
    McpColumn column(*page, true);
    for (const int width : {360, 480, 940, 1300}) {
        column.scroll->resize(width, 720);
        column.scroll->show();
        // Exercise live changes without resizing the window between scales.
        for (const int scale : {100, 130, 100}) {
            theme.setScale(scale);
            const int kinds = static_cast<int>(page->setupKind()->buttons().size());
            for (int kind = 0; kind < kinds; ++kind) {
                page->setupKind()->setCurrentIndex(kind);
                page->preview()->toggle()->setChecked(kind == 1);
                page->advanced()->toggle()->setChecked(kind == 2);
                McpColumn::settle();
                INFO("width " << width << ", scale " << scale << ", kind " << kind);
                CHECK(column.scroll->horizontalScrollBar()->maximum() == 0);
                std::vector<QAbstractButton*> buttons;
                for (QAbstractButton* button : page->agentLocation()->buttons()) {
                    buttons.push_back(button);
                }
                for (QAbstractButton* button : page->setupKind()->buttons()) {
                    buttons.push_back(button);
                }
                buttons.push_back(page->copySetup());
                buttons.push_back(page->copyToken());
                buttons.push_back(page->copyUrl());
                buttons.push_back(page->preview()->toggle());
                for (QAbstractButton* button : buttons) {
                    INFO(text(button->text()));
                    CHECK(button->width() >= button->sizeHint().width());
                    CHECK(button->height() >= button->sizeHint().height());
                    for (QLabel* label : button->findChildren<QLabel*>()) {
                        CHECK(label->width() >=
                              label->fontMetrics().horizontalAdvance(label->text()));
                        CHECK(label->height() >= label->fontMetrics().height());
                        CHECK(button->rect().contains(label->geometry()));
                    }
                }
                for (QLineEdit* field : {page->token(), page->url()}) {
                    CHECK(field->height() >= field->sizeHint().height());
                    CHECK(field->width() >= static_cast<int>(std::lround(160.0 * scale / 100)));
                }
            }
        }
    }
    page->setupKind()->buttons()[1]->setFocus();
    QTest::keyClick(page->setupKind()->buttons()[1], Qt::Key_Space);
    CHECK(page->setupKind()->currentIndex() == 1);
    int checked = 0;
    for (const fv::SegmentButton* button : page->setupKind()->buttons()) {
        checked += button->isChecked() ? 1 : 0;
    }
    CHECK(checked == 1);
    page->copySetup()->click();
    CHECK(has(rig.clipboard.last(), "claude mcp add"));
    CHECK(has(rig.clipboard.last(), kMcpToken.c_str()));
}
