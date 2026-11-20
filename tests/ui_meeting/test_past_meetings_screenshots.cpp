// The Past Meetings panel and the four meeting dialogs against the references OpenWhisper
// rendered for them (tools/parity/builders/a31.py: past_meetings_panel/list,
// meeting_delete_dialog/{with-audio,no-audio}, meeting_recovery_dialog/two-rows,
// meeting_intro_dialog/first-visit, meeting_export_dialog/three-meetings), with the same data:
// naive timestamps, so every zone reads the same, and today pinned to 2026-03-15.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <tuple>
#include <vector>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QPushButton>
#include <QScrollBar>
#include <QSize>
#include <QTimer>

#include "base/identity.h"
#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "past_meetings_support.h"
#include "screenshot/screenshot.h"
#include "tab_support.h"
#include "ui_meeting/meeting_delete_dialog.h"
#include "ui_meeting/meeting_export_dialog.h"
#include "ui_meeting/meeting_intro_dialog.h"
#include "ui_meeting/meeting_recovery_dialog.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"

using fv::Json;

namespace {

std::string variantOf(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start; Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes (amendment 17).
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    for (QAbstractScrollArea* area : root.findChildren<QAbstractScrollArea*>()) {
        for (QScrollBar* bar : {area->horizontalScrollBar(), area->verticalScrollBar()}) {
            QWidget* container = bar->parentWidget();
            if (container == nullptr || container->size() != kUnsized || bar->size() != kUnsized) {
                continue;
            }
            const QSize hint = bar->sizeHint();
            const QSize sized = bar->orientation() == Qt::Horizontal
                                    ? QSize(kUnsized.width(), hint.height())
                                    : QSize(hint.width(), kUnsized.height());
            container->resize(sized);
            bar->resize(sized);
        }
    }
}

// The tree keys check() compares itself.
const std::vector<std::string> kTreeKeys{
    "class", "qtClass",   "objectName", "geometry",  "visible",  "hasFocus",   "properties",
    "text",  "isChecked", "isDefault",  "alignment", "wordWrap", "styleSheet", "font"};

// A widget that is not shown keeps the geometry of whichever layout pass last reached it, which
// no one sees and Qt makes no promise about: it is taken from the reference for the widgets the
// reference has hidden. A plain local sheet (no token, no font size) goes through
// setLocalStyleSheet here, which records its source; OpenWhisper's filter leaves such a sheet
// alone.
void normalizeHidden(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
        if (expected.contains("wordWrap")) {
            node["wordWrap"] = expected["wordWrap"];
        }
    }
    auto& properties = node["properties"];
    if (properties.contains("unscaledStyleSheet")) {
        const std::string source = properties["unscaledStyleSheet"].get<std::string>();
        if (source.find('@') == std::string::npos &&
            source.find("font-size") == std::string::npos) {
            properties.erase("unscaledStyleSheet");
            properties.erase("scaledStyleSheet");
        }
    }
    const auto& want = expected["children"];
    auto& have = node["children"];
    for (std::size_t i = 0; i < std::min(want.size(), have.size()); ++i) {
        normalizeHidden(have[i], want[i]);
    }
}

