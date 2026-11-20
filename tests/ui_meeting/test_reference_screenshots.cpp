// The Meeting Mode tab against OpenWhisper's pixels (tools/parity/builders/a31.py,
// tests/reference/meeting_mode_tab/<state>/). The payload scripts are the builder's
// MEETING_SCRIPTS, on a FakeClock whose steady time stands for the pinned time.monotonic().
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QScrollBar>
#include <QSize>
#include <QString>

#include "base/json.h"
#include "screenshot/screenshot.h"
#include "tab_support.h"

namespace fvtest {
namespace {

using fv::Json;

// The builder's T0, in seconds.
constexpr double kT0 = 5000.0;

Json identity() {
    return {{"meeting_id", "m_sync"},
            {"display_title", "Quarterly roadmap sync"},
            {"started_at", "2026-11-14T09:00:00"},
            {"ended_at", "2026-11-14T09:42:00"},
            {"paused_total_s", 120}};
}

Json fourSteps(const std::vector<const char*>& statuses, const Json& details = Json::object()) {
    static const std::vector<std::pair<const char*, const char*>> kSteps{
        {"redecode", "Audio Re-transcription"},
        {"polish", "Transcript Cleanup"},
        {"consolidation", "Summary & Action Items"},
        {"finalize", "State Finalization"}};
    Json steps = Json::array();
    for (std::size_t index = 0; index < statuses.size(); ++index) {
        Json step = {{"id", kSteps[index].first},
                     {"name", kSteps[index].second},
                     {"status", statuses[index]}};
        if (details.contains(kSteps[index].first)) {
            step["detail"] = details.at(kSteps[index].first);
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

Json result(Json finalization) {
    Json payload = {{"active", false}, {"status", "ended"}, {"dashboard_available", true}};
    payload.update(identity());
    payload["finalization"] = std::move(finalization);
    return payload;
}

// MEETING_SCRIPTS of the builder.
std::vector<Json> script(std::string_view state) {
    const Json live = {{"active", true},
                       {"paused", false},
                       {"status", "active"},
                       {"elapsed_s", 42.0},
                       {"elapsed_at", kT0}};
    if (state == "live") {
        return {live};
    }
    if (state == "paused") {
        return {live,
                {{"active", true},
                 {"paused", true},
                 {"status", "paused"},
                 {"elapsed_s", 42.0},
                 {"elapsed_at", kT0}}};
    }
    if (state == "starting") {
        return {{{"active", false}, {"status", "starting"}}};
    }
    if (state == "result-running") {
        return {result({{"status", "running"},
                        {"message", "Cleaning transcript…"},
                        {"stage", "polish"},
                        {"current_step", 2},
                        {"total_steps", 4},
                        {"step_details", "Polishing block 1 of 2 (40 segments)..."},
                        {"steps", fourSteps({"completed", "running", "pending", "pending"})}})};
    }
    if (state == "result-completed") {
        return {result(
            {{"status", "completed"},
             {"message", "Final insights ready — 32 segments, 4 key points."},
             {"current_step", 4},
             {"total_steps", 4},
             {"steps", fourSteps({"completed", "completed", "completed", "completed"})},
             {"content_summary", {{"meeting_status", "ended"}, {"can_rerun_speakers", true}}}})};
    }
    if (state == "result-failed") {
        return {result(
            {{"status", "failed"},
             {"message", "Final insights failed."},
             {"current_step", 4},
             {"total_steps", 4},
             {"step_details", "Re-decoding failed; kept live transcript"},
             {"steps", fourSteps({"failed", "completed", "completed", "completed"},
                                 {{"redecode", "Re-decoding failed; kept live transcript"}})},
             {"content_summary", {{"meeting_status", "ended"}, {"can_rerun_speakers", false}}}})};
    }
    if (state == "empty-meeting") {
        return {{{"active", false},
                 {"status", "ended"},
                 {"meeting_id", "m_empty"},
                 {"display_title", "Morning stand-up"},
                 {"started_at", "2026-11-14T08:30:00"},
                 {"ended_at", "2026-11-14T08:30:12"},
                 {"finalization",
                  {{"status", "completed"},
                   {"message", "Final insights are ready."},
                   {"content_summary",
                    {{"meeting_status", "ended"},
                     {"is_empty", true},
                     {"has_audio", false},
                     {"has_transcript", false},
                     {"can_rerun_speakers", false}}}}}}};
    }
    return {};
}

// RECENT_ROWS of the builder.
std::vector<Json> recentRows() {
    return {{{"id", "m_review"},
             {"title", "Weekly design review"},
             {"status", "ended"},
             {"started_at", "2026-11-13T15:30:00"},
             {"ended_at", "2026-11-13T16:12:00"},
             {"paused_total_s", 0},
             {"cloud_enabled", true},
             {"state_json", R"({"cloud_enabled": true, "finalization": {"status": "completed"}})"}},
            {{"id", "m_acme"},
             {"title", "Customer call with Acme"},
             {"status", "ended"},
             {"started_at", "2026-11-12T09:00:00"},
             {"ended_at", "2026-11-12T09:25:40"},
             {"paused_total_s", 0}},
            {{"id", "m_failed"},
             {"title", ""},
             {"status", "failed"},
             {"started_at", "2026-11-10T14:00:00"},
             {"ended_at", "2026-11-10T14:00:20"}}};
}

// The tree keys check() compares itself.
const std::vector<std::string> kTreeKeys{
    "class", "qtClass",   "objectName", "geometry",  "visible",  "hasFocus",   "properties",
    "text",  "isChecked", "isDefault",  "alignment", "wordWrap", "styleSheet", "font"};

// Qt 6.11 sizes a scroll bar that was never laid out (and its container) to its size hint
// across, where 6.10 leaves both at 100 x 30 (lead amendment 17): the brief field's hidden
// horizontal bar is one.
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

// A widget that is not shown keeps the geometry of whichever layout pass last reached it (a
// fresh widget's 640 x 480 until one does), which no one sees and Qt makes no promise about:
// it is taken from the reference for the widgets the reference has hidden.
void normalizeHidden(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
    }
    // A plain local sheet (no token, no font size) goes through setLocalStyleSheet here, which
    // records its source; OpenWhisper's filter leaves such a sheet alone.
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

void compareTree(QWidget& widget, const Json& pins, const std::string& name,
                 const std::string& variant) {
    Json actual = fv::screenshot::dump_tree(widget);
    normalizeHidden(actual, pins["tree"]);
    for (const auto& difference : fv::screenshot::compare_trees(pins["tree"], actual, {"class"})) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
}

void expectTab(std::string_view state, const char* theme, int scale) {
    AppTheme applied(QString::fromLatin1(theme), scale);
    fv::MeetingModeTabOptions initial;
    initial.aiDestination =
        QStringLiteral("OpenRouter · deepseek-v4.1-flash — sends transcript text, never audio.");
    initial.aiCompactDestination = QStringLiteral("OpenRouter · deepseek-v4.1-flash");
    TabFixture tab(kT0, initial);
    tab->setAiInsightsAvailable(true);
    if (state == "idle") {
        tab->showRecentMeetings(recentRows());
    }
    for (const Json& payload : script(state)) {
        tab->setMeetingState(payload);
    }
    const std::string name = "meeting_mode_tab/" + std::string(state);
    const std::string variant = std::string(theme) + "-windows11-dpr1-s" + std::to_string(scale);
    sizeHiddenScrollBarsAsQt611(*tab);
    const Json pins = fv::screenshot::load_reference(name, variant);
    if (!fv::screenshot::pixel_comparison_enabled() ||
        !fv::screenshot::qt_kit_matches_reference()) {
        // expect_matches skips the rest of the case without pixels: the tree is compared here.
        tab->setFixedSize(pins["logical_size"][0].get<int>(), pins["logical_size"][1].get<int>());
        tab->show();
        QApplication::processEvents();
        compareTree(*tab, pins, name, variant);
    }
    fv::screenshot::Options options;
    options.ignore_keys = kTreeKeys;
    fv::screenshot::expect_matches(*tab, name, variant, options);
    // After the pixels, as the reference was dumped: shown, one turn of the event loop.
    compareTree(*tab, pins, name, variant);
    tab->hide();
}

} // namespace

#define FV_MEETING_TAB_REFERENCE(state)                                                            \
    TEST_CASE("the " state " tab matches the reference, dark", "[screenshot][meeting_mode_tab]") { \
        expectTab(state, "dark", 100);                                                             \
    }                                                                                              \
    TEST_CASE("the " state " tab matches the reference, light",                                    \
              "[screenshot][meeting_mode_tab]") {                                                  \
        expectTab(state, "light", 100);                                                            \
    }                                                                                              \
    TEST_CASE("the " state " tab matches the reference, dark at 130 %",                            \
              "[screenshot][meeting_mode_tab]") {                                                  \
        expectTab(state, "dark", 130);                                                             \
    }

FV_MEETING_TAB_REFERENCE("idle")
FV_MEETING_TAB_REFERENCE("live")
FV_MEETING_TAB_REFERENCE("paused")
FV_MEETING_TAB_REFERENCE("starting")
FV_MEETING_TAB_REFERENCE("result-running")
FV_MEETING_TAB_REFERENCE("result-completed")
FV_MEETING_TAB_REFERENCE("result-failed")
FV_MEETING_TAB_REFERENCE("empty-meeting")

#undef FV_MEETING_TAB_REFERENCE

} // namespace fvtest
