// The History widgets against the references OpenWhisper rendered for them
// (tools/parity/builders/a09.py, tests/reference/<widget>/<theme>-windows11-dpr1-s<scale>): the
// widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
//
// The trees are compared without class names: OpenWhisper's chip row is the private _ChipFlow,
// a name C++ reserves. checkClasses compares the classes on their own with that one mapped.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QList>
#include <QMessageBox>
#include <QScrollBar>
#include <QSize>

#include "base/testing/fake_clock.h"
#include "history_ui_support.h"
#include "screenshot/screenshot.h"
#include "ui_history/history_edge_tab.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_item.h"
#include "ui_history/history_sidebar.h"

using fv::HistoryEntry;

namespace {

constexpr char kModel[] = "parakeet (Parakeet TDT 0.6B v3 | cuda)";
constexpr char kDictationAudio[] = "recording_20260315_150000.wav";
constexpr char kFailedAudio[] = "recording_20260315_143000.wav";

// The builder's three entries, newest first.
std::vector<HistoryEntry> entries() {
    HistoryEntry dictation;
    dictation.id = "entry-dictation";
    dictation.text = "Hello from OpenWhisper_CPP.";
    dictation.raw_text = "hello from openwhisper-cpp";
    dictation.timestamp = "2026-03-15T15:00:00";
    dictation.model = kModel;
    dictation.audio_file = kDictationAudio;
    dictation.transcription_time = 0.42;
    dictation.audio_duration = 4.6;
    dictation.file_size = 403200;
    dictation.cleanup_provider = "openai";
    dictation.cleanup_model = "gpt-4o-mini";
    dictation.source_name = "Quick Record";
    dictation.app_name = "Notepad";
    dictation.cleanup_level = "medium";
    dictation.entry_kind = "dictation";

    HistoryEntry failed;
    failed.id = "entry-failed";
    failed.timestamp = "2026-03-15T14:30:00";
    failed.model = kModel;
    failed.audio_file = kFailedAudio;
    failed.audio_duration = 3.0;
    failed.file_size = 264600;
    failed.source_name = "Quick Record";
    failed.entry_kind = std::string(fv::FAILED_RECORDING_KIND);

    HistoryEntry upload;
    upload.id = "entry-file";
    upload.text = "We reviewed the interview notes and agreed on the next steps for the launch.";
    upload.timestamp = "2026-03-15T14:00:00";
    upload.model = kModel;
    upload.transcription_time = 12.3;
    upload.audio_duration = 754.0;
    upload.source_name = "interview.mp3";
    upload.entry_kind = "file";
    return {dictation, failed, upload};
}

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start: 12 px, or the 8 px the sidebar's sheet
// sets. Qt 6.10 leaves them at QWidget's default 100 x 30. They are never drawn, so only the
// trees differ, and here they are given 6.11's sizes (as tests/ui_shell corrects the hidden
// menu-bar extension button, architecture amendment 17). A bar already sized is left alone.
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    QList<QAbstractScrollArea*> areas = root.findChildren<QAbstractScrollArea*>();
    if (auto* self = qobject_cast<QAbstractScrollArea*>(&root)) {
        areas.prepend(self);
    }
    for (QAbstractScrollArea* area : areas) {
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

// The class of every widget in the tree against the reference's, where OpenWhisper's private
// _ChipFlow is ChipFlow here (a leading underscore and a capital is a name C++ reserves).
void checkClasses(const fv::Json& expected, const fv::Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_ChipFlow") {
        wanted = "ChipFlow";
    }
    INFO(path);
    CHECK(actual.value("class", std::string()) == wanted);
    const fv::Json empty = fv::Json::array();
    const fv::Json& want = expected.contains("children") ? expected["children"] : empty;
    const fv::Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name));
    }
}

// What every variant needs: the recordings on disk (as the store says), a player on a fake
// device, today pinned to the builder's 2026-03-15, and the app styled for the variant.
struct Scene {
    Scene(const std::string& theme, int scale)
        : appTheme(QString::fromStdString(theme), scale),
          clock(std::chrono::sys_days{std::chrono::year{2026} / 3 / 15} + std::chrono::hours{15}),
          variant(theme + "-windows11-dpr1-s" + std::to_string(scale)) {
        store->recordings[kDictationAudio] = std::filesystem::path("recordings") / kDictationAudio;
        store->recordings[kFailedAudio] = std::filesystem::path("recordings") / kFailedAudio;
    }

    void check(QWidget& widget, const std::string& name) {
        sizeHiddenScrollBarsAsQt611(widget);
        // The classes the comparison below leaves out, OpenWhisper's _ChipFlow read as ChipFlow;
        // checked first, since expect_matches ends the test case when it skips the pixels.
        widget.ensurePolished();
        checkClasses(fv::screenshot::load_reference(name, variant).at("tree"),
                     fv::screenshot::dump_tree(widget), name);
        fv::screenshot::Options options;
        options.ignore_keys = {"class"};
        fv::screenshot::expect_matches(widget, name, variant, options);
    }

