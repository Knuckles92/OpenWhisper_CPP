#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/clock.h"
#include "meeting_state/export_bulk.h"
#include "meeting_state/sql_repository.h"
#include "ui_history/export_dialog_base.h"

class QCheckBox;

namespace fv {

// ui_qt/dialogs/meeting_export_dialog.py MeetingExportDialog: pick past meetings, criteria and a
// format, then export them to disk (objectName meetingExportDialog). Which meetings and what
// each document holds are chosen here; the documents are meeting_state/export_bulk.h's. The
// default file is "<product in lower case>_meetings.<ext>" and the default folder
// "meetings_export", both beside the meetings folder.
//
// Items are past meetings (a row with content_summary), read by the provider at construction:
// off the GUI thread, then handed in. The entry loader reads one meeting for the export on io,
// once per meeting.
// Affinity: GUI thread.
class MeetingExportDialog : public ExportDialogBase {
    Q_OBJECT
public:
    // The inputs of one meeting's document, or nullopt once the meeting is gone (deleted while
    // an export was being worked through).
    using EntryLoader =
        std::function<std::optional<meeting_state::MeetingExport>(const std::string& meetingId)>;

    MeetingExportDialog(ItemProvider provider, std::filesystem::path meetingsFolder,
                        EntryLoader entryLoader, Executor& io, QWidget* parent = nullptr,
                        const Clock& clock = system_clock());

    QCheckBox* transcriptOnlyCheck() const { return transcriptOnlyCheck_; }
    QCheckBox* includeTranscriptCheck() const { return includeTranscriptCheck_; }
    QCheckBox* includeIntelligenceCheck() const { return includeIntelligenceCheck_; }

    // The writers an export calls on io (render_export_document and write_per_meeting_files),
    // replaceable before an export starts. options is {"include_transcript": bool,
    // "include_intelligence": bool}; entries are entryToJson objects.
    std::function<std::string(const Items& entries, const std::string& format, const Json& options)>
        renderDocument;
    std::function<void(const Items& entries, const std::string& format, const std::string& output,
                       const Json& options)>
        writePerMeetingFiles;

    // An export entry as the dialog passes it between its steps, and back.
    static Json entryToJson(const meeting_state::MeetingExport& entry);
    static meeting_state::MeetingExport entryFromJson(const Json& entry);

protected:
    QCheckBox* buildCriteriaCheck() override;
    std::vector<QCheckBox*> buildIncludeChecks() override;
    Json includeOptions() const override;
    QString itemLabel(const Json& meeting) const override;
    std::filesystem::path exportRoot() const override;
    Items filterItems(Items items, const std::optional<py::DateTime>& from,
                      const std::optional<py::DateTime>& to) const override;
    Collector entryCollector() const override;
    TitleOf progressTitle() const override;
    WriteFiles perItemWriter() const override;
    Render documentRenderer() const override;

private:
    std::filesystem::path meetingsFolder_;
    EntryLoader entryLoader_;
    const Clock* clock_;
    QCheckBox* transcriptOnlyCheck_ = nullptr;
    QCheckBox* includeTranscriptCheck_ = nullptr;
    QCheckBox* includeIntelligenceCheck_ = nullptr;
};

// The default provider of Python's dialog: list_export_meetings over the repository. Blocks on
// the database: call it off the GUI thread.
MeetingExportDialog::ItemProvider
meetingExportProvider(std::shared_ptr<meeting_state::SqlMeetingRepository> repository);
// The default entry loader: collect_meeting_export over the repository.
MeetingExportDialog::EntryLoader
meetingEntryLoader(std::shared_ptr<meeting_state::SqlMeetingRepository> repository);

} // namespace fv