// Swallows the timer events of every QTimer below a widget while it lives, as expect_matches
// holds them between show() and the grab.
class HeldTimers : public QObject {
public:
    explicit HeldTimers(QWidget& root) {
        for (QTimer* timer : root.findChildren<QTimer*>()) {
            timer->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override { return event->type() == QEvent::Timer; }
};

void compareTree(QWidget& widget, const Json& pins, const std::string& name,
                 const std::string& variant) {
    Json actual = fv::screenshot::dump_tree(widget);
    normalizeHidden(actual, pins["tree"]);
    for (const auto& difference : fv::screenshot::compare_trees(pins["tree"], actual)) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
}

// The pixels through expect_matches, and the tree with hidden widgets normalized, both of the
// widget as one show() and one turn of the event loop leave it, as the reference was grabbed.
void check(QWidget& widget, const std::string& name, const std::string& variant) {
    const Json pins = fv::screenshot::load_reference(name, variant);
    sizeHiddenScrollBarsAsQt611(widget);
    if (!fv::screenshot::pixel_comparison_enabled() ||
        !fv::screenshot::qt_kit_matches_reference()) {
        widget.setFixedSize(pins["logical_size"][0].get<int>(), pins["logical_size"][1].get<int>());
        const HeldTimers held(widget);
        widget.show();
        QApplication::processEvents();
        compareTree(widget, pins, name, variant);
    }
    fv::screenshot::Options options;
    options.ignore_keys = kTreeKeys;
    fv::screenshot::expect_matches(widget, name, variant, options);
    compareTree(widget, pins, name, variant);
}

// ---- the panel ----

Json row(const char* id, const char* title, const char* status, const char* started,
         const char* ended, int paused, const Json& summary = nullptr,
         const std::string& state = std::string()) {
    Json made = {{"id", id},          {"title", title},
                 {"status", status},  {"started_at", started},
                 {"ended_at", ended}, {"paused_total_s", paused}};
    if (!summary.is_null()) {
        made["content_summary"] = summary;
    }
    if (!state.empty()) {
        made["state_json"] = state;
    }
    return made;
}

// PAST_ROWS of the builder.
std::vector<Json> pastRows() {
    return {
        row("m_ready", "Planning review", "ended", "2026-03-14T09:30:00", "2026-03-14T10:12:00",
            120,
            Json{{"has_audio", true},
                 {"has_transcript", true},
                 {"preview_text", "We should ship the sidebar polish next."}},
            R"({"finalization": {"status": "completed"}})"),
        row("m_saved", "Deferred review", "ended", "2026-03-13T15:00:00", "2026-03-13T15:25:00", 0,
            Json{{"has_audio", true},
                 {"has_transcript", true},
                 {"preview_text", "Notes were not finished before the meeting ended."}},
            R"({"finalization": {"status": "failed", "card_deferred": true}})"),
        row("m_failed", "", "failed", "2026-03-12T14:00:00", "2026-03-12T14:00:00", 0,
            Json{{"is_empty", true}, {"has_audio", false}, {"has_transcript", false}}),
        row("m_empty", "Morning stand-up", "ended", "2026-03-11T08:30:00", "2026-03-11T08:30:20", 0,
            Json{{"is_empty", true},
                 {"has_audio", false},
                 {"has_transcript", false},
                 {"preview_text", ""}}),
        row("m_topic", "", "ended", "2026-03-10T11:00:00", "2026-03-10T12:01:00", 0,
            Json{{"has_audio", true},
                 {"has_transcript", true},
                 {"preview_text", "Let's start with last quarter's numbers."}},
            R"({"title": "", "topic": {"current": "Quarterly roadmap"}})"),
        row("m_live", "Running now", "active", "2026-03-15T09:00:00", "2026-03-15T09:42:00", 120,
            Json{{"has_audio", true}, {"has_transcript", true}, {"preview_text", "Hello."}}),
    };
}

void panel(const std::string& theme, int scale) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fvtest::PanelFixture fixture(pastRows());
    // The calendar button shows today's day: the fixture's clock reads 2026-03-15.
    fixture.refresh();
    fixture.panel->setSelectedMeetingId(QStringLiteral("m_saved"));
    check(*fixture.panel, "past_meetings_panel/list", variantOf(theme, scale));
}

// ---- the dialogs ----

void deleteDialog(const char* name, bool hasAudio, const std::string& theme) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme));
    fv::MeetingDeleteDialog dialog(nullptr, hasAudio);
    check(dialog, name, variantOf(theme, 100));
}

