// The History export dialog and the export behaviour it shares with the meeting exporter
// (OpenWhisper's tests/test_export_dialog_base.py and tests/test_history_export_dialog.py).
// Python runs the shared tests through both concrete dialogs; the meeting exporter is M7's, so
// they run through the History one, and the meeting worker's test through a stand-in subclass
// whose collector loads entries the way the meeting exporter's does.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractAnimation>
#include <QApplication>
#include <QCheckBox>
#include <QDate>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "history/export.h"
#include "history_ui_support.h"
#include "testkit/testkit.h"
#include "ui_history/export_dialog_base.h"
#include "ui_history/history_export_dialog.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

using fv::ExportDialogBase;
using fv::HistoryExportDialog;
using fv::Json;

namespace {

QString q(const char* text) {
    return QString::fromUtf8(text);
}

std::vector<Json> alphaBeta() {
    return {Json{{"id", "a"}, {"preview_text", "Alpha"}, {"title", "Alpha"}},
            Json{{"id", "b"}, {"preview_text", "Beta"}, {"title", "Beta"}}};
}

std::filesystem::path recordingsIn(const fv::testkit::TempDataRoot& root) {
    return root.path() / "recordings";
}

// _entry of test_history_export_dialog.py.
Json entry(const char* id, const char* preview = "Hello from history",
           const char* timestamp = "2026-03-15T14:30:00+00:00",
           const char* formatted = "Mar 15, 2026 02:30 PM") {
    return Json{{"id", id},
                {"timestamp", timestamp},
                {"formatted_timestamp", formatted},
                {"preview_text", preview},
                {"text", preview},
                {"has_audio", false},
                {"audio_file", nullptr},
                {"model", "local_whisper"},
                {"raw_text", nullptr}};
}

// The shared fixture's dialog, closed when the test ends.
struct Dialog {
    fv::testkit::TempDataRoot root;
    fvtest::Io io;
    fv::FakeClock clock{std::chrono::sys_days{std::chrono::year{2026} / 3 / 20}, "UTC"};
    std::unique_ptr<HistoryExportDialog> dialog;
    std::vector<std::pair<QMessageBox::Icon, QString>> messages;

