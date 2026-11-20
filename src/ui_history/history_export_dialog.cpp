#include "ui_history/history_export_dialog.h"

#include <utility>

#include <QCheckBox>

#include "appkit/qt_bridge.h"
#include "base/utf8.h"
#include "history/export.h"

namespace fv {
namespace {

// What Python's `entry.get(key) or fallback` gives for a JSON export entry.
QString textOr(const Json& entry, const char* key, const QString& fallback) {
    const auto found = entry.find(key);
    if (found == entry.end() || found->is_null()) {
        return fallback;
    }
    if (found->is_string()) {
        const std::string& text = found->get_ref<const std::string&>();
        return text.empty() ? fallback : qt::toQString(text);
    }
    if (found->is_boolean()) {
        return found->get<bool>() ? QStringLiteral("True") : fallback;
    }
    if (found->is_number()) {
        return found->get<double>() == 0.0 ? fallback : qt::toQString(found->dump());
    }
    return found->empty() ? fallback : qt::toQString(found->dump());
}

history::ExportOptions exportOptions(const Json& options) {
    history::ExportOptions made;
    made.include_cleaned = options.value("include_cleaned", true);
    made.include_raw = options.value("include_raw", true);
    return made;
}

ExportDialogCopy historyCopy() {
    ExportDialogCopy copy;
    copy.dialogName = QStringLiteral("historyExportDialog");
    copy.cardName = QStringLiteral("historyExportCard");
    copy.eyebrowName = QStringLiteral("historyExportEyebrow");
    copy.metaName = QStringLiteral("historyExportMeta");
    copy.miniButtonName = QStringLiteral("historyExportMiniBtn");
    copy.paneName = QStringLiteral("historyExportPane");
    copy.listName = QStringLiteral("historyExportList");
    copy.progressName = QStringLiteral("historyExportProgress");
    copy.title = QStringLiteral("Export History");
    copy.subtitle = QStringLiteral("Choose which transcriptions to export, what each document "
                                   "includes, and where the files are written.");
    copy.loadError = QStringLiteral("History could not be loaded");
    copy.loadLog = "Could not load transcription history for export";
    copy.emptyText = QStringLiteral("No transcriptions to export yet.");
    copy.scopeEyebrow = QStringLiteral("TRANSCRIPTIONS");
    copy.allLabel = QStringLiteral("All history");
    copy.selectedLabel = QStringLiteral("Selected transcriptions");
    copy.searchPlaceholder = QStringLiteral("Search transcriptions…");
    copy.noun = QStringLiteral("transcription");
    copy.nounPlural = QStringLiteral("transcriptions");
    copy.markdownHint = QStringLiteral("Cleaned and raw sections apply to Markdown exports; other "
                                       "formats always include everything available.");
    copy.failedText = QStringLiteral("Could not export history");
    copy.failedLog = "History export failed";
    copy.defaultDirName = QStringLiteral("history_export");
    copy.defaultFileStem = qt::toQString(history::default_export_file_stem());
    return copy;
}

} // namespace

HistoryExportDialog::HistoryExportDialog(ItemProvider provider,
                                         std::filesystem::path recordingsFolder, Executor& io,
                                         QWidget* parent, const Clock& clock)
    : ExportDialogBase(historyCopy(), std::move(provider), io, parent),
      recordingsFolder_(std::move(recordingsFolder)), clock_(&clock) {
    const Clock* bound = clock_;
    renderDocument = [bound](const Items& entries, const std::string& format, const Json& options) {
        return history::render_export_document(entries, format, exportOptions(options), *bound);
    };
    writePerEntryFiles = [bound](const Items& entries, const std::string& format,
                                 const std::string& output, const Json& options) {
        history::write_per_entry_files(entries, format, path_from_utf8(output),
                                       exportOptions(options), *bound);
    };
    setup();
}

QCheckBox* HistoryExportDialog::buildCriteriaCheck() {
    audioOnlyCheck_ = new QCheckBox(QStringLiteral("Only entries with saved audio"));
    return audioOnlyCheck_;
}

std::vector<QCheckBox*> HistoryExportDialog::buildIncludeChecks() {
    includeCleanedCheck_ = new QCheckBox(QStringLiteral("Include cleaned transcript"));
    includeRawCheck_ = new QCheckBox(QStringLiteral("Include raw transcript"));
    return {includeCleanedCheck_, includeRawCheck_};
}

Json HistoryExportDialog::includeOptions() const {
    Json options = Json::object();
    options["include_cleaned"] = includeCleanedCheck_->isChecked();
    options["include_raw"] = includeRawCheck_->isChecked();
    return options;
}

QString HistoryExportDialog::itemLabel(const Json& entry) const {
    return textOr(entry, "formatted_timestamp", QStringLiteral("unknown date")) +
           QStringLiteral(" — ") +
           textOr(entry, "preview_text", QStringLiteral("Empty transcript"));
}

std::filesystem::path HistoryExportDialog::exportRoot() const {
    // os.path.dirname(os.path.abspath(RECORDINGS_FOLDER)).
    std::filesystem::path folder = std::filesystem::absolute(recordingsFolder_).lexically_normal();
    if (!folder.has_filename()) {
        folder = folder.parent_path();
    }
    return folder.parent_path();
}

ExportDialogBase::Items
HistoryExportDialog::filterItems(Items items, const std::optional<py::DateTime>& from,
                                 const std::optional<py::DateTime>& to) const {
    return history::filter_export_entries(items, from, to, audioOnlyCheck_->isChecked(), *clock_);
}

ExportDialogBase::Collector HistoryExportDialog::entryCollector() const {
    return [](const Json& entry) {
        return std::optional<Json>(entry);
    };
}

ExportDialogBase::TitleOf HistoryExportDialog::progressTitle() const {
    return [](const Json& entry) {
        return textOr(entry, "preview_text", QString());
    };
}

ExportDialogBase::WriteFiles HistoryExportDialog::perItemWriter() const {
    return writePerEntryFiles;
}

ExportDialogBase::Render HistoryExportDialog::documentRenderer() const {
    return renderDocument;
}

} // namespace fv
