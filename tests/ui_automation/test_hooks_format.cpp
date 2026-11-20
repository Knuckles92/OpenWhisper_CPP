// The words the Hooks page uses (ui_qt/widgets/hooks_panel.py event_label … unencrypted_note)
// against OpenWhisper's own results (tools/parity/cases/ui_qt.widgets.hooks_panel.py and
// ui_qt.dialogs.settings_hooks.py), and OpenWhisper's tests of the "Saved." line of the editors
// (tests/test_automation_fix_ui_hooks_saved.py), which run through real editors over the fakes
// of hooks_support.h.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <QDir>
#include <QFileInfo>
#include <QPushButton>

#include "automation/catalog.h"
#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "hooks_support.h"
#include "ui_automation/hooks_format.h"
#include "ui_automation/hooks_page.h"
#include "ui_automation/hooks_panel.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"

using fv::Json;
using namespace fvtest;
namespace hf = fv::hooks_format;

namespace {

fv::automation::DeliveryRecord deliveryFrom(const Json& spec) {
    fv::automation::DeliveryRecord item;
    item.event_id = spec.at("event_id").get<std::string>();
    item.event_type = spec.at("event_type").get<std::string>();
    item.at = spec.at("at").get<std::string>();
    item.ok = spec.at("ok").get<bool>();
    if (!spec.at("status").is_null()) {
        item.status = spec.at("status").get<int>();
    }
    item.elapsed_ms = spec.at("elapsed_ms").get<int>();
    item.error = spec.at("error").get<std::string>();
    item.attempt = spec.at("attempt").get<int>();
    return item;
}

fv::automation::RunRecord runFrom(const Json& spec) {
    fv::automation::RunRecord item;
    item.event_id = spec.at("event_id").get<std::string>();
    item.at = spec.at("at").get<std::string>();
    item.ok = spec.at("ok").get<bool>();
    if (!spec.at("exit_code").is_null()) {
        item.exit_code = spec.at("exit_code").get<int>();
    }
    item.elapsed_ms = spec.at("elapsed_ms").get<int>();
    item.error = spec.at("error").get<std::string>();
    return item;
}

Json pair(const hf::Outcome& outcome) {
    return Json::array({outcome.text, outcome.ok});
}

} // namespace