    explicit Dialog(std::vector<Json> items = alphaBeta()) {
        dialog = std::make_unique<HistoryExportDialog>(
            [items] { return items; }, recordingsIn(root), io.executor(), nullptr, clock);
        dialog->showMessage = [this](QWidget*, QMessageBox::Icon icon, const QString&,
                                     const QString& text) {
            messages.emplace_back(icon, text);
        };
    }
    ~Dialog() {
        if (dialog->isBusy()) {
            dialog->setBusy(false);
        }
        dialog->close();
    }
    HistoryExportDialog* operator->() { return dialog.get(); }
};

QPropertyAnimation* animationOf(QParallelAnimationGroup* group, QObject* target) {
    for (int index = 0; index < group->animationCount(); ++index) {
        auto* animation = qobject_cast<QPropertyAnimation*>(group->animationAt(index));
        if (animation != nullptr && animation->targetObject() == target) {
            return animation;
        }
    }
    return nullptr;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

// ---- tests/test_export_dialog_base.py ----

TEST_CASE("scope animation can reverse without leaving height constraints",
          "[test_export_dialog_base][widget]") {
    // from
    // tests/test_export_dialog_base.py::test_scope_animation_can_reverse_without_leaving_height_constraints
    const double fontScale = GENERATE(1.0, 1.3);
    CAPTURE(fontScale);
    Dialog fixture;
    HistoryExportDialog& dialog = *fixture.dialog;
    QFont font = dialog.font();
    font.setPointSizeF(font.pointSizeF() * fontScale);
    dialog.setFont(font);
    dialog.show();
    QApplication::processEvents();
    dialog.resizeToContent();
    const int originalWidth = dialog.width();
    dialog.selectedRadio()->setChecked(true);
    QParallelAnimationGroup* group = dialog.animationGroup();
    REQUIRE(group != nullptr);
    group->setCurrentTime(group->duration() / 2);
    const int interruptedHeight = dialog.selectedPanel()->height();

    dialog.allRadio()->setChecked(true);
    QPropertyAnimation* shrinking = animationOf(group, dialog.selectedPanel());
    REQUIRE(shrinking != nullptr);
    CHECK(shrinking->startValue().toInt() == interruptedHeight);
    group->setCurrentTime(group->duration());
    QApplication::processEvents();

    CHECK(dialog.selectedPanel()->isHidden());
    CHECK(dialog.filtersPanel()->isVisible());
    CHECK(dialog.selectedPanel()->maximumHeight() == fv::kUnlimitedHeight);
    CHECK(dialog.filtersPanel()->maximumHeight() == fv::kUnlimitedHeight);
    CHECK(dialog.layout()->sizeConstraint() == QLayout::SetDefaultConstraint);
    CHECK(dialog.height() == dialog.contentHeight());
    CHECK(dialog.width() == originalWidth);
}

TEST_CASE("hidden scope and date changes are ready when shown",
          "[test_export_dialog_base][widget]") {
    // from
    // tests/test_export_dialog_base.py::test_hidden_scope_and_date_changes_are_ready_when_shown
    Dialog dialog;
    dialog->selectedRadio()->setChecked(true);
    dialog->dateRangeCheck()->setChecked(true);
    dialog->allRadio()->setChecked(true);
    CHECK(dialog->selectedPanel()->isHidden());
    CHECK_FALSE(dialog->filtersPanel()->isHidden());
    CHECK_FALSE(dialog->dateRangeRow()->isHidden());
    CHECK(dialog->dateRangeRow()->maximumHeight() == fv::kUnlimitedHeight);
}

TEST_CASE("select all applies only to search matches", "[test_export_dialog_base][widget]") {
    // from tests/test_export_dialog_base.py::test_select_all_applies_only_to_search_matches
    Dialog dialog;
    dialog->selectedRadio()->setChecked(true);
    dialog->selectedSearch()->setText(q("Alpha"));
    dialog->setAllChecked(true);
    CHECK(dialog->checkedIds() == std::vector<std::string>{"a"});
    CHECK(dialog->selectedCountLabel()->text() == q("1 of 2 selected"));
    CHECK(dialog->exportButton()->isEnabled());

    dialog->selectedSearch()->clear();
    dialog->setAllChecked(true);
    dialog->selectedSearch()->setText(q("Beta"));
    dialog->setAllChecked(false);
    CHECK(dialog->checkedIds() == std::vector<std::string>{"a"});
    CHECK(dialog->selectedList()->item(1)->checkState() == Qt::Unchecked);
}

TEST_CASE("close during export requests cancel and keeps dialog open",
          "[test_export_dialog_base][widget]") {
    // from
    // tests/test_export_dialog_base.py::test_close_during_export_requests_cancel_and_keeps_dialog_open
    Dialog dialog;
    dialog->show();
    QApplication::processEvents();
    dialog->setBusy(true);
    Q_EMIT dialog->progress(1, 2, q("Alpha"));
    CHECK(dialog->progressLabel()->text() == q("Exporting 1/2 — Alpha"));
    CHECK_FALSE(dialog->exportButton()->isVisible());
    CHECK(dialog->progressBar()->isVisible());

    dialog->close();
    CHECK(dialog->isVisible());
    CHECK(dialog->cancelRequested());
    CHECK_FALSE(dialog->cancelWorkButton()->isEnabled());
    Q_EMIT dialog->exportCanceled();
    CHECK_FALSE(dialog->isBusy());
    CHECK(dialog->exportButton()->isVisible());
    CHECK(dialog->closeButton()->isVisible());
    CHECK_FALSE(dialog->progressBar()->isVisible());
    CHECK(dialog->progressLabel()->text() == q("Export canceled — nothing was written"));
}

TEST_CASE("changing format during scope animation finishes both sections",
          "[test_export_dialog_base][widget]") {
    // from
    // tests/test_export_dialog_base.py::test_changing_format_during_scope_animation_finishes_both_sections
    Dialog dialog;
    dialog->show();
    QApplication::processEvents();
    dialog->selectedRadio()->setChecked(true);
    QParallelAnimationGroup* group = dialog->animationGroup();
    REQUIRE(group != nullptr);
    group->setCurrentTime(group->duration() / 2);
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("json")));
    group->setCurrentTime(group->duration());
    QApplication::processEvents();
    CHECK(dialog->filtersPanel()->isHidden());
    CHECK(dialog->selectedPanel()->isVisible());
    CHECK(dialog->selectedPanel()->maximumHeight() == fv::kUnlimitedHeight);
    CHECK(dialog->filtersPanel()->maximumHeight() == fv::kUnlimitedHeight);
    CHECK(dialog->markdownOnlyHint()->isVisible());
}