    fvtest::AppTheme appTheme;
    fvtest::FakeDevice device;
    fv::FakeClock clock;
    std::shared_ptr<fvtest::FakeStore> store = std::make_shared<fvtest::FakeStore>();
    fvtest::Io io;
    std::string variant;
};

void historyItem(const std::string& theme, int scale, int index, const char* name) {
    Scene scene(theme, scale);
    fv::HistoryItemWidget card(entries()[static_cast<std::size_t>(index)], *scene.store);
    scene.check(card, name);
}

void historySidebar(const std::string& theme, int scale) {
    Scene scene(theme, scale);
    fv::HistorySidebar sidebar(scene.store, scene.io.executor(), nullptr, scene.clock);
    // Opened; its load is answered with the builder's entries.
    sidebar.expand();
    sidebar.applyHistoryResults(sidebar.loadGeneration(), QString(), entries(), QString());
    scene.check(sidebar, "history_sidebar/expanded");
}

void edgeTab(const std::string& theme, int scale, bool expanded) {
    Scene scene(theme, scale);
    fv::HistoryEdgeTab tab;
    tab.setShortcutHint(QStringLiteral("Ctrl+H"));
    tab.setExpanded(expanded);
    scene.check(tab, expanded ? "history_edge_tab/expanded" : "history_edge_tab/collapsed");
}

void entryDialog(const std::string& theme, int scale) {
    Scene scene(theme, scale);
    fv::HistoryEntryDialog dialog(entries()[0], scene.store, scene.io.executor());
    scene.check(dialog, "history_entry_dialog/dictation");
}

void deleteConfirm(const std::string& theme) {
    Scene scene(theme, 100);
    fv::HistorySidebar sidebar(scene.store, scene.io.executor(), nullptr, scene.clock);
    fv::HistorySidebar::DeleteConfirmation confirmation =
        sidebar.makeDeleteConfirmation(nullptr, QString(), true);
    // As the builder does: the box sizes itself as it shows, and that size is the one recorded.
    confirmation.box->show();
    QCoreApplication::processEvents();
    confirmation.box->hide();
    scene.check(*confirmation.box, "history_delete_confirm/with-audio");
}

} // namespace

TEST_CASE("reference screenshot history_item dictation dark", "[screenshot]") {
    historyItem("dark", 100, 0, "history_item/dictation");
}

TEST_CASE("reference screenshot history_item dictation light", "[screenshot]") {
    historyItem("light", 100, 0, "history_item/dictation");
}

TEST_CASE("reference screenshot history_item dictation dark 130", "[screenshot]") {
    historyItem("dark", 130, 0, "history_item/dictation");
}

TEST_CASE("reference screenshot history_item failed dark", "[screenshot]") {
    historyItem("dark", 100, 1, "history_item/failed");
}

TEST_CASE("reference screenshot history_item failed light", "[screenshot]") {
    historyItem("light", 100, 1, "history_item/failed");
}

TEST_CASE("reference screenshot history_item failed dark 130", "[screenshot]") {
    historyItem("dark", 130, 1, "history_item/failed");
}

TEST_CASE("reference screenshot history_sidebar expanded dark", "[screenshot]") {
    historySidebar("dark", 100);
}

TEST_CASE("reference screenshot history_sidebar expanded light", "[screenshot]") {
    historySidebar("light", 100);
}

TEST_CASE("reference screenshot history_sidebar expanded dark 130", "[screenshot]") {
    historySidebar("dark", 130);
}

TEST_CASE("reference screenshot history_edge_tab collapsed dark", "[screenshot]") {
    edgeTab("dark", 100, false);
}

TEST_CASE("reference screenshot history_edge_tab collapsed light", "[screenshot]") {
    edgeTab("light", 100, false);
}

TEST_CASE("reference screenshot history_edge_tab collapsed dark 130", "[screenshot]") {
    edgeTab("dark", 130, false);
}

TEST_CASE("reference screenshot history_edge_tab expanded dark", "[screenshot]") {
    edgeTab("dark", 100, true);
}

TEST_CASE("reference screenshot history_edge_tab expanded light", "[screenshot]") {
    edgeTab("light", 100, true);
}

TEST_CASE("reference screenshot history_edge_tab expanded dark 130", "[screenshot]") {
    edgeTab("dark", 130, true);
}

TEST_CASE("reference screenshot history_entry_dialog dictation dark", "[screenshot]") {
    entryDialog("dark", 100);
}

TEST_CASE("reference screenshot history_entry_dialog dictation light", "[screenshot]") {
    entryDialog("light", 100);
}

TEST_CASE("reference screenshot history_entry_dialog dictation dark 130", "[screenshot]") {
    entryDialog("dark", 130);
}

TEST_CASE("reference screenshot history_delete_confirm with-audio dark", "[screenshot]") {
    deleteConfirm("dark");
}

TEST_CASE("reference screenshot history_delete_confirm with-audio light", "[screenshot]") {
    deleteConfirm("light");
}
