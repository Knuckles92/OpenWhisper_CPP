// Settings > Automate > Hooks: webhooks, programs and text filters (tests/
// test_automation_ui_hooks_page.py, test_automation_fix_ui_hooks_answers.py and _results.py),
// the page inside a real Settings window over the fakes of actions_page_support.h, and a section
// on its own over those of hooks_support.h. Where Python asserts at once on something that comes
// back from a worker (a secret), the event loop turns first; an engine that answers on the
// calling thread is shown at once, as a Python Future that is already resolved is.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <latch>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>

#include "automation_support.h"
#include "hooks_support.h"
#include "settings/keys.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/hooks_format.h"
#include "ui_automation/hooks_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"

using fv::Json;
using namespace fvtest;
namespace hooks = fv::automation::hooks;
namespace hf = fv::hooks_format;

namespace {

const char* const kPage = "[test_automation_ui_hooks_page][widget]";
const char* const kAnswers = "[test_automation_fix_ui_hooks_answers][widget]";

const std::string kCompleted(cat::TRANSCRIPTION_COMPLETED);
const std::string kSaved(cat::HISTORY_SAVED);
const std::string kRan(cat::ACTION_RAN);

// The saved lists of the settings file.
Json hookSettings(std::vector<Json> webhooks, std::vector<Json> programs = {},
                  std::vector<Json> filters = {}) {
    Json settings = Json::object();
    if (!webhooks.empty()) {
        settings["automation_webhooks"] = Json(std::move(webhooks));
    }
    if (!programs.empty()) {
        settings["automation_programs"] = Json(std::move(programs));
    }
    if (!filters.empty()) {
        settings["automation_filters"] = Json(std::move(filters));
    }
    return settings;
}

fv::HooksPage& hooksPage(ActionsRig& rig) {
    auto* content = dynamic_cast<fv::HooksPage*>(rig.current().pageContent(dest::hooks));
    REQUIRE(content != nullptr);
    return *content;
}

// The webhook as it is saved now.
std::optional<hooks::Webhook> savedHook(HooksRig& rig, const std::string& id) {
    return hooks::find_webhook(rig.stored(), id);
}

std::vector<std::string> ids(const std::vector<fv::HookEntry>& entries) {
    std::vector<std::string> found;
    for (const fv::HookEntry& entry : entries) {
        found.push_back(entry.id);
    }
    return found;
}

// A section's `changed` signals, as the page's tile gets them.
struct Changes {
    template <class Section>
    Changes(Section& section, std::string kindName) : kind(std::move(kindName)) {
        QObject::connect(&section, &fv::HookSection::changed, &section,
                         [this] { seen.push_back(kind); });
    }
    std::string kind;
    std::vector<std::string> seen;
};

} // namespace

// ---- the page module ----

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_page_builds_lazily_with_a_tile_per_search_field
TEST_CASE("test_page_builds_lazily_with_a_tile_per_search_field", kPage) {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog(false);
    CHECK_FALSE(dialog.isBuilt(dest::hooks));
    CHECK(dialog.rail()->value(qs(dest::hooks)) == QStringLiteral("None yet"));
    dialog.selectDestination(qs(dest::hooks));
    CHECK(dialog.isBuilt(dest::hooks));
    CHECK(dialog.pageTitleLabel()->text() == qs(fv::hooks_page::TITLE));
    fv::HooksPage& page = hooksPage(rig);
    const std::vector<QWidget*> tiles{page.webhooksTile(), page.programsTile(), page.filtersTile()};
    const auto& fields = fv::hooks_page::search_fields();
    REQUIRE(fields.size() == tiles.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        INFO(fields[i].target);
        CHECK(page.isAncestorOf(tiles[i]));
        CHECK(static_cast<fv::InfoTile*>(tiles[i])->titleLabel()->text() == qs(fields[i].title));
        CHECK(page.namedControl(fields[i].target) == tiles[i]);
    }
    for (const std::string& name : fv::hooks_page::control_names()) {
        INFO(name);
        CHECK(page.namedControl(name) != nullptr);
        CHECK(dialog.control(name) == page.namedControl(name));
        CHECK(rig.context().settingsPages().controlDestination(name) == dest::hooks);
    }
}

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_a_section_attribute_builds_the_page
TEST_CASE("test_a_section_attribute_builds_the_page", kPage) {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog(false);
    QWidget* section = dialog.control("hooks_programs");
    REQUIRE(section != nullptr);
    CHECK(dialog.isBuilt(dest::hooks));
    CHECK(dialog.pageContent(dest::hooks)->isAncestorOf(section));
}

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_rail_value_counts_hooks_and_those_switched_off
TEST_CASE("test_rail_value_counts_hooks_and_those_switched_off",
          "[test_automation_ui_hooks_page][unit]") {
    using fv::hooks_page::rail_value;
    CHECK(rail_value(Json::object()) == "None yet");
    CHECK(rail_value(hookSettings({kHook})) == "1 hook");
    CHECK(rail_value(hookSettings({kHook}, {programRow()}, {kFilter})) == "3 hooks · 2 on");
}

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_saves_notify_the_app_and_redraw_the_rail
TEST_CASE("test_saves_notify_the_app_and_redraw_the_rail", kPage) {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog(false);
    auto* webhooks = dynamic_cast<fv::WebhookSection*>(dialog.control("hooks_webhooks"));
    auto* programs = dynamic_cast<fv::ProgramSection*>(dialog.control("hooks_programs"));
    auto* filters = dynamic_cast<fv::FilterSection*>(dialog.control("hooks_filters"));
    REQUIRE((webhooks && programs && filters));
    webhooks->openEditor();
    webhooks->webhookEditor()->nameEdit()->setText(QStringLiteral("Home"));
    webhooks->webhookEditor()->urlEdit()->setText(QStringLiteral("https://example.com/in"));
    REQUIRE(webhooks->save());
    CHECK(rig.changes == QStringList{QStringLiteral("webhooks")});
    CHECK(dialog.rail()->value(qs(dest::hooks)) == QStringLiteral("1 hook"));
    programs->openEditor();
    programs->programEditor()->nameEdit()->setText(QStringLiteral("Journal"));
    programs->programEditor()->programEdit()->setText(
        QDir::toNativeSeparators(QFileInfo("journal.exe").absoluteFilePath()));
    REQUIRE(programs->save());
    filters->openEditor();
    filters->filterEditor()->nameEdit()->setText(QStringLiteral("Names"));
    filters->filterEditor()->urlEdit()->setText(QStringLiteral("http://127.0.0.1:5005/fix"));
    REQUIRE(filters->save());
    CHECK(rig.changes == QStringList{QStringLiteral("webhooks"), QStringLiteral("programs"),
                                     QStringLiteral("filters")});
    CHECK(hooks::load_filters(*rig.settings().snapshot()).size() == 1);
}

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_load_and_refresh_reread_saved_hooks
TEST_CASE("test_load_and_refresh_reread_saved_hooks", kPage) {
    ActionsRig rig(hookSettings({kHook}));
    fv::SettingsDialog& dialog = rig.dialog(false);
    auto* webhooks = dynamic_cast<fv::WebhookSection*>(dialog.control("hooks_webhooks"));
    auto* programs = dynamic_cast<fv::ProgramSection*>(dialog.control("hooks_programs"));
    REQUIRE((webhooks && programs));
    CHECK(ids(webhooks->entries()) == std::vector<std::string>{"hook01"});
    rig.settings().mutate(
        [](Json& settings) { settings["automation_programs"] = Json::array({programRow()}); });
    dialog.refreshPage(dest::hooks);
    CHECK(ids(programs->entries()) == std::vector<std::string>{"prog01"});
}

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_leaving_the_page_saves_a_valid_draft_and_asks_about_an_invalid_one
TEST_CASE("test_leaving_the_page_saves_a_valid_draft_and_asks_about_an_invalid_one", kPage) {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog(false);
    auto* section = dynamic_cast<fv::WebhookSection*>(dialog.control("hooks_webhooks"));
    REQUIRE(section != nullptr);
    fv::HooksPage& page = hooksPage(rig);
    section->openEditor();
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Draft"));
    section->webhookEditor()->urlEdit()->setText(QStringLiteral("https://example.com/hook"));
    CHECK(page.saveDrafts(true));
    const auto saved = hooks::load_webhooks(*rig.settings().snapshot());
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].name == "Draft");

    section->openEditor();
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("No address"));
    rig.answer = QMessageBox::Cancel;
    CHECK_FALSE(page.saveDrafts(true));
    REQUIRE_FALSE(rig.questions.empty());
    CHECK(rig.questions.back().second.contains(QStringLiteral("Enter the address to send to.")));
    CHECK(section->hasUnsavedChanges());
    rig.answer = QMessageBox::Discard;
    CHECK(page.saveDrafts(true));
    CHECK(section->webhookEditor()->isHidden());
    CHECK(hooks::load_webhooks(*rig.settings().snapshot()).size() == 1);
}

