#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "base/clock.h"
#include "ui_history/export_dialog_base.h"

class QCheckBox;

namespace fv {

// ui_qt/dialogs/history_export_dialog.py HistoryExportDialog: pick History entries, criteria and a
// format, then export them to disk. Scope and criteria are picked here; the documents are
// history/export.h's. Items are export entries (history::serialize_history_entry), read by the
// provider at construction: list_export_entries off the GUI thread, then handed in, since the
// GUI thread never blocks on the database. The default file and folder go beside the recordings
// folder (the data root).
// Affinity: GUI thread.
class HistoryExportDialog : public ExportDialogBase {
    Q_OBJECT
public:
    HistoryExportDialog(ItemProvider provider, std::filesystem::path recordingsFolder, Executor& io,
                        QWidget* parent = nullptr, const Clock& clock = system_clock());

    QCheckBox* audioOnlyCheck() const { return audioOnlyCheck_; }
    QCheckBox* includeCleanedCheck() const { return includeCleanedCheck_; }
    QCheckBox* includeRawCheck() const { return includeRawCheck_; }

    // The writers an export calls on io (history::render_export_document and
    // write_per_entry_files), replaceable before an export starts. options is
    // {"include_cleaned": bool, "include_raw": bool}.
    std::function<std::string(const Items& entries, const std::string& format, const Json& options)>
        renderDocument;
    std::function<void(const Items& entries, const std::string& format, const std::string& output,
                       const Json& options)>
        writePerEntryFiles;

protected:
    QCheckBox* buildCriteriaCheck() override;
    std::vector<QCheckBox*> buildIncludeChecks() override;
    Json includeOptions() const override;
    QString itemLabel(const Json& entry) const override;
    std::filesystem::path exportRoot() const override;
    Items filterItems(Items items, const std::optional<py::DateTime>& from,
                      const std::optional<py::DateTime>& to) const override;
    Collector entryCollector() const override;
    TitleOf progressTitle() const override;
    WriteFiles perItemWriter() const override;
    Render documentRenderer() const override;

private:
    std::filesystem::path recordingsFolder_;
    const Clock* clock_;
    QCheckBox* audioOnlyCheck_ = nullptr;
    QCheckBox* includeCleanedCheck_ = nullptr;
    QCheckBox* includeRawCheck_ = nullptr;
};

} // namespace fv
