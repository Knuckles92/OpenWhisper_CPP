// The Past Meetings export dialog (OpenWhisper's tests/test_meeting_export_dialog.py), and the
// meeting side of the export it shares with History: the entry loader on io, the documents, the
// labels. The shared dialog behaviour is tests/ui_history's.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QDate>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>

#include "appkit/qt_bridge.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "meeting_state/export_bulk.h"
#include "past_meetings_support.h"
#include "testkit/testkit.h"
#include "ui_history/export_dialog_base.h"
#include "ui_meeting/meeting_export_dialog.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::MeetingExportDialog;

namespace {

QString q(const char* text) {
    return QString::fromUtf8(text);
}

// _meeting of the Python test.
Json meeting(const char* id, const char* title = "Review",
             const char* started = "2026-03-15T14:30:00", bool hasTranscript = true) {
    return Json{{"id", id},
                {"title", title},
                {"status", "ended"},
                {"started_at", started},
                {"content_summary", {{"has_transcript", hasTranscript}}}};
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// The dialog over meetings, with an entry loader that answers from a table (a meeting not in it
// is gone) and a message hook that records what the user is told.
struct Dialog {
    explicit Dialog(std::vector<Json> meetings,
                    std::map<std::string, fv::meeting_state::MeetingExport> loadable = {})
        : loaded(std::move(loadable)) {
        dialog = std::make_unique<MeetingExportDialog>(
            [meetings] { return meetings; }, meetingsFolder(),
            [this](const std::string& id) -> std::optional<fv::meeting_state::MeetingExport> {
                loads.push_back(id);
                const auto found = loaded.find(id);
                if (found == loaded.end()) {
                    return std::nullopt;
                }
                return found->second;
            },
            io.executor(), nullptr, clock);
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
    MeetingExportDialog* operator->() { return dialog.get(); }
    std::filesystem::path meetingsFolder() const { return root.path() / "meetings"; }

    fv::testkit::TempDataRoot root;
    fvtest::Io io;
    fv::FakeClock clock{std::chrono::sys_days{std::chrono::year{2026} / 3 / 20}, "UTC"};
    std::map<std::string, fv::meeting_state::MeetingExport> loaded;
    std::vector<std::string> loads;
    std::unique_ptr<MeetingExportDialog> dialog;
    std::vector<std::pair<QMessageBox::Icon, QString>> messages;
};

fv::meeting_state::MeetingExport exportEntry(const Json& row, const char* spoken) {
    fv::meeting_state::MeetingExport entry;
    entry.meeting = row;
    entry.state = Json::object();
    entry.segments.push_back(Json{{"segment_id", "sg_1"},
                                  {"channel", "mic"},
                                  {"start_s", 1.0},
                                  {"end_s", 3.0},
                                  {"text", spoken}});
    return entry;
}

} // namespace

// from
// tests/test_meeting_export_dialog.py::TestMeetingExportDialog::test_empty_state_has_no_export_button
TEST_CASE("test_empty_state_has_no_export_button", "[test_meeting_export_dialog][widget]") {
    Dialog dialog({});

    CHECK_FALSE(dialog->hasControls());
    CHECK(dialog->exportButton() == nullptr);
    bool found = false;
    for (fv::WrappedLabel* label : dialog->findChildren<fv::WrappedLabel*>()) {
        found = found || label->text().contains(q("No past meetings to export yet."));
    }
    CHECK(found);
}

// from
// tests/test_meeting_export_dialog.py::TestMeetingExportDialog::test_resolve_targets_filters_date_range
TEST_CASE("test_resolve_targets_filters_date_range", "[test_meeting_export_dialog][widget]") {
    Dialog dialog({meeting("m_old", "Old", "2026-01-02T09:00:00"),
                   meeting("m_mid", "Mid", "2026-03-15T14:30:00"),
                   meeting("m_new", "New", "2026-06-01T09:00:00")});

    dialog->dateRangeCheck()->setChecked(true);
    dialog->fromDate()->setDate(QDate(2026, 3, 1));
    dialog->toDate()->setDate(QDate(2026, 3, 31));
    const std::vector<Json> targets = dialog->resolveTargets();

    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["id"] == "m_mid");
}

// from
// tests/test_meeting_export_dialog.py::TestMeetingExportDialog::test_selected_scope_uses_checked_meetings
TEST_CASE("test_selected_scope_uses_checked_meetings", "[test_meeting_export_dialog][widget]") {
    Dialog dialog({meeting("m_a", "Alpha"), meeting("m_b", "Beta")});

    dialog->selectedRadio()->setChecked(true);
    dialog->selectedList()->item(1)->setCheckState(Qt::Checked);
    dialog->refreshExportEnabled();
    const std::vector<Json> targets = dialog->resolveTargets();

    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["id"] == "m_b");
    CHECK(dialog->exportButton()->isEnabled());
}