void recoveryDialog(const std::string& theme) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme));
    const auto meeting = [](const char* id, const char* title, const char* startedAt) {
        return QJsonObject{{"id", QString::fromLatin1(id)},
                           {"title", QString::fromLatin1(title)},
                           {"started_at", QString::fromLatin1(startedAt)}};
    };
    fv::MeetingRecoveryDialog dialog(
        {meeting("m_crash_1", "Weekly design review", "2026-03-14T15:30:12"),
         meeting("m_crash_2", "", "2026-03-13T09:00:00")});
    dialog.findChildren<fv::PrimaryButton*>().first()->click();
    check(dialog, "meeting_recovery_dialog/two-rows", variantOf(theme, 100));
}

void introDialog(const std::string& theme) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme));
    fv::MeetingModeIntroDialog dialog;
    check(dialog, "meeting_intro_dialog/first-visit", variantOf(theme, 100));
}

// EXPORT_MEETINGS of the builder.
std::vector<Json> exportMeetings() {
    std::vector<Json> made;
    for (const auto& [id, title, started] :
         {std::tuple{"m_export_1", "Weekly design review", "2026-03-14T15:30:00"},
          std::tuple{"m_export_2", "Customer call with Acme", "2026-03-12T09:00:00"},
          std::tuple{"m_export_3", "Quarterly roadmap sync", "2026-03-10T11:00:00"}}) {
        made.push_back(Json{{"id", id},
                            {"title", title},
                            {"status", "ended"},
                            {"started_at", started},
                            {"content_summary", {{"has_transcript", true}}}});
    }
    return made;
}

void exportDialog(const std::string& theme) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme));
    fvtest::Io io;
    fv::MeetingExportDialog dialog([] { return exportMeetings(); },
                                   R"(C:\Users\Dana\AppData\Local\FioriVoce\meetings)", nullptr,
                                   io.executor());
    dialog.fromDate()->setDate(QDate(2026, 2, 15));
    dialog.toDate()->setDate(QDate(2026, 3, 15));
    check(dialog, "meeting_export_dialog/three-meetings", variantOf(theme, 100));
}

} // namespace

TEST_CASE("reference screenshot past_meetings_panel list dark", "[screenshot]") {
    panel("dark", 100);
}

TEST_CASE("reference screenshot past_meetings_panel list light", "[screenshot]") {
    panel("light", 100);
}

TEST_CASE("reference screenshot past_meetings_panel list dark 130", "[screenshot]") {
    panel("dark", 130);
}

TEST_CASE("reference screenshot meeting_delete_dialog with-audio dark", "[screenshot]") {
    deleteDialog("meeting_delete_dialog/with-audio", true, "dark");
}

TEST_CASE("reference screenshot meeting_delete_dialog with-audio light", "[screenshot]") {
    deleteDialog("meeting_delete_dialog/with-audio", true, "light");
}

TEST_CASE("reference screenshot meeting_delete_dialog no-audio dark", "[screenshot]") {
    deleteDialog("meeting_delete_dialog/no-audio", false, "dark");
}

TEST_CASE("reference screenshot meeting_delete_dialog no-audio light", "[screenshot]") {
    deleteDialog("meeting_delete_dialog/no-audio", false, "light");
}

TEST_CASE("reference screenshot meeting_recovery_dialog two-rows dark", "[screenshot]") {
    recoveryDialog("dark");
}

TEST_CASE("reference screenshot meeting_recovery_dialog two-rows light", "[screenshot]") {
    recoveryDialog("light");
}

TEST_CASE("reference screenshot meeting_intro_dialog first-visit dark", "[screenshot]") {
    introDialog("dark");
}

TEST_CASE("reference screenshot meeting_intro_dialog first-visit light", "[screenshot]") {
    introDialog("light");
}

TEST_CASE("reference screenshot meeting_export_dialog three-meetings dark", "[screenshot]") {
    exportDialog("dark");
}

TEST_CASE("reference screenshot meeting_export_dialog three-meetings light", "[screenshot]") {
    exportDialog("light");
}