// from
// tests/test_automation_ui_hooks_page.py::TestPageModule::test_open_api_link_goes_to_api_and_mcp
TEST_CASE("test_open_api_link_goes_to_api_and_mcp", kPage) {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog(false);
    dialog.selectDestination(qs(dest::hooks));
    auto* filters = dynamic_cast<fv::FilterSection*>(dialog.control("hooks_filters"));
    REQUIRE(filters != nullptr);
    filters->leases()->openApi()->click();
    CHECK(dialog.currentDestination() == qs(dest::mcp));
}

// ---- webhooks ----

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_add_edit_and_delete_persist_and_forget_the_secret
TEST_CASE("test_add_edit_and_delete_persist_and_forget_the_secret", kPage) {
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    Changes changes(*section, "webhook");
    CHECK_FALSE(section->emptyState()->isHidden());
    CHECK(section->countLabel()->isHidden());
    section->addButton()->click();
    fv::WebhookEditor& editor = *section->webhookEditor();
    CHECK_FALSE(editor.isHidden());
    CHECK(section->emptyState()->isHidden());
    CHECK(editor.deleteButton()->isHidden());
    CHECK(editor.extras()->isHidden());
    CHECK(editor.events()->events() == std::vector<std::string>{kCompleted});
    editor.nameEdit()->setText(QStringLiteral("  Home  "));
    editor.urlEdit()->setText(QStringLiteral("https://example.com/in"));
    editor.events()->chip(kSaved)->click();
    editor.saveButton()->click();
    const auto saved = rig.savedWebhooks();
    REQUIRE(saved.size() == 1);
    const hooks::Webhook hook = saved[0];
    CHECK(hook.name == "Home");
    CHECK(hook.url == "https://example.com/in");
    CHECK(hook.enabled);
    CHECK(hook.events == std::vector<std::string>{kCompleted, kSaved});
    CHECK(changes.seen == std::vector<std::string>{"webhook"});
    CHECK(editor.message()->text() == QStringLiteral("Saved."));
    CHECK_FALSE(editor.extras()->isHidden());
    CHECK_FALSE(editor.deleteButton()->isHidden());
    CHECK(section->countLabel()->text() == QStringLiteral("1 / %1").arg(hooks::MAX_WEBHOOKS));
    fv::HookRow* row = section->row(hook.id);
    REQUIRE(row != nullptr);
    CHECK(row->property("selected").toBool());
    CHECK(row->detail()->text().contains(QStringLiteral("example.com")));

    editor.urlEdit()->setText(QStringLiteral("https://example.org/new"));
    CHECK(section->hasUnsavedChanges());
    section->refresh(); // a draft in progress survives a refresh
    CHECK(editor.urlEdit()->text() == QStringLiteral("https://example.org/new"));
    CHECK(section->save());
    CHECK(savedHook(rig, hook.id)->url == "https://example.org/new");
    CHECK(rig.savedWebhooks().size() == 1);

    rig.answers.push_back(QMessageBox::Yes);
    editor.deleteButton()->click();
    CHECK(rig.savedWebhooks().empty());
    CHECK(waitFor(
        [&] { return rig.webhooks.secrets_deleted() == std::vector<std::string>{hook.id}; }));
    CHECK(editor.isHidden());
    CHECK_FALSE(section->emptyState()->isHidden());
    CHECK(changes.seen == std::vector<std::string>{"webhook", "webhook", "webhook"});
}

// from tests/test_automation_ui_hooks_page.py::TestWebhooks::test_delete_can_be_called_off
TEST_CASE("test_delete_can_be_called_off", kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    Changes changes(*section, "webhook");
    section->openEditor("hook01");
    section->webhookEditor()->deleteButton()->click(); // answered Cancel
    CHECK(rig.savedWebhooks().size() == 1);
    CHECK(changes.seen.empty());
}