TEST_CASE("format values match both export services", "[test_export_dialog_base][unit]") {
    // from tests/test_export_dialog_base.py::test_format_values_match_both_export_services
    // The meeting exporter's values (meeting.export.bulk) arrive with M7.
    CHECK(fv::export_format::markdown == fv::history::FORMAT_MARKDOWN);
    CHECK(fv::export_format::txt == fv::history::FORMAT_TXT);
    CHECK(fv::export_format::json == fv::history::FORMAT_JSON);
}

namespace {

struct WriterCall {
    std::string kind;
    std::vector<Json> entries;
    std::string format;
    Json options;
};

// _run_export: runs an export on io to its end and returns what the dialog reported.
std::vector<std::pair<int, QString>> runExport(ExportDialogBase& dialog, fvtest::Io& io,
                                               bool perItem) {
    std::vector<std::pair<int, QString>> emitted;
    QObject::connect(
        &dialog, &ExportDialogBase::exportFinished,
        [&emitted](int count, const QString& out) { emitted.emplace_back(count, out); });
    QObject::connect(&dialog, &ExportDialogBase::exportFailed,
                     [&emitted](const QString& message) { emitted.emplace_back(-1, message); });
    dialog.perItemCheck()->setChecked(perItem);
    dialog.startExport();
    io.drain();
    CHECK_FALSE(dialog.isBusy());
    return emitted;
}

} // namespace

TEST_CASE("history worker passes include options to writers", "[test_export_dialog_base][widget]") {
    // from tests/test_export_dialog_base.py::test_history_worker_passes_include_options_to_writers
    const bool perItem = GENERATE(false, true);
    CAPTURE(perItem);
    fv::testkit::TempDataRoot out;
    Dialog dialog({Json{{"id", "a"}, {"preview_text", "Alpha"}},
                   Json{{"id", "b"}, {"preview_text", "Beta"}}});
    auto calls = std::make_shared<std::vector<WriterCall>>();
    auto mutex = std::make_shared<std::mutex>();
    dialog->renderDocument = [calls, mutex](const std::vector<Json>& entries,
                                            const std::string& format, const Json& options) {
        const std::lock_guard lock(*mutex);
        calls->push_back({"doc", entries, format, options});
        return std::string("doc");
    };
    dialog->writePerEntryFiles = [calls, mutex](const std::vector<Json>& entries,
                                                const std::string& format, const std::string&,
                                                const Json& options) {
        const std::lock_guard lock(*mutex);
        calls->push_back({"files", entries, format, options});
    };
    dialog->includeRawCheck()->setChecked(false);
    const std::filesystem::path target = out.path() / "out.md";
    const QString path = fv::qt::toQString(fv::path_to_utf8(target));
    dialog->pathEdit()->setText(path);
    const auto emitted = runExport(*dialog.dialog, dialog.io, perItem);

    const std::lock_guard lock(*mutex);
    REQUIRE(calls->size() == 1);
    const WriterCall& call = calls->front();
    CHECK(call.kind == (perItem ? "files" : "doc"));
    REQUIRE(call.entries.size() == 2);
    CHECK(call.entries[0]["id"] == "a");
    CHECK(call.entries[1]["id"] == "b");
    CHECK(call.format == "markdown");
    CHECK(call.options == Json{{"include_cleaned", true}, {"include_raw", false}});
    CHECK(emitted == std::vector<std::pair<int, QString>>{{2, path}});
    if (!perItem) {
        CHECK(readFile(target) == "doc");
    }
}