// from
// tests/test_meeting_export_dialog.py::TestMeetingExportDialog::test_markdown_toggles_disable_for_json
TEST_CASE("test_markdown_toggles_disable_for_json", "[test_meeting_export_dialog][widget]") {
    Dialog dialog({meeting("m_a")});

    CHECK(dialog->includeTranscriptCheck()->isEnabled());
    CHECK(dialog->includeIntelligenceCheck()->isEnabled());
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("json")));
    CHECK(dialog->currentFormat() == q("json"));
    CHECK_FALSE(dialog->includeTranscriptCheck()->isEnabled());
    CHECK_FALSE(dialog->includeIntelligenceCheck()->isEnabled());
    dialog->formatCombo()->setCurrentIndex(dialog->formatCombo()->findData(q("markdown")));
    CHECK(dialog->includeTranscriptCheck()->isEnabled());
}

// from
// tests/test_meeting_export_dialog.py::TestMeetingExportDialog::test_default_path_is_absolute_and_follows_format
TEST_CASE("test_default_path_is_absolute_and_follows_format",
          "[test_meeting_export_dialog][widget]") {
    // FioriVoce's single-file name is "<product in lower case>_meetings" (the Python test reads
    // openwhisper_meetings.md), beside the meetings folder.
    Dialog dialog({meeting("m_a")});
    std::filesystem::create_directories(dialog.meetingsFolder());

    const std::filesystem::path path = fv::path_from_utf8(fv::qt::toUtf8(dialog->defaultPath()));
    CHECK(path.is_absolute());
    CHECK(fv::path_to_utf8(path.filename()) == "fiorivoce_meetings.md");
    CHECK(path.parent_path().lexically_normal() == dialog.root.path().lexically_normal());
    dialog->perItemCheck()->setChecked(true);
    CHECK(dialog->defaultPath().endsWith(q("meetings_export")));
}

// from
// tests/test_meeting_export_dialog.py::TestMeetingExportDialog::test_export_without_matches_does_not_start_worker
TEST_CASE("test_export_without_matches_does_not_start_worker",
          "[test_meeting_export_dialog][widget]") {
    Dialog dialog({meeting("m_old", "Review", "2026-01-02T09:00:00")});

    dialog->dateRangeCheck()->setChecked(true);
    dialog->fromDate()->setDate(QDate(2026, 6, 1));
    dialog->toDate()->setDate(QDate(2026, 6, 30));
    dialog->startExport();

    REQUIRE(dialog.messages.size() == 1);
    CHECK(dialog.messages[0].first == QMessageBox::Information);
    CHECK(dialog.messages[0].second == q("No meetings match the selected criteria."));
    CHECK_FALSE(dialog->isBusy());
    CHECK(dialog.loads.empty());
}

TEST_CASE("the items read like the History rows: the title and the local start",
          "[test_meeting_export_dialog][widget]") {
    Dialog dialog({meeting("m_a", "Alpha", "2026-03-15T14:30:00"),
                   meeting("m_b", "", "2026-03-15T09:05:00"),
                   Json{{"id", "m_c"}, {"title", "No date"}, {"status", "ended"}}});

    QStringList labels;
    for (int row = 0; row < dialog->selectedList()->count(); ++row) {
        labels << dialog->selectedList()->item(row)->text();
    }
    CHECK(labels == QStringList{q("Alpha — 2026-03-15 14:30"),
                                q("Untitled meeting — 2026-03-15 09:05"),
                                q("No date — unknown date")});
}