// from tests/test_automation_ui_hooks_page.py::TestWebhooks::test_validation_messages_show_inline
TEST_CASE("test_validation_messages_show_inline", kPage) {
    struct Case {
        const char* name;
        const char* url;
        bool events;
        const char* message;
    };
    const std::vector<Case> cases{
        {"", "https://example.com", true, "Give the webhook a name."},
        {"Home", "", true, "Enter the address to send to."},
        {"Home", "ftp://example.com", true, "Use an address that starts with http:// or https://."},
        {"Home", "https://example.com", false, "Choose at least one event."},
    };
    for (const Case& entry : cases) {
        INFO(entry.message);
        HooksRig rig;
        auto section = rig.section<fv::WebhookSection>();
        Changes changes(*section, "webhook");
        section->openEditor();
        fv::WebhookEditor& editor = *section->webhookEditor();
        editor.nameEdit()->setText(QString::fromUtf8(entry.name));
        editor.urlEdit()->setText(QString::fromUtf8(entry.url));
        if (!entry.events) {
            editor.events()->setEvents({});
        }
        CHECK_FALSE(section->save());
        CHECK(editor.message()->text() == QString::fromUtf8(entry.message));
        CHECK(editor.message()->property("tone").toString() == QStringLiteral("error"));
        CHECK(rig.savedWebhooks().empty());
        CHECK(changes.seen.empty());
    }
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_duplicate_names_and_the_limit_are_refused
TEST_CASE("test_duplicate_names_and_the_limit_are_refused", kPage) {
    std::vector<Json> rows;
    for (std::size_t i = 0; i < hooks::MAX_WEBHOOKS; ++i) {
        Json row = kHook;
        row["id"] = "h" + std::to_string(i);
        row["name"] = "Hook " + std::to_string(i);
        rows.push_back(row);
    }
    HooksRig rig(hookSettings(rows));
    auto section = rig.section<fv::WebhookSection>();
    CHECK_FALSE(section->addButton()->isEnabled());
    CHECK(section->addButton()->toolTip().contains(QStringLiteral("up to 20")));
    section->openEditor("h1");
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("hook 0"));
    CHECK_FALSE(section->save());
    CHECK(section->webhookEditor()->message()->text() ==
          QStringLiteral("A webhook with that name already exists."));
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_plain_http_beyond_this_network_gets_a_gentle_note
TEST_CASE("test_plain_http_beyond_this_network_gets_a_gentle_note", kPage) {
    const std::vector<std::pair<const char*, bool>> cases{
        {"http://93.184.216.34/hook", true},     {"http://hooks.example.com/in", true},
        {"https://hooks.example.com/in", false}, {"http://127.0.0.1:8080/in", false},
        {"http://192.168.1.20/in", false},       {"http://homeassistant.local:8123/api", false},
    };
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor();
    for (const auto& [url, warned] : cases) {
        INFO(url);
        section->webhookEditor()->urlEdit()->setText(QString::fromUtf8(url));
        CHECK(section->webhookEditor()->urlNote()->isHidden() == !warned);
        if (warned) {
            CHECK(section->webhookEditor()->urlNote()->text().contains(
                QStringLiteral("unencrypted")));
        }
    }
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_the_row_switch_saves_and_the_editor_keeps_it
TEST_CASE("test_the_row_switch_saves_and_the_editor_keeps_it", kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    Changes changes(*section, "webhook");
    section->openEditor("hook01");
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Renamed"));
    section->row("hook01")->switchButton()->click();
    CHECK_FALSE(savedHook(rig, "hook01")->enabled);
    CHECK(section->row("hook01")->detail()->text().startsWith(QStringLiteral("Off · ")));
    CHECK(section->webhookEditor()->nameEdit()->text() == QStringLiteral("Renamed"));
    CHECK(section->save());
    const auto saved = savedHook(rig, "hook01");
    REQUIRE(saved.has_value());
    CHECK(saved->name == "Renamed");
    CHECK_FALSE(saved->enabled);
    CHECK(changes.seen == std::vector<std::string>{"webhook", "webhook"});
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_a_failed_switch_save_shows_why_and_keeps_the_saved_state
TEST_CASE("test_a_failed_switch_save_shows_why_and_keeps_the_saved_state", kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    Changes changes(*section, "webhook");
    rig.store().file().fail_next_replace("disk is read-only");
    section->row("hook01")->switchButton()->click();
    CHECK(section->row("hook01")->switchButton()->isChecked());
    CHECK(section->message()->text().contains(QStringLiteral("read-only")));
    CHECK(changes.seen.empty());
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_opening_another_hook_asks_to_save_the_draft
TEST_CASE("test_opening_another_hook_asks_to_save_the_draft", kPage) {
    Json second = kHook;
    second["id"] = "hook02";
    second["name"] = "Office";
    HooksRig rig(hookSettings({kHook, second}));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("hook01");
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Home 2"));
    rig.answers.push_back(QMessageBox::Cancel);
    CHECK_FALSE(section->openEditor("hook02"));
    CHECK(section->editingId() == "hook01");
    rig.answers.push_back(QMessageBox::Save);
    CHECK(section->openEditor("hook02"));
    CHECK(section->editingId() == "hook02");
    CHECK(savedHook(rig, "hook01")->name == "Home 2");
    REQUIRE(rig.questions.size() == 2);
    CHECK(rig.questions[0].first == QStringLiteral("Unsaved webhook"));
    CHECK(rig.questions[1].first == QStringLiteral("Unsaved webhook"));
    // The editor sits under the row it edits.
    QVBoxLayout* layout = section->listLayout();
    CHECK(layout->indexOf(section->editor()) == layout->indexOf(section->row("hook02")) + 1);
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_send_test_shows_the_result_and_recent_deliveries
TEST_CASE("test_send_test_shows_the_result_and_recent_deliveries", kPage) {
    HooksRig rig(hookSettings({kHook}));
    rig.webhooks.set_test_answer(record(200, 120));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("hook01");
    fv::WebhookEditor& editor = *section->webhookEditor();
    REQUIRE(editor.recent()->shown().has_value());
    CHECK(editor.recent()->shown()->empty());
    rig.webhooks.set_deliveries(
        "hook01", {record(200, 118, true, "", std::string(cat::TRANSCRIPTION_COMPLETED)),
                   record(502, 1430, false, "Bad gateway", "webhook.test", 3)});
    editor.testButton()->click();
    CHECK(rig.webhooks.tests_sent() == std::vector<std::string>{"hook01"});
    CHECK(editor.testChip()->text() == QStringLiteral("200 · 120 ms"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("ok"));
    const std::vector<fv::RecentRow> rows = *editor.recent()->shown();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].what == QStringLiteral("Finished dictations"));
    CHECK(rows[0].result == QStringLiteral("200 · 118 ms"));
    CHECK(rows[0].ok);
    CHECK(rows[1].what == QStringLiteral("Test"));
    CHECK(rows[1].result == QStringLiteral("502 · 1430 ms · try 3"));
    CHECK_FALSE(rows[1].ok);
    section->refreshLive();
    fv::HookRow* row = section->row("hook01");
    CHECK(row->detail()->text().endsWith(QStringLiteral("Last delivery 200 · 118 ms")));
    CHECK(row->dot()->status() == fv::EngineStatus::Ready);
}

// from tests/test_automation_ui_hooks_page.py::TestWebhooks::test_send_test_reports_errors_honestly
TEST_CASE("test_send_test_reports_errors_honestly", kPage) {
    HooksRig rig(hookSettings({kHook}));
    ScriptedWebhooks engine;
    fv::HookServices services = rig.services();
    services.webhooks = &engine;
    auto section = rig.section<fv::WebhookSection>(services);
    section->openEditor("hook01");
    fv::WebhookEditor& editor = *section->webhookEditor();
    // An engine that can't send says why in its own words, on the line under the button (an
    // error is too long for the chip); no fake success.
    fv::WrappedLabel* errorLine = editor.testChip()->errorLine();
    engine.onSendTest = [](const std::string&,
                           std::function<void(fv::automation::DeliveryRecord)>) {
        throw std::runtime_error("Saved webhooks changed; try again.");
    };
    editor.testButton()->click();
    CHECK(errorLine->text() == QStringLiteral("Saved webhooks changed; try again."));
    CHECK(errorLine->property("tone").toString() == QStringLiteral("error"));
    CHECK(editor.testChip()->isHidden());
    CHECK(editor.testButton()->isEnabled());

    engine.onSendTest = nullptr;
    engine.fake.set_test_answer(record(std::nullopt, 5000, false, "Timed out"));
    editor.testButton()->click();
    CHECK(errorLine->text() == QStringLiteral("Timed out"));
    CHECK_FALSE(errorLine->isHidden());
    engine.fake.set_test_answer(record(404, 80, false));
    editor.testButton()->click();
    CHECK(editor.testChip()->text() == QStringLiteral("404 · 80 ms"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("error"));
    CHECK(errorLine->isHidden());
    engine.fake.set_test_answer(record(200, 95), true);
    editor.testButton()->click();
    CHECK(editor.testChip()->text() == QStringLiteral("Sending…"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("busy"));
    CHECK_FALSE(editor.testButton()->isEnabled());
    engine.fake.release_tests();
    CHECK(waitFor([&] { return editor.testChip()->text() == QStringLiteral("200 · 95 ms"); }));
    CHECK(editor.testButton()->isEnabled());
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_send_test_saves_a_changed_draft_first
TEST_CASE("test_send_test_saves_a_changed_draft_first", kPage) {
    HooksRig rig(hookSettings({kHook}));
    rig.webhooks.set_test_answer(record());
    auto section = rig.section<fv::WebhookSection>();
    Changes changes(*section, "webhook");
    section->openEditor("hook01");
    section->webhookEditor()->urlEdit()->setText(QStringLiteral("https://example.com/v2"));
    section->webhookEditor()->testButton()->click();
    CHECK(savedHook(rig, "hook01")->url == "https://example.com/v2");
    CHECK(changes.seen == std::vector<std::string>{"webhook"});
    section->webhookEditor()->urlEdit()->setText(QStringLiteral("not a url"));
    section->webhookEditor()->testButton()->click();
    CHECK((section->webhookEditor()->testChip()->isHidden() ||
           section->webhookEditor()->testChip()->text() == QStringLiteral("200 · 120 ms")));
    CHECK(section->webhookEditor()->message()->text().contains(QStringLiteral("http://")));
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_copy_and_new_secret_go_through_the_engine
TEST_CASE("test_copy_and_new_secret_go_through_the_engine", kPage) {
    // What the workers reach is declared before the rig, whose io pool waits for them.
    ScriptedWebhooks engine;
    std::mutex callsMutex;
    std::vector<std::pair<std::string, std::string>> calls;
    engine.onEnsure = [&](const std::string& id) {
        std::lock_guard lock(callsMutex);
        calls.emplace_back("ensure", id);
        return std::string("whsec_first");
    };
    engine.onRotate = [&](const std::string& id) {
        std::lock_guard lock(callsMutex);
        calls.emplace_back("rotate", id);
        return std::string("whsec_second");
    };
    HooksRig rig(hookSettings({kHook}));
    fv::HookServices services = rig.services();
    services.webhooks = &engine;
    auto section = rig.section<fv::WebhookSection>(services);
    section->openEditor("hook01");
    fv::WebhookEditor& editor = *section->webhookEditor();
    editor.copySecretButton()->click();
    CHECK(waitFor([&] { return editor.secretChip()->text() == QStringLiteral("Copied"); }));
    CHECK(rig.copied == QStringList{QStringLiteral("whsec_first")});
    editor.rotateSecretButton()->click(); // answered Cancel
    {
        std::lock_guard lock(callsMutex);
        CHECK(calls.size() == 1);
    }
    rig.answers.push_back(QMessageBox::Yes);
    editor.rotateSecretButton()->click();
    CHECK(waitFor(
        [&] { return editor.secretChip()->text() == QStringLiteral("New secret copied"); }));
    CHECK(rig.copied == QStringList{QStringLiteral("whsec_first"), QStringLiteral("whsec_second")});
    {
        std::lock_guard lock(callsMutex);
        CHECK(calls == std::vector<std::pair<std::string, std::string>>{{"ensure", "hook01"},
                                                                        {"rotate", "hook01"}});
    }
    // The secret never reaches settings.
    CHECK(rig.stored().dump().find("whsec_") == std::string::npos);
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_secret_errors_show_under_the_buttons
TEST_CASE("test_secret_errors_show_under_the_buttons", kPage) {
    HooksRig rig(hookSettings({kHook}));
    // The engine's SecretUnavailable carries user-facing copy.
    rig.webhooks.fail_secrets("Your system's credential store is locked.");
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("hook01");
    section->webhookEditor()->copySecretButton()->click();
    fv::WrappedLabel* errorLine = section->webhookEditor()->secretChip()->errorLine();
    CHECK(waitFor([&] { return !errorLine->isHidden(); }));
    CHECK(errorLine->text() == QStringLiteral("Your system's credential store is locked."));
    CHECK(section->webhookEditor()->secretChip()->isHidden());
    CHECK(section->webhookEditor()->copySecretButton()->isEnabled());
}

// from
// tests/test_automation_ui_hooks_page.py::TestWebhooks::test_cancel_closes_a_new_draft_without_saving
TEST_CASE("test_cancel_closes_a_new_draft_without_saving", kPage) {
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    Changes changes(*section, "webhook");
    section->openEditor();
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Never saved"));
    section->webhookEditor()->cancelButton()->click();
    CHECK(section->webhookEditor()->isHidden());
    CHECK_FALSE(section->hasUnsavedChanges());
    CHECK(rig.savedWebhooks().empty());
    CHECK(changes.seen.empty());
}

// ---- programs ----

// from
// tests/test_automation_ui_hooks_page.py::TestPrograms::test_add_with_the_file_picker_arguments_and_time_limit
TEST_CASE("test_add_with_the_file_picker_arguments_and_time_limit", kPage) {
    const QString chosen =
        QDir::toNativeSeparators(QFileInfo(QStringLiteral("tools/log.exe")).absoluteFilePath());
    auto& chooser = fv::action_fields::chooseProgram();
    const auto previous = chooser;
    chooser = [&](QWidget*, const QString&, const QString&) {
        return chosen;
    };
    HooksRig rig;
    auto section = rig.section<fv::ProgramSection>();
    Changes changes(*section, "program");
    section->openEditor();
    fv::ProgramEditor& editor = *section->programEditor();
    editor.nameEdit()->setText(QStringLiteral("Log"));
    editor.chooseButton()->click();
    CHECK(editor.programEdit()->text() == chosen);
    editor.argsEdit()->setPlainText(QStringLiteral("--quiet\n\n  --tag=voice  \n"));
    editor.timeout()->setValue(45);
    editor.events()->chip(kRan)->click();
    CHECK(section->save());
    chooser = previous;
    const auto saved = rig.savedPrograms();
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].program == text(chosen));
    CHECK(saved[0].args == std::vector<std::string>{"--quiet", "--tag=voice"});
    CHECK(saved[0].timeout_s == 45);
    CHECK(saved[0].events == std::vector<std::string>{kCompleted, kRan});
    CHECK(changes.seen == std::vector<std::string>{"program"});
    CHECK(section->row(saved[0].id)->detail()->text().contains(QStringLiteral("log.exe")));
}

// from
// tests/test_automation_ui_hooks_page.py::TestPrograms::test_a_relative_program_path_is_refused
TEST_CASE("test_a_relative_program_path_is_refused", kPage) {
    HooksRig rig;
    auto section = rig.section<fv::ProgramSection>();
    section->openEditor();
    section->programEditor()->nameEdit()->setText(QStringLiteral("Log"));
    section->programEditor()->programEdit()->setText(QStringLiteral("log.exe"));
    CHECK_FALSE(section->save());
    CHECK(section->programEditor()->message()->text() ==
          QStringLiteral("Choose the program with its full path."));
}

// from tests/test_automation_ui_hooks_page.py::TestPrograms::test_run_test_and_recent_runs
TEST_CASE("test_run_test_and_recent_runs", kPage) {
    HooksRig rig(hookSettings({}, {programRow()}));
    ScriptedPrograms engine;
    const std::vector<fv::automation::RunRecord> runs{
        runRecord("evt_2", "2026-10-10T18:03:00Z", true, 0, 340),
        runRecord("evt_1", "2026-10-10T17:40:00Z", false, std::nullopt, 30000,
                  "Timed out after 30 s")};
    engine.fake.set_runs("prog01", runs);
    engine.fake.set_test_answer(runs[0]);
    fv::HookServices services = rig.services();
    services.programs = &engine;
    auto section = rig.section<fv::ProgramSection>(services);
    section->openEditor("prog01");
    fv::ProgramEditor& editor = *section->programEditor();
    editor.testButton()->click();
    CHECK(editor.testChip()->text() == QStringLiteral("Exit 0 · 340 ms"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("ok"));
    const std::vector<fv::RecentRow> rows = *editor.recent()->shown();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].result == QStringLiteral("Exit 0 · 340 ms"));
    CHECK(rows[0].ok);
    CHECK(rows[0].what == QStringLiteral("Ran"));
    CHECK(rows[1].result == QStringLiteral("Timed out after 30 s"));
    CHECK_FALSE(rows[1].ok);
    CHECK(rows[1].what == QStringLiteral("Failed"));
    engine.fake.set_test_answer(runRecord("evt_3", "2026-10-10T18:04:00Z", false, 2, 80));
    editor.testButton()->click();
    CHECK(editor.testChip()->text() == QStringLiteral("Exit 2 · 80 ms"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("error"));
    engine.onRunTest = [](const std::string&, std::function<void(fv::automation::RunRecord)>) {
        throw std::runtime_error("Programs aren't available yet.");
    };
    editor.testButton()->click();
    CHECK(editor.testChip()->errorLine()->text() ==
          QStringLiteral("Programs aren't available yet."));
}

// ---- text filters ----

// from
// tests/test_automation_ui_hooks_page.py::TestFilters::test_web_address_and_program_filters_save_only_their_own_fields
TEST_CASE("test_web_address_and_program_filters_save_only_their_own_fields", kPage) {
    HooksRig rig;
    auto section = rig.section<fv::FilterSection>();
    Changes changes(*section, "filter");
    section->openEditor();
    fv::FilterEditor& editor = *section->filterEditor();
    CHECK(editor.kindBar()->currentIndex() == 0);
    CHECK_FALSE(editor.urlEdit()->isHidden());
    CHECK(editor.programRow()->isHidden());
    CHECK(editor.replyHint()->text().contains(QStringLiteral("posts the words as JSON")));
    editor.nameEdit()->setText(QStringLiteral("Names"));
    editor.urlEdit()->setText(QStringLiteral("http://127.0.0.1:5005/fix"));
    editor.programEdit()->setText(
        QDir::toNativeSeparators(QFileInfo("ignored.exe").absoluteFilePath()));
    editor.stageBar()->setCurrentIndex(0);
    editor.timeout()->setValue(800);
    CHECK(section->save());
    auto saved = rig.savedFilters();
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].kind == "http");
    CHECK(saved[0].stage == std::string(hooks::BEFORE_CLEANUP));
    CHECK(saved[0].url == "http://127.0.0.1:5005/fix");
    CHECK(saved[0].program.empty());
    CHECK(saved[0].timeout_ms == 800);
    const std::string id = saved[0].id;

    editor.kindBar()->buttons()[1]->click();
    CHECK(editor.urlEdit()->isHidden());
    CHECK_FALSE(editor.programRow()->isHidden());
    CHECK(editor.replyHint()->text().contains(QStringLiteral("program's input")));
    editor.programEdit()->setText(
        QDir::toNativeSeparators(QFileInfo("swearjar.exe").absoluteFilePath()));
    editor.argsEdit()->setPlainText(QStringLiteral("--strict"));
    CHECK(section->save());
    const auto program = hooks::find_filter(rig.stored(), id);
    REQUIRE(program.has_value());
    CHECK(program->kind == "program");
    CHECK(program->url.empty());
    CHECK(program->args == std::vector<std::string>{"--strict"});
    CHECK(changes.seen == std::vector<std::string>{"filter", "filter"});
    CHECK(section->row(id)->detail()->text().contains(
        QStringLiteral("swearjar.exe · Before AI cleanup · 800 ms")));
    CHECK(section->row(id)->dot()->isHidden());
}

// from tests/test_automation_ui_hooks_page.py::TestFilters::test_time_limit_validation
TEST_CASE("test_time_limit_validation", kPage) {
    HooksRig rig;
    auto section = rig.section<fv::FilterSection>();
    section->openEditor();
    fv::FilterEditor& editor = *section->filterEditor();
    CHECK(editor.timeout()->minimum() == hooks::FILTER_TIMEOUT_MIN_MS);
    CHECK(editor.timeout()->maximum() == hooks::FILTER_TIMEOUT_MAX_MS);
    CHECK(editor.timeout()->value() == hooks::FILTER_TIMEOUT_DEFAULT_MS);
    editor.nameEdit()->setText(QStringLiteral("Names"));
    CHECK_FALSE(section->save());
    CHECK(editor.message()->text() == QStringLiteral("Enter the address to send to."));
}

// from
// tests/test_automation_ui_hooks_page.py::TestFilters::test_leased_filters_show_read_only_with_their_expiry
TEST_CASE("test_leased_filters_show_read_only_with_their_expiry", kPage) {
    HooksRig rig;
    // The first lease lapses before the second is shown; the clock the page and the leases share
    // moves only when told.
    rig.leases().register_lease("http://127.0.0.1:7071/f", std::string(hooks::BEFORE_CLEANUP));
    rig.clock().advance(std::chrono::seconds(30));
    const fv::automation::Lease kept =
        rig.leases().register_lease("http://127.0.0.1:7070/f", std::string(hooks::AFTER_CLEANUP));
    rig.clock().advance(std::chrono::seconds(31));
    auto section = rig.section<fv::FilterSection>();
    fv::LeaseList& list = *section->leases();
    REQUIRE(list.shown().has_value());
    REQUIRE(list.shown()->size() == 1);
    const auto [id, url, stage, seconds] = (*list.shown())[0];
    CHECK(url == "http://127.0.0.1:7070/f");
    CHECK(stage == std::string(hooks::AFTER_CLEANUP));
    CHECK(seconds == 29);
    CHECK(list.note()->isHidden());
    CHECK(list.openApi()->isHidden());
    REQUIRE(list.lines().size() == 1);
    QFrame* line = list.lines()[0];
    CHECK(list.lineDetail(line)->text() == QStringLiteral("After AI cleanup · ends in 29 s"));
    // A renewal moves the countdown on the same row.
    REQUIRE(rig.leases().renew_lease(kept.id).has_value());
    rig.clock().advance(std::chrono::milliseconds(39600));
    section->refreshLive();
    REQUIRE(list.lines().size() == 1);
    CHECK(list.lines()[0] == line);
    CHECK(list.lineDetail(line)->text() == QStringLiteral("After AI cleanup · ends in 20 s"));
    rig.leases().drop_all_leases();
    section->refreshLive();
    REQUIRE(list.shown().has_value());
    CHECK(list.shown()->empty());
    CHECK_FALSE(list.note()->isHidden());
}

// from tests/test_automation_ui_hooks_page.py::test_a_secret_worker_never_keeps_the_editor_alive
TEST_CASE("test_a_secret_worker_never_keeps_the_editor_alive", kPage) {
    // Workers answer through the requests; deleting Settings mid-request is safe. What the worker
    // reaches is declared before the rig, whose io pool waits for it when the test ends.
    ScriptedWebhooks engine;
    std::latch release(1);
    std::atomic<bool> finished = false;
    HooksRig rig(hookSettings({kHook}));
    engine.onEnsure = [&](const std::string&) {
        release.wait();
        finished = true;
        return std::string("whsec_late");
    };
    fv::HookServices services = rig.services();
    services.webhooks = &engine;
    {
        auto section = rig.section<fv::WebhookSection>(services);
        section->openEditor("hook01");
        section->webhookEditor()->copySecretButton()->click();
        // The widget goes while the worker still holds the secret.
        pump(2);
    }
    pump(4);
    release.count_down();
    CHECK(waitFor([&] { return finished.load(); }));
    pump(); // the late answer finds no editor and is dropped
    CHECK(rig.copied.isEmpty());
}

// ---- helpers and layout ----

// from tests/test_automation_ui_hooks_page.py::test_result_text_and_clock
TEST_CASE("test_result_text_and_clock", "[test_automation_ui_hooks_page][unit]") {
    CHECK(hf::delivery_result(record(200, 120)) == hf::Outcome{"200 · 120 ms", true});
    CHECK(hf::delivery_result(record(std::nullopt, 0, false, "")) ==
          hf::Outcome{"No answer", false});
    CHECK(hf::events_summary({}) == "No events");
    CHECK(hf::events_summary({kRan, kSaved}) == "Actions that ran +1");
    CHECK(hf::clock("not a time") == "not a time");
    CHECK(hf::clock("2026-10-10T18:02:11.120Z").size() == 8);
    const std::vector<std::string> types = hf::event_types();
    const auto& published = cat::PUBLIC_TYPES();
    CHECK(std::set<std::string>(types.begin(), types.end()) ==
          std::set<std::string>(published.begin(), published.end()));
}

// from tests/test_automation_ui_hooks_page.py::test_sections_fit_narrow_columns_at_every_scale
TEST_CASE("test_sections_fit_narrow_columns_at_every_scale", kPage) {
    // OpenWhisper runs it for its classic and Omarchy looks; the Omarchy palette is not ported
    // and reads as dark, so the two scales run once each.
    for (const int percent : {100, 130}) {
        INFO("font " << percent << " %");
        AppTheme theme(QStringLiteral("dark"), percent);
        HooksRig rig(hookSettings({kHook}, {programRow()}));
        auto webhooks = rig.section<fv::WebhookSection>();
        auto programs = rig.section<fv::ProgramSection>();
        QScrollArea scroll;
        scroll.setWidgetResizable(true);
        auto* host = new QWidget;
        auto* column = new QVBoxLayout(host);
        column->addWidget(webhooks.get());
        column->addWidget(programs.get());
        column->addStretch();
        scroll.setWidget(host);
        scroll.show();
        webhooks->openEditor("hook01");
        programs->openEditor("prog01");
        for (const int width : {360, 480, 720}) {
            INFO("column " << width);
            scroll.resize(width, 900);
            pump(8);
            CHECK(scroll.horizontalScrollBar()->maximum() == 0);
            for (fv::HookSection* section : {static_cast<fv::HookSection*>(webhooks.get()),
                                             static_cast<fv::HookSection*>(programs.get())}) {
                fv::HookRow* row = section->row(section->entries().front().id);
                QPushButton* edit = row->editButton();
                QWidget* toggle = row->switchButton();
                CHECK_FALSE(
                    QRect(edit->mapTo(row, QPoint(0, 0)), edit->size())
                        .intersects(QRect(toggle->mapTo(row, QPoint(0, 0)), toggle->size())));
                for (QAbstractButton* button : section->findChildren<QAbstractButton*>()) {
                    if (!button->isVisible()) {
                        continue;
                    }
                    INFO(button->text().toStdString());
                    const QPoint position = button->mapTo(host, QPoint(0, 0));
                    CHECK(position.x() >= 0);
                    CHECK(position.x() + button->width() <= host->width() + 1);
                    CHECK(button->height() >= std::min(button->sizeHint().height(), 34) - 1);
                }
            }
            fv::AdaptiveButtons* footer = webhooks->webhookEditor()->footer();
            for (QPushButton* button :
                 {static_cast<QPushButton*>(webhooks->webhookEditor()->saveButton()),
                  static_cast<QPushButton*>(webhooks->webhookEditor()->cancelButton())}) {
                CHECK(button->width() >= button->sizeHint().width() - 1);
                CHECK(footer->rect().contains(
                    QRect(button->mapTo(footer, QPoint(0, 0)), button->size())));
            }
        }
    }
}

// ---- answers that arrive after the editor moved on ----

namespace {

const Json kAlpha = hookRow("alpha00", "Alpha", "a.example");
const Json kBeta = hookRow("beta000", "Beta", "b.example");

} // namespace

// from
// tests/test_automation_fix_ui_hooks_answers.py::test_a_late_test_result_and_secret_for_one_webhook_stay_off_the_next
TEST_CASE("test_a_late_test_result_and_secret_for_one_webhook_stay_off_the_next", kAnswers) {
    // What the workers reach is declared before the rig, whose io pool waits for them.
    ScriptedWebhooks engine;
    std::mutex pendingMutex;
    std::map<std::string, std::function<void(fv::automation::DeliveryRecord)>> pending;
    engine.onSendTest = [&](const std::string& id,
                            std::function<void(fv::automation::DeliveryRecord)> report) {
        std::lock_guard lock(pendingMutex);
        pending[id] = std::move(report);
    };
    std::latch release(1);
    std::atomic<bool> block = true;
    std::atomic<int> slowCalls = 0;
    const auto secretOf = [&](const std::string& id) {
        if (block) {
            release.wait();
        }
        ++slowCalls;
        return "whsec_secret_of_" + id;
    };
    engine.onEnsure = secretOf;
    engine.onRotate = secretOf;
    HooksRig rig(hookSettings({kAlpha, kBeta}));
    fv::HookServices services = rig.services();
    services.webhooks = &engine;
    auto section = rig.section<fv::WebhookSection>(services);
    section->openEditor("alpha00");
    fv::WebhookEditor& editor = *section->webhookEditor();
    rig.answers.push_back(QMessageBox::Yes);
    editor.testButton()->click();
    editor.copySecretButton()->click();
    editor.rotateSecretButton()->click();
    CHECK_FALSE(editor.testButton()->isEnabled());
    CHECK_FALSE(editor.copySecretButton()->isEnabled());
    CHECK_FALSE(editor.rotateSecretButton()->isEnabled());

    section->openEditor("beta000");
    CHECK(editor.itemId() == QStringLiteral("beta000"));
    {
        std::lock_guard lock(pendingMutex);
        pending["alpha00"](record(200, 120));
    }
    release.count_down();
    CHECK(waitFor([&] { return slowCalls.load() == 2; }));
    pump();
    // Alpha's secret is never copied while Beta is shown, and Beta says nothing of it.
    CHECK(rig.copied.isEmpty());
    CHECK(editor.testChip()->isHidden());
    CHECK(editor.secretChip()->isHidden());
    // Beta starts fresh: nothing of Alpha's holds its buttons.
    CHECK(editor.testButton()->isEnabled());
    CHECK(editor.copySecretButton()->isEnabled());
    CHECK(editor.rotateSecretButton()->isEnabled());

    // Beta's own requests still answer Beta.
    block = false;
    editor.copySecretButton()->click();
    CHECK(waitFor([&] { return editor.secretChip()->text() == QStringLiteral("Copied"); }));
    CHECK(rig.copied == QStringList{QStringLiteral("whsec_secret_of_beta000")});
    editor.testButton()->click();
    {
        std::lock_guard lock(pendingMutex);
        pending["beta000"](record(201, 80));
    }
    CHECK(waitFor([&] { return editor.testChip()->text() == QStringLiteral("201 · 80 ms"); }));
}

// from
// tests/test_automation_fix_ui_hooks_answers.py::test_a_late_run_result_for_one_program_stays_off_the_next
TEST_CASE("test_a_late_run_result_for_one_program_stays_off_the_next", kAnswers) {
    Json second = programRow("progb00", "Logger");
    HooksRig rig(hookSettings({}, {programRow("proga00", "Journal"), second}));
    ScriptedPrograms engine;
    std::map<std::string, std::function<void(fv::automation::RunRecord)>> pending;
    engine.onRunTest = [&](const std::string& id,
                           std::function<void(fv::automation::RunRecord)> report) {
        pending[id] = std::move(report);
    };
    fv::HookServices services = rig.services();
    services.programs = &engine;
    auto section = rig.section<fv::ProgramSection>(services);
    section->openEditor("proga00");
    fv::ProgramEditor& editor = *section->programEditor();
    editor.testButton()->click();
    CHECK(editor.testChip()->text() == QStringLiteral("Running…"));
    section->openEditor("progb00");
    pending["proga00"](runRecord("evt_1", "2026-10-10T18:03:00Z", false, 3, 90));
    pump();
    CHECK(editor.testChip()->isHidden());
    CHECK(editor.testButton()->isEnabled());
}

// from tests/test_automation_fix_ui_hooks_answers.py::test_closing_the_editor_forgets_its_requests
TEST_CASE("test_closing_the_editor_forgets_its_requests", kAnswers) {
    HooksRig rig(hookSettings({kAlpha}));
    ScriptedWebhooks engine;
    std::function<void(fv::automation::DeliveryRecord)> pending;
    engine.onSendTest = [&](const std::string&,
                            std::function<void(fv::automation::DeliveryRecord)> report) {
        pending = std::move(report);
    };
    fv::HookServices services = rig.services();
    services.webhooks = &engine;
    auto section = rig.section<fv::WebhookSection>(services);
    section->openEditor("alpha00");
    section->webhookEditor()->testButton()->click();
    section->cancel();
    section->openEditor("alpha00");
    CHECK(section->webhookEditor()->testButton()->isEnabled());
    pending(record(500, 30, false));
    pump();
    CHECK(section->webhookEditor()->testChip()->isHidden());
}

// ---- without an engine ----

TEST_CASE("without engines the test buttons are disabled and the recent lists say so", kPage) {
    HooksRig rig(hookSettings({kHook}, {programRow()}));
    fv::HookServices services = rig.services();
    services.webhooks = nullptr;
    services.programs = nullptr;
    auto webhooks = rig.section<fv::WebhookSection>(services);
    auto programs = rig.section<fv::ProgramSection>(services);
    webhooks->openEditor("hook01");
    programs->openEditor("prog01");
    CHECK_FALSE(webhooks->webhookEditor()->testButton()->isEnabled());
    CHECK_FALSE(webhooks->webhookEditor()->copySecretButton()->isEnabled());
    CHECK_FALSE(webhooks->webhookEditor()->rotateSecretButton()->isEnabled());
    CHECK_FALSE(programs->programEditor()->testButton()->isEnabled());
    CHECK(webhooks->webhookEditor()->recent()->shown()->empty());
    CHECK(programs->programEditor()->recent()->shown()->empty());
    CHECK(webhooks->row("hook01")->dot()->status() == fv::EngineStatus::Unknown);
    CHECK_FALSE(
        webhooks->row("hook01")->detail()->text().contains(QStringLiteral("Last delivery")));
}

TEST_CASE("an untouched editor follows a change made elsewhere and closes when its item is deleted",
          kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("hook01");
    // Changed elsewhere while open and untouched here.
    rig.store().mutate([](Json& settings) {
        settings["automation_webhooks"][0]["url"] = "https://example.org/elsewhere";
    });
    section->refresh();
    CHECK(section->webhookEditor()->urlEdit()->text() ==
          QStringLiteral("https://example.org/elsewhere"));
    CHECK_FALSE(section->hasUnsavedChanges());
    // A draft in progress is never replaced.
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Mine"));
    rig.store().mutate([](Json& settings) {
        settings["automation_webhooks"][0]["url"] = "https://example.org/again";
    });
    section->refresh();
    CHECK(section->webhookEditor()->nameEdit()->text() == QStringLiteral("Mine"));
    CHECK(section->webhookEditor()->urlEdit()->text() ==
          QStringLiteral("https://example.org/elsewhere"));
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Home"));
    CHECK(section->webhookEditor()->nameEdit()->text() == QStringLiteral("Home"));
    // Deleted elsewhere while open and untouched.
    section->cancel();
    section->openEditor("hook01");
    rig.store().mutate([](Json& settings) { settings["automation_webhooks"] = Json::array(); });
    section->refresh();
    CHECK(section->webhookEditor()->isHidden());
}

// ---- a result sits in a chip beside its button; an error is words, on a line of its own ----

namespace {

const char* const kResults = "[test_automation_fix_ui_hooks_results][widget]";

const std::string kLongError =
    "Couldn't connect: [WinError 10061] No connection could be made because the target machine "
    "actively refused it";

} // namespace

// from
// tests/test_automation_fix_ui_hooks_results.py::test_a_long_error_wraps_under_the_buttons_and_the_button_keeps_its_size
TEST_CASE("test_a_long_error_wraps_under_the_buttons_and_the_button_keeps_its_size", kResults) {
    HooksRig rig(hookSettings({kAlpha}, {programRow("proga00", "Journal")}));
    rig.webhooks.set_test_answer(record(std::nullopt, 5000, false, kLongError));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("alpha00");
    Column column(*section, 480);
    fv::WebhookEditor& editor = *section->webhookEditor();
    editor.testButton()->click();
    pump(8);
    fv::WrappedLabel* line = editor.testChip()->errorLine();
    CHECK(line->isVisible());
    CHECK(line->text() == qs(kLongError));
    CHECK(line->property("tone").toString() == QStringLiteral("error"));
    CHECK(line->wordWrap());
    CHECK(line->height() >= line->heightForWidth(line->width()) - 1);
    CHECK(editor.testChip()->isHidden());
    // The row stays side by side: Send test isn't stretched across the card.
    auto* row = qobject_cast<fv::AdaptiveButtons*>(editor.testButton()->parentWidget());
    REQUIRE(row != nullptr);
    CHECK(row->boxLayout()->direction() == QBoxLayout::LeftToRight);
    fv::Button* button = editor.testButton();
    CHECK(button->width() <= std::max(button->sizeHint().width(), button->minimumWidth()) + 1);

    // A short result goes back in the chip, and the error goes away.
    rig.webhooks.set_test_answer(record(404, 80, false));
    editor.testButton()->click();
    pump();
    CHECK(editor.testChip()->text() == QStringLiteral("404 · 80 ms"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("error"));
    CHECK_FALSE(editor.testChip()->isHidden());
    CHECK(line->isHidden());
}

// from
// tests/test_automation_fix_ui_hooks_results.py::test_engine_and_secret_errors_use_the_line_too
TEST_CASE("test_engine_and_secret_errors_use_the_line_too", kResults) {
    HooksRig rig(hookSettings({kAlpha}));
    ScriptedWebhooks engine;
    engine.onSendTest = [](const std::string&,
                           std::function<void(fv::automation::DeliveryRecord)>) {
        throw std::runtime_error("Saved webhooks changed; try again.");
    };
    engine.onEnsure = [](const std::string&) -> std::string {
        throw std::runtime_error(
            "Your system's credential store is locked. Unlock it and try again.");
    };
    fv::HookServices services = rig.services();
    services.webhooks = &engine;
    auto section = rig.section<fv::WebhookSection>(services);
    section->openEditor("alpha00");
    fv::WebhookEditor& editor = *section->webhookEditor();
    editor.testButton()->click();
    CHECK(editor.testChip()->isHidden());
    CHECK(editor.testChip()->errorLine()->text() ==
          QStringLiteral("Saved webhooks changed; try again."));
    editor.copySecretButton()->click();
    CHECK(waitFor([&] { return editor.copySecretButton()->isEnabled(); }));
    CHECK(editor.secretChip()->isHidden());
    CHECK(editor.secretChip()->errorLine()->text().startsWith(
        QStringLiteral("Your system's credential store")));
    CHECK_FALSE(editor.secretChip()->errorLine()->isHidden());
}

// from
// tests/test_automation_fix_ui_hooks_results.py::test_a_program_that_did_not_run_says_why_on_the_line
TEST_CASE("test_a_program_that_did_not_run_says_why_on_the_line", kResults) {
    HooksRig rig(hookSettings({}, {programRow("proga00", "Journal")}));
    rig.programs.set_test_answer(
        runRecord("evt_1", "2026-10-10T18:03:00Z", false, std::nullopt, 12,
                  "Couldn't start the program: the file was not found at its saved path"));
    auto section = rig.section<fv::ProgramSection>();
    section->openEditor("proga00");
    fv::ProgramEditor& editor = *section->programEditor();
    editor.testButton()->click();
    CHECK(editor.testChip()->isHidden());
    CHECK(editor.testChip()->errorLine()->text().startsWith(
        QStringLiteral("Couldn't start the program")));
    rig.programs.set_test_answer(runRecord("evt_2", "2026-10-10T18:04:00Z", false, 2, 80));
    editor.testButton()->click();
    CHECK(editor.testChip()->text() == QStringLiteral("Exit 2 · 80 ms"));
    CHECK(editor.testChip()->property("tone").toString() == QStringLiteral("error"));
    CHECK(editor.testChip()->errorLine()->isHidden());
}

// from
// tests/test_automation_fix_ui_hooks_results.py::test_moving_to_another_item_clears_the_error_line
TEST_CASE("test_moving_to_another_item_clears_the_error_line", kResults) {
    HooksRig rig(hookSettings({kAlpha}));
    rig.webhooks.set_test_answer(record(std::nullopt, 5000, false, kLongError));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("alpha00");
    section->webhookEditor()->testButton()->click();
    CHECK_FALSE(section->webhookEditor()->testChip()->errorLine()->isHidden());
    section->openEditor();
    CHECK(section->webhookEditor()->testChip()->errorLine()->isHidden());
}

// ---- Tab goes down the page the way it reads ----

namespace {

const char* const kTabOrder = "[test_automation_fix_ui_hooks_tab_order][widget]";

const Json kGamma = hookRow("gamma00", "Gamma", "c.example");

// The widgets Tab visits from `start` inside `within`, as Qt picks them.
std::vector<QWidget*> tabWalk(QWidget* start, QWidget* within, int limit = 400) {
    std::vector<QWidget*> visited{start};
    QWidget* widget = start;
    for (int i = 0; i < limit; ++i) {
        widget = widget->nextInFocusChain();
        if (widget == start) {
            break;
        }
        if (within->isAncestorOf(widget) &&
            (widget->focusPolicy() & Qt::TabFocus) == Qt::TabFocus &&
            widget->focusProxy() == nullptr && widget->isVisible() && widget->isEnabled()) {
            visited.push_back(widget);
        }
    }
    return visited;
}

std::string nameOf(QWidget* widget) {
    if (!widget->accessibleName().isEmpty()) {
        return text(widget->accessibleName());
    }
    if (const auto* button = qobject_cast<QAbstractButton*>(widget)) {
        return text(button->text());
    }
    return widget->metaObject()->className();
}

void assertReadingOrder(const std::vector<QWidget*>& widgets, QWidget* within) {
    for (std::size_t i = 0; i + 1 < widgets.size(); ++i) {
        const QPoint before = widgets[i]->mapTo(within, QPoint(0, 0));
        const QPoint after = widgets[i + 1]->mapTo(within, QPoint(0, 0));
        const bool sameLine = std::abs(after.y() - before.y()) <= 6;
        INFO("Tab goes from '" << nameOf(widgets[i]) << "' back up to '" << nameOf(widgets[i + 1])
                               << "'");
        CHECK((after.y() > before.y() || (sameLine && after.x() > before.x())));
    }
}

std::vector<QWidget*> rowStops(fv::HookSection& section, std::initializer_list<const char*> ids) {
    std::vector<QWidget*> stops;
    for (const char* id : ids) {
        stops.push_back(section.row(id)->editButton());
        stops.push_back(section.row(id)->switchButton());
    }
    return stops;
}

std::vector<QWidget*> concat(std::initializer_list<std::vector<QWidget*>> lists) {
    std::vector<QWidget*> all;
    for (const auto& list : lists) {
        all.insert(all.end(), list.begin(), list.end());
    }
    return all;
}

std::vector<QWidget*> slice(const std::vector<QWidget*>& walk, std::size_t from,
                            std::size_t count) {
    return std::vector<QWidget*>(walk.begin() + static_cast<std::ptrdiff_t>(from),
                                 walk.begin() + static_cast<std::ptrdiff_t>(from + count));
}

std::vector<QWidget*> tail(const std::vector<QWidget*>& walk, std::size_t count) {
    return std::vector<QWidget*>(walk.end() - static_cast<std::ptrdiff_t>(count), walk.end());
}

// A section between two buttons, as on the Hooks page.
struct TabPage {
    explicit TabPage(HooksRig& rig) : section(nullptr) {
        window = std::make_unique<QWidget>();
        auto* layout = new QVBoxLayout(window.get());
        first = fv::hooks_panel::smallButton(QStringLiteral("Before"));
        layout->addWidget(first);
        section = rig.section<fv::WebhookSection>();
        layout->addWidget(section.get());
        last = fv::hooks_panel::smallButton(QStringLiteral("After"));
        layout->addWidget(last);
        window->resize(720, 1600);
        window->show();
        pump(8);
    }
    std::unique_ptr<QWidget> window;
    fv::Button* first = nullptr;
    Held<fv::WebhookSection> section;
    fv::Button* last = nullptr;
};

} // namespace

// from
// tests/test_automation_fix_ui_hooks_tab_order.py::test_tab_runs_through_rows_and_the_editor_in_screen_order
TEST_CASE("test_tab_runs_through_rows_and_the_editor_in_screen_order", kTabOrder) {
    HooksRig rig(hookSettings({kAlpha, kBeta, kGamma}));
    TabPage page(rig);
    page.section->openEditor("beta000");
    pump(8);
    const std::vector<QWidget*> walk = tabWalk(page.first, page.window.get());
    fv::WebhookEditor* editor = page.section->webhookEditor();
    REQUIRE(walk.size() > 12);
    CHECK(slice(walk, 0, 6) == concat({{page.first},
                                       rowStops(*page.section, {"alpha00", "beta000"}),
                                       {editor->nameEdit()}}));
    CHECK(tail(walk, 6) == concat({{editor->cancelButton(), editor->saveButton()},
                                   rowStops(*page.section, {"gamma00"}),
                                   {page.section->addButton(), page.last}}));
    assertReadingOrder(walk, page.window.get());
}

// from tests/test_automation_fix_ui_hooks_tab_order.py::test_a_hook_added_later_takes_its_place
TEST_CASE("test_a_hook_added_later_takes_its_place", kTabOrder) {
    HooksRig rig(hookSettings({kAlpha, kBeta, kGamma}));
    TabPage page(rig);
    page.section->openEditor();
    fv::WebhookEditor* editor = page.section->webhookEditor();
    editor->nameEdit()->setText(QStringLiteral("Delta"));
    editor->urlEdit()->setText(QStringLiteral("https://d.example/hook"));
    // A new item's editor sits after the rows.
    std::vector<QWidget*> walk = tabWalk(page.first, page.window.get());
    REQUIRE(walk.size() > 8);
    CHECK(slice(walk, 0, 8) == concat({{page.first},
                                       rowStops(*page.section, {"alpha00", "beta000", "gamma00"}),
                                       {editor->nameEdit()}}));
    REQUIRE(page.section->save());
    pump(8);
    const std::string delta = text(editor->itemId());
    walk = tabWalk(page.first, page.window.get());
    REQUIRE(walk.size() > 10);
    std::vector<QWidget*> expected{page.first};
    for (const char* id : {"alpha00", "beta000", "gamma00"}) {
        expected.push_back(page.section->row(id)->editButton());
        expected.push_back(page.section->row(id)->switchButton());
    }
    expected.push_back(page.section->row(delta)->editButton());
    expected.push_back(page.section->row(delta)->switchButton());
    expected.push_back(editor->nameEdit());
    CHECK(slice(walk, 0, 10) == expected);
    CHECK(tail(walk, 3) ==
          std::vector<QWidget*>{editor->saveButton(), page.section->addButton(), page.last});
    assertReadingOrder(walk, page.window.get());
}

// from
// tests/test_automation_fix_ui_hooks_tab_order.py::test_closing_the_editor_leaves_the_rows_in_order
TEST_CASE("test_closing_the_editor_leaves_the_rows_in_order", kTabOrder) {
    HooksRig rig(hookSettings({kAlpha, kBeta, kGamma}));
    TabPage page(rig);
    page.section->openEditor("alpha00");
    pump(8);
    page.section->cancel();
    pump(8);
    CHECK(tabWalk(page.first, page.window.get()) ==
          concat({{page.first},
                  rowStops(*page.section, {"alpha00", "beta000", "gamma00"}),
                  {page.section->addButton(), page.last}}));
}

// from
// tests/test_automation_fix_ui_hooks_tab_order.py::test_the_hooks_page_reads_top_to_bottom_with_every_kind_of_editor_open
TEST_CASE("test_the_hooks_page_reads_top_to_bottom_with_every_kind_of_editor_open", kTabOrder) {
    Json settings = hookSettings(
        {kAlpha, kBeta}, {programRow("prog001", "Journal"), programRow("prog002", "Logger")},
        {Json{{"id", "filt001"},
              {"name", "Names"},
              {"kind", "program"},
              {"stage", "after_cleanup"},
              {"program",
               QDir::toNativeSeparators(QFileInfo("names.exe").absoluteFilePath()).toStdString()},
              {"timeout_ms", 1500},
              {"enabled", true}}});
    ActionsRig rig(settings);
    fv::SettingsDialog& dialog = rig.dialog(false);
    dialog.resize(1100, 900);
    dialog.selectDestination(qs(dest::hooks));
    dialog.show();
    pump(8);
    fv::HooksPage& page = hooksPage(rig);
    const std::vector<fv::HookSection*> sections{page.webhooks(), page.programs(), page.filters()};
    page.programs()->openEditor("prog001");
    page.filters()->openEditor("filt001");
    pump(8);
    QWidget* first = page.webhooks()->row("alpha00")->editButton();
    const std::vector<QWidget*> walk = tabWalk(first, &page);
    assertReadingOrder(walk, &page);
    const auto indexOf = [&](QWidget* widget) {
        const auto found = std::find(walk.begin(), walk.end(), widget);
        return found == walk.end() ? -1 : static_cast<int>(found - walk.begin());
    };
    for (fv::HookSection* section : sections) {
        int lastRow = -1;
        for (const fv::HookEntry& entry : section->entries()) {
            const int edit = indexOf(section->row(entry.id)->editButton());
            const int toggle = indexOf(section->row(entry.id)->switchButton());
            CHECK(edit >= 0);
            CHECK(toggle >= 0);
            lastRow = std::max({lastRow, edit, toggle});
        }
        CHECK(indexOf(section->addButton()) > lastRow);
    }
    // The program and filter editors' fields are all on the way.
    for (fv::HookEditor* editor : {page.programs()->editor(), page.filters()->editor()}) {
        CHECK(indexOf(editor->nameEdit()) >= 0);
        CHECK(indexOf(editor->saveButton()) >= 0);
    }
    CHECK(indexOf(page.programs()->programEditor()->argsEdit()) >= 0);
    CHECK(indexOf(page.filters()->filterEditor()->argsEdit()) >= 0);
    const auto inside = [&](QWidget* container) {
        return std::any_of(walk.begin(), walk.end(), [container](QWidget* widget) {
            return widget == container || container->isAncestorOf(widget);
        });
    };
    CHECK(inside(page.programs()->programEditor()->timeout()));
    CHECK(inside(page.filters()->filterEditor()->timeout()));
}

// from tests/test_automation_fix_ui_hooks_tab_order.py::test_a_hook_deleted_elsewhere_leaves_no_gap
TEST_CASE("test_a_hook_deleted_elsewhere_leaves_no_gap", kTabOrder) {
    HooksRig rig(hookSettings({kAlpha, kBeta, kGamma}));
    TabPage page(rig);
    rig.store().mutate([](Json& settings) { hooks::delete_webhook(settings, "beta000"); });
    page.section->refresh();
    pump(8);
    CHECK(tabWalk(page.first, page.window.get()) ==
          concat({{page.first},
                  rowStops(*page.section, {"alpha00", "gamma00"}),
                  {page.section->addButton(), page.last}}));
}

// ---- what the page does while it is on screen ----

TEST_CASE("the visible section polls the engines every second and stops when hidden", kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    CHECK_FALSE(section->isPolling());
    section->openEditor("hook01");
    section->show();
    pump();
    CHECK(section->isPolling());
    CHECK(section->row("hook01")->dot()->status() == fv::EngineStatus::Unknown);
    // A delivery that arrives while the section is shown reaches the row and the editor's list
    // without a click.
    rig.webhooks.set_deliveries("hook01", {record(200, 77)});
    CHECK(waitFor(
        [&] {
            return section->row("hook01")->dot()->status() == fv::EngineStatus::Ready &&
                   section->webhookEditor()->recent()->shown()->size() == 1;
        },
        3500));
    section->hide();
    CHECK_FALSE(section->isPolling());
}

TEST_CASE("toggling a row's switch never reads as an unsaved edit of the open editor", kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("hook01");
    section->row("hook01")->switchButton()->click();
    CHECK_FALSE(section->hasUnsavedChanges());
    section->row("hook01")->switchButton()->click();
    CHECK_FALSE(section->hasUnsavedChanges());
    CHECK(savedHook(rig, "hook01")->enabled);
    // Opening another item or leaving the page then asks nothing.
    CHECK(section->saveDraft(true));
    CHECK(rig.questions.empty());
}

TEST_CASE("a narrow column puts the row's buttons under its text", kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    Column column(*section, 720);
    CHECK_FALSE(section->row("hook01")->isNarrow());
    column.resize(260);
    CHECK(section->row("hook01")->isNarrow());
    column.resize(720);
    CHECK_FALSE(section->row("hook01")->isNarrow());
}

TEST_CASE("a draft that is open when the section is refreshed after a delete elsewhere stays",
          kPage) {
    HooksRig rig(hookSettings({kHook}));
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor("hook01");
    section->webhookEditor()->nameEdit()->setText(QStringLiteral("Mine"));
    rig.store().mutate([](Json& settings) { settings["automation_webhooks"] = Json::array(); });
    section->refresh();
    CHECK_FALSE(section->webhookEditor()->isHidden());
    CHECK(section->webhookEditor()->nameEdit()->text() == QStringLiteral("Mine"));
}