namespace {

// The meeting exporter's shape: meetings to pick from, an entry loader the collector calls on the
// worker (a missing meeting is skipped), and its own include options.
class MeetingLikeDialog final : public ExportDialogBase {
public:
    MeetingLikeDialog(Items meetings, std::map<std::string, Json> loaded,
                      std::filesystem::path root, fv::Executor& io)
        : ExportDialogBase(
              copy(), [meetings] { return meetings; }, io),
          loaded_(std::move(loaded)), root_(std::move(root)) {
        setup();
    }

    QCheckBox* includeIntelligenceCheck() const { return intelligence_; }
    std::function<std::string(const Items&, const std::string&, const Json&)> render;
    std::function<void(const Items&, const std::string&, const std::string&, const Json&)> write;

protected:
    QCheckBox* buildCriteriaCheck() override {
        return new QCheckBox(q("Only meetings with audio"));
    }
    std::vector<QCheckBox*> buildIncludeChecks() override {
        transcript_ = new QCheckBox(q("Include transcript"));
        intelligence_ = new QCheckBox(q("Include intelligence"));
        return {transcript_, intelligence_};
    }
    Json includeOptions() const override {
        return Json{{"include_transcript", transcript_->isChecked()},
                    {"include_intelligence", intelligence_->isChecked()}};
    }
    QString itemLabel(const Json& item) const override {
        return fv::qt::toQString(item.value("title", std::string()));
    }
    std::filesystem::path exportRoot() const override { return root_; }
    Items filterItems(Items items, const std::optional<fv::py::DateTime>&,
                      const std::optional<fv::py::DateTime>&) const override {
        return items;
    }
    Collector entryCollector() const override {
        return [loaded = loaded_](const Json& item) -> std::optional<Json> {
            const auto found = loaded.find(item.value("id", std::string()));
            if (found == loaded.end()) {
                return std::nullopt;
            }
            return std::optional<Json>(std::in_place, found->second);
        };
    }
    TitleOf progressTitle() const override {
        return [](const Json& item) {
            return fv::qt::toQString(item.value("title", std::string()));
        };
    }
    WriteFiles perItemWriter() const override { return write; }
    Render documentRenderer() const override { return render; }

private:
    static fv::ExportDialogCopy copy() {
        fv::ExportDialogCopy made;
        made.title = q("Export Meetings");
        made.noun = q("meeting");
        made.nounPlural = q("meetings");
        made.defaultDirName = q("meetings_export");
        made.defaultFileStem = q("meetings");
        return made;
    }

    std::map<std::string, Json> loaded_;
    std::filesystem::path root_;
    QCheckBox* transcript_ = nullptr;
    QCheckBox* intelligence_ = nullptr;
};

} // namespace

