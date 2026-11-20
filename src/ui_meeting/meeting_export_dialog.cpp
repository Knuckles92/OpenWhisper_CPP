#include "ui_meeting/meeting_export_dialog.h"

#include <format>
#include <utility>

#include <QCheckBox>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "meeting_state/content.h"
#include "meeting_state/time_utils.h"
#include "pyport/text.h"
#include "ui_meeting/detail/payload.h"

namespace fv {
namespace {

meeting_state::BulkExportOptions exportOptions(const Json& options) {
    meeting_state::BulkExportOptions made;
    made.include_transcript = options.value("include_transcript", true);
    made.include_intelligence = options.value("include_intelligence", true);
    return made;
}

std::vector<meeting_state::MeetingExport> entriesFrom(const ExportDialogBase::Items& entries) {
    std::vector<meeting_state::MeetingExport> made;
    made.reserve(entries.size());
    for (const Json& entry : entries) {
        made.push_back(MeetingExportDialog::entryFromJson(entry));
    }
    return made;
}

ExportDialogCopy meetingCopy() {
    ExportDialogCopy copy;
    copy.dialogName = QStringLiteral("meetingExportDialog");
    copy.cardName = QStringLiteral("meetingExportCard");
    copy.eyebrowName = QStringLiteral("meetingExportEyebrow");
    copy.metaName = QStringLiteral("meetingExportMeta");
    copy.miniButtonName = QStringLiteral("meetingExportMiniBtn");
    copy.paneName = QStringLiteral("meetingExportPane");
    copy.listName = QStringLiteral("meetingExportList");
    copy.progressName = QStringLiteral("meetingExportProgress");
    copy.title = QStringLiteral("Export Past Meetings");
    copy.subtitle = QStringLiteral("Choose which meetings to export, what each document "
                                   "includes, and where the files are written.");
    copy.loadError = QStringLiteral("Past meetings could not be loaded");
    copy.loadLog = "Could not load past meetings for export";
    copy.emptyText = QStringLiteral("No past meetings to export yet.");
    copy.scopeEyebrow = QStringLiteral("MEETINGS");
    copy.allLabel = QStringLiteral("All past meetings");
    copy.selectedLabel = QStringLiteral("Selected meetings");
    copy.searchPlaceholder = QStringLiteral("Search meetings…");
    copy.noun = QStringLiteral("meeting");
    copy.nounPlural = QStringLiteral("meetings");
    copy.markdownHint = QStringLiteral("Transcript and intelligence sections apply to Markdown "
                                       "exports; other formats always include everything "
                                       "available.");
    copy.failedText = QStringLiteral("Could not export past meetings");
    copy.failedLog = "Past meetings export failed";
    copy.defaultDirName = QStringLiteral("meetings_export");
    copy.defaultFileStem = qt::toQString(py::lower(identity().product) + "_meetings");
    return copy;
}

} // namespace

Json MeetingExportDialog::entryToJson(const meeting_state::MeetingExport& entry) {
    Json segments = Json::array();
    for (const Json& segment : entry.segments) {
        segments.push_back(segment);
    }
    return Json{{"meeting", entry.meeting}, {"state", entry.state}, {"segments", segments}};
}

meeting_state::MeetingExport MeetingExportDialog::entryFromJson(const Json& entry) {
    meeting_state::MeetingExport made;
    made.meeting = entry.at("meeting");
    made.state = entry.at("state");
    for (const Json& segment : entry.at("segments")) {
        made.segments.push_back(segment);
    }
    return made;
}

MeetingExportDialog::MeetingExportDialog(ItemProvider provider,
                                         std::filesystem::path meetingsFolder,
                                         EntryLoader entryLoader, Executor& io, QWidget* parent,
                                         const Clock& clock)
    : ExportDialogBase(meetingCopy(), std::move(provider), io, parent),
      meetingsFolder_(std::move(meetingsFolder)), entryLoader_(std::move(entryLoader)),
      clock_(&clock) {
    const Clock* bound = clock_;
    renderDocument = [bound](const Items& entries, const std::string& format, const Json& options) {
        return meeting_state::render_combined(entriesFrom(entries), format, exportOptions(options),
                                              *bound);
    };
    writePerMeetingFiles = [bound](const Items& entries, const std::string& format,
                                   const std::string& output, const Json& options) {
        meeting_state::write_per_meeting_files(path_from_utf8(output), format, entriesFrom(entries),
                                               exportOptions(options), *bound);
    };
    setup();
}

QCheckBox* MeetingExportDialog::buildCriteriaCheck() {
    transcriptOnlyCheck_ = new QCheckBox(QStringLiteral("Only meetings with a transcript"));
    return transcriptOnlyCheck_;
}

std::vector<QCheckBox*> MeetingExportDialog::buildIncludeChecks() {
    includeTranscriptCheck_ = new QCheckBox(QStringLiteral("Include transcript"));
    includeIntelligenceCheck_ =
        new QCheckBox(QStringLiteral("Include meeting intelligence (notes, decisions, actions)"));
    return {includeTranscriptCheck_, includeIntelligenceCheck_};
}

Json MeetingExportDialog::includeOptions() const {
    Json options = Json::object();
    options["include_transcript"] = includeTranscriptCheck_->isChecked();
    options["include_intelligence"] = includeIntelligenceCheck_->isChecked();
    return options;
}

QString MeetingExportDialog::itemLabel(const Json& meeting) const {
    const auto started =
        meeting_state::as_local_time(ui_meeting::detail::get(meeting, "started_at"), *clock_);
    const std::string startedText =
        started ? std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}", started->year, started->month,
                              started->day, started->hour, started->minute)
                : std::string("unknown date");
    return qt::toQString(meeting_state::fallback_meeting_title(meeting) + " — " + startedText);
}