TEST_CASE("only meetings with a transcript drops the ones known to have none",
          "[test_meeting_export_dialog][widget]") {
    Dialog dialog(
        {meeting("m_with"), meeting("m_without", "Silent", "2026-03-15T14:30:00", false)});

    CHECK(dialog->resolveTargets().size() == 2);
    dialog->transcriptOnlyCheck()->setChecked(true);
    const std::vector<Json> targets = dialog->resolveTargets();

    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["id"] == "m_with");
}

TEST_CASE("exporting all meetings writes every transcript on io and skips a vanished one",
          "[test_meeting_export_dialog][widget]") {
    const Json alpha = meeting("m_a", "Alpha");
    const Json beta = meeting("m_b", "Beta");
    const Json gone = meeting("m_gone", "Deleted meanwhile");
    Dialog dialog({alpha, beta, gone}, {{"m_a", exportEntry(alpha, "Alpha says hello")},
                                        {"m_b", exportEntry(beta, "Beta says goodbye")}});
    const std::filesystem::path target = fv::path_from_utf8(fv::qt::toUtf8(dialog->defaultPath()));
    CHECK(fv::path_to_utf8(target.filename()) == "fiorivoce_meetings.md");
    std::vector<int> finished;
    QObject::connect(dialog.dialog.get(), &fv::ExportDialogBase::exportFinished,
                     [&finished](int count, const QString&) { finished.push_back(count); });

    dialog->startExport();
    CHECK(dialog->isBusy());
    dialog.io.drain();

    CHECK_FALSE(dialog->isBusy());
    CHECK(finished == std::vector<int>{2});
    CHECK(dialog.loads == std::vector<std::string>{"m_a", "m_b", "m_gone"});
    const std::string written = readFile(target);
    CHECK(written.find("Alpha says hello") != std::string::npos);
    CHECK(written.find("Beta says goodbye") != std::string::npos);
    CHECK(written.find("Deleted meanwhile") == std::string::npos);
}

TEST_CASE("the transcript can be left out of a Markdown export",
          "[test_meeting_export_dialog][widget]") {
    const Json alpha = meeting("m_a", "Alpha");
    Dialog dialog({alpha}, {{"m_a", exportEntry(alpha, "Alpha says hello")}});
    const std::filesystem::path target = fv::path_from_utf8(fv::qt::toUtf8(dialog->defaultPath()));

    dialog->includeTranscriptCheck()->setChecked(false);
    dialog->startExport();
    dialog.io.drain();

    const std::string written = readFile(target);
    CHECK(written.find("Alpha") != std::string::npos);
    CHECK(written.find("Alpha says hello") == std::string::npos);
}

TEST_CASE("one file per meeting goes into the folder", "[test_meeting_export_dialog][widget]") {
    const Json alpha = meeting("m_a", "Alpha");
    const Json beta = meeting("m_b", "Beta");
    Dialog dialog({alpha, beta}, {{"m_a", exportEntry(alpha, "Alpha says hello")},
                                  {"m_b", exportEntry(beta, "Beta says goodbye")}});
    dialog->perItemCheck()->setChecked(true);
    const std::filesystem::path folder = fv::path_from_utf8(fv::qt::toUtf8(dialog->defaultPath()));

    dialog->startExport();
    dialog.io.drain();

    std::vector<std::string> names;
    for (const auto& file : std::filesystem::directory_iterator(folder)) {
        names.push_back(fv::path_to_utf8(file.path().extension()));
    }
    CHECK(names == std::vector<std::string>{".markdown", ".markdown"});
}

TEST_CASE("an export entry survives the trip between the steps", "[test_meeting_export_dialog]") {
    const Json alpha = meeting("m_a", "Alpha");
    const fv::meeting_state::MeetingExport entry = exportEntry(alpha, "Hello");

    const fv::meeting_state::MeetingExport back =
        MeetingExportDialog::entryFromJson(MeetingExportDialog::entryToJson(entry));

    CHECK(back.meeting == entry.meeting);
    CHECK(back.state == entry.state);
    CHECK(back.segments == entry.segments);
}