TEST_CASE("meeting worker loads entries and skips missing", "[test_export_dialog_base][widget]") {
    // from tests/test_export_dialog_base.py::test_meeting_worker_loads_entries_and_skips_missing
    const bool perItem = GENERATE(false, true);
    CAPTURE(perItem);
    fv::testkit::TempDataRoot root;
    fvtest::Io io;
    MeetingLikeDialog dialog(
        {Json{{"id", "a"}, {"title", "Alpha"}}, Json{{"id", "b"}, {"title", "Beta"}}},
        {{"a", Json{{"meeting", "a"}}}}, root.path(), io.executor());
    dialog.showMessage = [](QWidget*, QMessageBox::Icon, const QString&, const QString&) {
    };
    auto calls = std::make_shared<std::vector<WriterCall>>();
    dialog.render = [calls](const std::vector<Json>& entries, const std::string& format,
                            const Json& options) {
        calls->push_back({"doc", entries, format, options});
        return std::string("doc");
    };
    dialog.write = [calls](const std::vector<Json>& entries, const std::string& format,
                           const std::string&, const Json& options) {
        calls->push_back({"files", entries, format, options});
    };
    dialog.includeIntelligenceCheck()->setChecked(false);
    const QString path = fv::qt::toQString(fv::path_to_utf8(root.path() / "out.md"));
    dialog.pathEdit()->setText(path);
    const auto emitted = runExport(dialog, io, perItem);

    REQUIRE(calls->size() == 1);
    CHECK(calls->front().kind == (perItem ? "files" : "doc"));
    CHECK(calls->front().entries == std::vector<Json>{Json{{"meeting", "a"}}});
    CHECK(calls->front().options ==
          Json{{"include_transcript", true}, {"include_intelligence", false}});
    CHECK(emitted == std::vector<std::pair<int, QString>>{{1, path}});
}

TEST_CASE("a dialog deleted mid export is not held by the worker",
          "[test_export_dialog_base][widget]") {
    // from
    // tests/test_export_dialog_base.py::test_a_dialog_deleted_mid_export_is_not_held_by_the_worker
    // The worker used to run as a method of the dialog: it kept a deleted dialog alive, then
    // dropped the last reference (deleting a widget) or emitted on it from its own thread.
    fv::testkit::TempDataRoot root;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;
        bool rendering = false;
    };
    auto gate = std::make_shared<Gate>();
    const std::filesystem::path target = root.path() / "out.md";
    {
        fv::ThreadPool io("export-test", 1);
        QPointer<HistoryExportDialog> dialog = new HistoryExportDialog(
            [] { return std::vector<Json>{Json{{"id", "a"}, {"preview_text", "Alpha"}}}; },
            recordingsIn(root), io);
        dialog->renderDocument = [gate](const std::vector<Json>&, const std::string&, const Json&) {
            std::unique_lock lock(gate->mutex);
            gate->rendering = true;
            gate->changed.notify_all();
            gate->changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate->open; });
            return std::string("doc");
        };
        dialog->pathEdit()->setText(fv::qt::toQString(fv::path_to_utf8(target)));
        dialog->startExport();
        {
            std::unique_lock lock(gate->mutex);
            gate->changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate->rendering; });
        }
        dialog->deleteLater();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(dialog == nullptr);
        {
            const std::lock_guard lock(gate->mutex);
            gate->open = true;
        }
        gate->changed.notify_all();
        // The pool's destructor waits for the worker, which finishes without the dialog.
    }
    QApplication::processEvents();
    CHECK(readFile(target) == "doc");
}

// ---- tests/test_history_export_dialog.py ----

TEST_CASE("TestHistoryExportDialog empty state has no export button",
          "[test_history_export_dialog][widget]") {
    // from
    // tests/test_history_export_dialog.py::TestHistoryExportDialog::test_empty_state_has_no_export_button
    Dialog dialog(std::vector<Json>{});
    CHECK_FALSE(dialog->hasControls());
    bool found = false;
    for (fv::WrappedLabel* label : dialog->findChildren<fv::WrappedLabel*>()) {
        found = found || label->text().contains(q("No transcriptions to export yet."));
    }
    CHECK(found);
}