TEST_CASE("golden: hooks_panel.event_label", "[golden]") {
    fv::golden::for_each("ui_qt.widgets.hooks_panel", "event_label", [](const fv::golden::Case& c) {
        CHECK(hf::event_label(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden: hooks_panel.event_types", "[golden]") {
    fv::golden::for_each("ui_qt.widgets.hooks_panel", "event_types", [](const fv::golden::Case& c) {
        CHECK(hf::event_types() == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden: hooks_panel.events_summary", "[golden]") {
    fv::golden::for_each("ui_qt.widgets.hooks_panel", "events_summary",
                         [](const fv::golden::Case& c) {
                             CHECK(hf::events_summary(c.arg<std::vector<std::string>>(0)) ==
                                   c.result<std::string>());
                         });
}

TEST_CASE("golden: hooks_panel.clock", "[golden]") {
    // The vectors pin the zone to UTC.
    const fv::FakeClock utc;
    fv::golden::for_each("ui_qt.widgets.hooks_panel", "clock", [&](const fv::golden::Case& c) {
        INFO(c.arg<std::string>(0));
        CHECK(hf::clock(c.arg<std::string>(0), utc) == c.result<std::string>());
    });
}

TEST_CASE("golden: hooks_panel.delivery_result", "[golden]") {
    fv::golden::for_each(
        "ui_qt.widgets.hooks_panel", "delivery_result", [](const fv::golden::Case& c) {
            CHECK(fv::golden::equal(c.result<Json>(),
                                    pair(hf::delivery_result(deliveryFrom(c.arg<Json>(0))))));
        });
}

TEST_CASE("golden: hooks_panel.run_result", "[golden]") {
    fv::golden::for_each("ui_qt.widgets.hooks_panel", "run_result", [](const fv::golden::Case& c) {
        CHECK(fv::golden::equal(c.result<Json>(), pair(hf::run_result(runFrom(c.arg<Json>(0))))));
    });
}

TEST_CASE("golden: hooks_panel.unencrypted_note", "[golden]") {
    fv::golden::for_each(
        "ui_qt.widgets.hooks_panel", "unencrypted_note", [](const fv::golden::Case& c) {
            INFO(c.arg<std::string>(0));
            CHECK(hf::unencrypted_note(c.arg<std::string>(0)) == c.result<std::string>());
        });
}

TEST_CASE("golden: settings_hooks.rail_value", "[golden]") {
    fv::golden::for_each(
        "ui_qt.dialogs.settings_hooks", "rail_value", [](const fv::golden::Case& c) {
            CHECK(fv::hooks_page::rail_value(c.arg<Json>(0)) == c.result<std::string>());
        });
}

TEST_CASE("hooks_format: the host of an address keeps its port and falls back to the address",
          "[unit]") {
    CHECK(hf::host_of("https://example.com:8443/hooks") == "example.com:8443");
    CHECK(hf::host_of("http://user@example.com/in") == "user@example.com");
    CHECK(hf::host_of("not a url") == "not a url");
    CHECK(hf::host_of("http:///path") == "http:///path");
    CHECK(hf::host_of("http://[bad/x") == "http://[bad/x");
    // What urlsplit refuses reads as written: a bracketed host that is no IPv6 address or
    // IPvFuture literal, text beside the brackets, a non-ASCII netloc NFKC gives a separator.
    CHECK(hf::host_of("http://[::1]:8080/x") == "[::1]:8080");
    CHECK(hf::host_of("http://[v1.fe]/x") == "[v1.fe]");
    CHECK(hf::host_of("http://[vzz]/x") == "http://[vzz]/x");
    CHECK(hf::host_of("http://[127.0.0.1]/x") == "http://[127.0.0.1]/x");
    CHECK(hf::host_of("http://[:::::]/x") == "http://[:::::]/x");
    CHECK(hf::host_of("http://x[::1]/in") == "http://x[::1]/in");
    CHECK(hf::host_of("http://[::1]x/in") == "http://[::1]x/in");
    CHECK(hf::host_of("http://h℀s.example.com/in") == "http://h℀s.example.com/in");
    CHECK(hf::host_of("http://exämple.com/in") == "exämple.com");
}

TEST_CASE("hooks_format: stages and the engines' records read as the page words them", "[unit]") {
    CHECK(hf::stage_label("before_cleanup") == "Before AI cleanup");
    CHECK(hf::stage_label("after_cleanup") == "After AI cleanup");
    CHECK(hf::stage_label("sometime") == "sometime");
    CHECK(hf::delivery_result(record(200, 120)) == hf::Outcome{"200 · 120 ms", true});
    CHECK(hf::delivery_result(record(std::nullopt, 0, false, "")) ==
          hf::Outcome{"No answer", false});
    CHECK(hf::run_result(runRecord("e", "2026-10-10T18:03:00Z", true, 0, 340)) ==
          hf::Outcome{"Exit 0 · 340 ms", true});
}

// ---- "Saved." shows only while the fields match what was saved ----

namespace {

const char* const kSavedSuite = "[test_automation_fix_ui_hooks_saved][widget]";

}

// from
// tests/test_automation_fix_ui_hooks_saved.py::test_saved_goes_away_once_the_fields_change_again
TEST_CASE("test_saved_goes_away_once_the_fields_change_again", kSavedSuite) {
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor();
    fv::WebhookEditor& editor = *section->webhookEditor();
    editor.nameEdit()->setText(QStringLiteral("Home"));
    editor.urlEdit()->setText(QStringLiteral("https://example.com/in"));
    REQUIRE(section->save());
    CHECK(editor.message()->text() == QStringLiteral("Saved."));
    CHECK_FALSE(editor.message()->isHidden());
    editor.urlEdit()->setText(QStringLiteral("https://example.com/other"));
    CHECK(editor.hasUnsavedChanges());
    CHECK(editor.message()->isHidden());
}

// from tests/test_automation_fix_ui_hooks_saved.py::test_webhook_fields_clear_it
TEST_CASE("test_webhook_fields_clear_it", kSavedSuite) {
    for (const bool changeName : {true, false}) {
        INFO((changeName ? "name" : "events"));
        HooksRig rig;
        auto section = rig.section<fv::WebhookSection>();
        section->openEditor();
        fv::WebhookEditor& editor = *section->webhookEditor();
        editor.nameEdit()->setText(QStringLiteral("Home"));
        editor.urlEdit()->setText(QStringLiteral("https://example.com/in"));
        REQUIRE(section->save());
        if (changeName) {
            editor.nameEdit()->setText(QStringLiteral("Home 2"));
        } else {
            editor.events()->chip(std::string(cat::ACTION_RAN))->click();
        }
        CHECK(editor.message()->isHidden());
    }
}

// from tests/test_automation_fix_ui_hooks_saved.py::test_program_fields_clear_it
TEST_CASE("test_program_fields_clear_it", kSavedSuite) {
    HooksRig rig;
    auto section = rig.section<fv::ProgramSection>();
    section->openEditor();
    fv::ProgramEditor& editor = *section->programEditor();
    const QString first = QDir::toNativeSeparators(QFileInfo("log.exe").absoluteFilePath());
    const QString other = QDir::toNativeSeparators(QFileInfo("other.exe").absoluteFilePath());
    editor.nameEdit()->setText(QStringLiteral("Log"));
    editor.programEdit()->setText(first);
    const std::vector<std::function<void()>> changes{
        [&] { editor.argsEdit()->setPlainText(QStringLiteral("--quiet")); },
        [&] { editor.timeout()->setValue(45); },
        [&] { editor.programEdit()->setText(other); },
    };
    for (const auto& change : changes) {
        REQUIRE(section->save());
        CHECK(editor.message()->text() == QStringLiteral("Saved."));
        change();
        CHECK(editor.message()->isHidden());
    }
}

// from tests/test_automation_fix_ui_hooks_saved.py::test_filter_fields_clear_it
TEST_CASE("test_filter_fields_clear_it", kSavedSuite) {
    HooksRig rig;
    auto section = rig.section<fv::FilterSection>();
    section->openEditor();
    fv::FilterEditor& editor = *section->filterEditor();
    editor.nameEdit()->setText(QStringLiteral("Names"));
    editor.urlEdit()->setText(QStringLiteral("http://127.0.0.1:5005/fix"));
    editor.programEdit()->setText(
        QDir::toNativeSeparators(QFileInfo("fix.exe").absoluteFilePath()));
    const std::vector<std::function<void()>> changes{
        [&] { editor.stageBar()->setCurrentIndex(0); },
        [&] { editor.timeout()->setValue(900); },
        [&] { editor.kindBar()->setCurrentIndex(1); },
    };
    for (const auto& change : changes) {
        REQUIRE(section->save());
        CHECK(editor.message()->text() == QStringLiteral("Saved."));
        change();
        CHECK(editor.message()->isHidden());
    }
}

// from
// tests/test_automation_fix_ui_hooks_saved.py::test_an_edit_undone_does_not_bring_it_back_but_a_save_does
TEST_CASE("test_an_edit_undone_does_not_bring_it_back_but_a_save_does", kSavedSuite) {
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor();
    fv::WebhookEditor& editor = *section->webhookEditor();
    editor.nameEdit()->setText(QStringLiteral("Home"));
    editor.urlEdit()->setText(QStringLiteral("https://example.com/in"));
    REQUIRE(section->save());
    editor.urlEdit()->setText(QStringLiteral("https://example.com/in2"));
    editor.urlEdit()->setText(QStringLiteral("https://example.com/in"));
    CHECK(editor.message()->isHidden());
    REQUIRE(section->save());
    CHECK(editor.message()->text() == QStringLiteral("Saved."));
}

// from tests/test_automation_fix_ui_hooks_saved.py::test_an_error_stays_until_the_next_save
TEST_CASE("test_an_error_stays_until_the_next_save", kSavedSuite) {
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    section->openEditor();
    fv::WebhookEditor& editor = *section->webhookEditor();
    editor.nameEdit()->setText(QStringLiteral("Home"));
    CHECK_FALSE(section->save());
    const QString message = editor.message()->text();
    editor.events()->chip(std::string(cat::ACTION_RAN))->click();
    CHECK(editor.message()->text() == message);
    CHECK_FALSE(editor.message()->isHidden());
    CHECK(rig.savedWebhooks().empty());
}