std::filesystem::path MeetingExportDialog::exportRoot() const {
    // os.path.dirname(os.path.abspath(MEETINGS_FOLDER)).
    std::filesystem::path folder = std::filesystem::absolute(meetingsFolder_).lexically_normal();
    if (!folder.has_filename()) {
        folder = folder.parent_path();
    }
    return folder.parent_path();
}

ExportDialogBase::Items
MeetingExportDialog::filterItems(Items items, const std::optional<py::DateTime>& from,
                                 const std::optional<py::DateTime>& to) const {
    return meeting_state::filter_export_meetings(items, from, to, transcriptOnlyCheck_->isChecked(),
                                                 *clock_);
}

ExportDialogBase::Collector MeetingExportDialog::entryCollector() const {
    EntryLoader loader = entryLoader_;
    return [loader](const Json& meeting) -> std::optional<Json> {
        if (!loader) {
            return std::nullopt;
        }
        const std::optional<meeting_state::MeetingExport> entry =
            loader(ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(meeting, "id")));
        if (!entry) {
            return std::nullopt;
        }
        return entryToJson(*entry);
    };
}

ExportDialogBase::TitleOf MeetingExportDialog::progressTitle() const {
    return [](const Json& meeting) {
        return qt::toQString(meeting_state::fallback_meeting_title(meeting));
    };
}

ExportDialogBase::WriteFiles MeetingExportDialog::perItemWriter() const {
    return writePerMeetingFiles;
}

ExportDialogBase::Render MeetingExportDialog::documentRenderer() const {
    return renderDocument;
}

MeetingExportDialog::ItemProvider
meetingExportProvider(std::shared_ptr<meeting_state::SqlMeetingRepository> repository) {
    return [repository = std::move(repository)] {
        return meeting_state::list_export_meetings(*repository);
    };
}

MeetingExportDialog::EntryLoader
meetingEntryLoader(std::shared_ptr<meeting_state::SqlMeetingRepository> repository) {
    return [repository = std::move(repository)](const std::string& meetingId) {
        return meeting_state::collect_meeting_export(*repository, meetingId);
    };
}

} // namespace fv