TEST_CASE("TestHistoryExportDialog resolve targets filters date range",
          "[test_history_export_dialog][widget]") {
    // from
    // tests/test_history_export_dialog.py::TestHistoryExportDialog::test_resolve_targets_filters_date_range
    Dialog dialog({entry("h_old", "Old", "2026-01-02T09:00:00+00:00"),
                   entry("h_mid", "Mid", "2026-03-15T14:30:00+00:00"),
                   entry("h_new", "New", "2026-06-01T09:00:00+00:00")});
    dialog->dateRangeCheck()->setChecked(true);
    dialog->fromDate()->setDate(QDate(2026, 3, 1));
    dialog->toDate()->setDate(QDate(2026, 3, 31));
    const std::vector<Json> targets = dialog->resolveTargets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["id"] == "h_mid");
}

TEST_CASE("TestHistoryExportDialog selected scope uses checked entries",
          "[test_history_export_dialog][widget]") {
    // from
    // tests/test_history_export_dialog.py::TestHistoryExportDialog::test_selected_scope_uses_checked_entries
    Dialog dialog({entry("h_a", "Alpha"), entry("h_b", "Beta")});
    dialog->selectedRadio()->setChecked(true);
    dialog->selectedList()->item(1)->setCheckState(Qt::Checked);
    dialog->refreshExportEnabled();
    const std::vector<Json> targets = dialog->resolveTargets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["id"] == "h_b");
    CHECK(dialog->exportButton()->isEnabled());
}

TEST_CASE("TestHistoryExportDialog markdown toggles disable for json",
          "[test_history_export_dialog][widget]") {
    // from
    // tests/test_history_export_dialog.py::TestHistoryExportDialog::test_markdown_toggles_disable_for_json
    Dialog dialog({entry("h_a")});
    CHECK(dialog->includeCleanedCheck()->isEnabled());
    CHECK(dialog->includeRawCheck()->isEnabled());
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("json")));
    CHECK(dialog->currentFormat() == q("json"));
    CHECK_FALSE(dialog->includeCleanedCheck()->isEnabled());
    CHECK_FALSE(dialog->includeRawCheck()->isEnabled());
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("markdown")));
    CHECK(dialog->includeCleanedCheck()->isEnabled());
}

TEST_CASE("TestHistoryExportDialog default path is absolute and follows format",
          "[test_history_export_dialog][widget]") {
    // from
    // tests/test_history_export_dialog.py::TestHistoryExportDialog::test_default_path_is_absolute_and_follows_format
    // OpenWhisper_CPP's single-file name is "<product in lower case>_history" (lead decision 4).
    Dialog dialog({entry("h_a")});
    std::filesystem::create_directories(recordingsIn(dialog.root));
    const std::filesystem::path path = fv::path_from_utf8(fv::qt::toUtf8(dialog->defaultPath()));
    CHECK(path.is_absolute());
    CHECK(fv::path_to_utf8(path.filename()) == "openwhisper_cpp_history.md");
    CHECK(path.parent_path() == dialog.root.path());
    dialog->perItemCheck()->setChecked(true);
    CHECK(dialog->defaultPath().endsWith(q("history_export")));
    // Still a default, it follows the format.
    dialog->perItemCheck()->setChecked(false);
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("txt")));
    CHECK(dialog->pathEdit()->text().endsWith(q("openwhisper_cpp_history.txt")));
    // A path of the user's own stays.
    dialog->pathEdit()->setText(q("C:/elsewhere/mine.md"));
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("json")));
    CHECK(dialog->pathEdit()->text() == q("C:/elsewhere/mine.md"));
}

TEST_CASE("TestHistoryExportDialog export without matches does not start worker",
          "[test_history_export_dialog][widget]") {
    // from
    // tests/test_history_export_dialog.py::TestHistoryExportDialog::test_export_without_matches_does_not_start_worker
    Dialog dialog({entry("h_old", "Hello from history", "2026-01-02T09:00:00+00:00")});
    dialog->dateRangeCheck()->setChecked(true);
    dialog->fromDate()->setDate(QDate(2026, 6, 1));
    dialog->toDate()->setDate(QDate(2026, 6, 30));
    dialog->startExport();
    REQUIRE(dialog.messages.size() == 1);
    CHECK(dialog.messages[0].first == QMessageBox::Information);
    CHECK(dialog.messages[0].second == q("No transcriptions match the selected criteria."));
    CHECK_FALSE(dialog->isBusy());
    CHECK(dialog.io.executor().queued() == 0);
}

// ---- beyond OpenWhisper's tests ----

TEST_CASE("a single-file export is written in text mode", "[widget]") {
    // _write_document opens the file in Python's text mode: CRLF line endings on Windows.
    fv::testkit::TempDataRoot out;
    const std::filesystem::path target = out.path() / "nested" / "export.md";
    fv::export_detail::write_document(target, "one\ntwo\n");
#ifdef _WIN32
    CHECK(readFile(target) == "one\r\ntwo\r\n");
#else
    CHECK(readFile(target) == "one\ntwo\n");
#endif
}

TEST_CASE("exporting all history writes every transcription", "[widget]") {
    // The demo's step: "Export history…" writes openwhisper_cpp_history.md with every
    // transcription.
    Dialog dialog({entry("h_a", "Alpha says hello"), entry("h_b", "Beta says goodbye")});
    const std::filesystem::path target = fv::path_from_utf8(fv::qt::toUtf8(dialog->defaultPath()));
    CHECK(fv::path_to_utf8(target.filename()) == "openwhisper_cpp_history.md");
    std::vector<std::pair<int, QString>> finished;
    QObject::connect(
        dialog.dialog.get(), &ExportDialogBase::exportFinished,
        [&finished](int count, const QString& output) { finished.emplace_back(count, output); });
    dialog->startExport();
    CHECK(dialog->isBusy());
    dialog.io.drain();
    CHECK_FALSE(dialog->isBusy());
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].first == 2);
    const std::string written = readFile(target);
    CHECK(written.find("Alpha says hello") != std::string::npos);
    CHECK(written.find("Beta says goodbye") != std::string::npos);
    REQUIRE(dialog.messages.size() == 1);
    CHECK(dialog.messages[0].second ==
          q("Exported 2 transcriptions to:\n") + fv::qt::toQString(fv::path_to_utf8(target)));
}

TEST_CASE("a failed export says why", "[widget]") {
    // _on_export_failed: "Export Failed" with "Could not export history:\n<error>".
    Dialog dialog({entry("h_a")});
    dialog->renderDocument = [](const std::vector<Json>&, const std::string&,
                                const Json&) -> std::string {
        throw std::runtime_error("disk full");
    };
    dialog->startExport();
    dialog.io.drain();
    REQUIRE(dialog.messages.size() == 1);
    CHECK(dialog.messages[0].first == QMessageBox::Warning);
    CHECK(dialog.messages[0].second == q("Could not export history:\ndisk full"));
    CHECK_FALSE(dialog->exportButton()->isHidden());
}

TEST_CASE("an item with a number for an id can be picked and exported", "[widget]") {
    // _make_list_item and _resolve_targets read the id as str(item.get("id") or ""): a meeting's
    // numeric id is checkable and exported; a missing or zero one never is.
    Dialog dialog({Json{{"id", 7}, {"preview_text", "Seven"}},
                   Json{{"id", 0}, {"preview_text", "Zero"}}, Json{{"preview_text", "None"}}});
    dialog->selectedRadio()->setChecked(true);
    for (int row = 0; row < dialog->selectedList()->count(); ++row) {
        dialog->selectedList()->item(row)->setCheckState(Qt::Checked);
    }
    CHECK(dialog->checkedIds() == std::vector<std::string>{"7"});
    const std::vector<Json> targets = dialog->resolveTargets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["preview_text"] == "Seven");
    CHECK(dialog->exportButton()->isEnabled());
}
