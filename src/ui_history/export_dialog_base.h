#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QDialog>
#include <QMessageBox>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>

#include "base/json.h"
#include "pyport/datetime.h"

class QButtonGroup;
class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QParallelAnimationGroup;
class QPushButton;
class QRadioButton;
class QVBoxLayout;

namespace fv {

class AnimatedProgressBar;
class Button;
class ElidingComboBox;
class Executor;
class NoWheelDateEdit;
class WrappedLabel;

// The format values both export services accept (history::FORMAT_* and the meeting exporter's).
namespace export_format {
inline constexpr std::string_view markdown = "markdown";
inline constexpr std::string_view txt = "txt";
inline constexpr std::string_view json = "json";
} // namespace export_format

// What a concrete exporter calls things: the object names the theme styles (literal per
// subclass: "<prefix>Dialog", "<prefix>Card", ...), and its copy.
struct ExportDialogCopy {
    QString dialogName;
    QString cardName;
    QString eyebrowName;
    QString metaName;
    QString miniButtonName;
    QString paneName;
    QString listName;
    QString progressName;
    // Window title and heading; also titles the message boxes.
    QString title;
    QString subtitle;
    QString loadError;
    std::string loadLog;
    QString emptyText;
    QString scopeEyebrow;
    QString allLabel;
    QString selectedLabel;
    QString searchPlaceholder;
    // Singular and plural name of one exported item.
    QString noun;
    QString nounPlural;
    QString markdownHint;
    QString failedText;
    std::string failedLog;
    QString defaultDirName;
    QString defaultFileStem;
};

// ui_qt/dialogs/export_dialog_base.py ExportDialogBase: pick items, criteria and a format, then
// export them to disk. Subclasses give the copy and implement the hooks at the end of the class:
// the criteria checkbox, the include options, list labels, the default location, filtering, and
// how collected entries are written.
//
// Collection and writing run on io and never see the dialog: the functions they call are bound
// on the GUI thread as an export starts, and every report comes back through a session-lived
// relay with the export's token, so nothing touches widgets off the GUI thread and a deleted
// dialog is never held. Cancel is cooperative and checked between items; nothing is written until
// collection finishes. Closing or rejecting while busy cancels instead of closing.
//
// A subclass calls setup() at the end of its constructor (it calls the hooks).
// Affinity: GUI thread.
class ExportDialogBase : public QDialog {
    Q_OBJECT
public:
    static constexpr int kSelectedListHeight = 180;
    // An export item: a JSON object with at least an "id".
    using Items = std::vector<Json>;
    // Reads the items; what it throws is shown as the load error.
    using ItemProvider = std::function<Items()>;

    ~ExportDialogBase() override;

    const QString& title() const { return copy_.title; }
    const QString& nounPlural() const { return copy_.nounPlural; }
    const Items& items() const { return items_; }
    const QString& loadError() const { return loadError_; }
    // False for the empty and load-error states, which have only Close.
    bool hasControls() const { return exportButton_ != nullptr; }

    // The controls (null in the empty state).
    QFrame* scopeCard() const { return scopeCard_; }
    QFrame* contentCard() const { return contentCard_; }
    QFrame* outputCard() const { return outputCard_; }
    QRadioButton* allRadio() const { return allRadio_; }
    QRadioButton* selectedRadio() const { return selectedRadio_; }
    QWidget* filtersPanel() const { return filtersPanel_; }
    QWidget* selectedPanel() const { return selectedPanel_; }
    QCheckBox* dateRangeCheck() const { return dateRangeCheck_; }
    QWidget* dateRangeRow() const { return dateRangeRow_; }
    NoWheelDateEdit* fromDate() const { return fromDate_; }
    NoWheelDateEdit* toDate() const { return toDate_; }
    QLineEdit* selectedSearch() const { return selectedSearch_; }
    QListWidget* selectedList() const { return selectedList_; }
    QLabel* selectedCountLabel() const { return selectedCountLabel_; }
    ElidingComboBox* formatCombo() const { return formatCombo_; }
    QCheckBox* perItemCheck() const { return perItemCheck_; }
    const std::vector<QCheckBox*>& includeChecks() const { return includeChecks_; }
    WrappedLabel* markdownOnlyHint() const { return markdownOnlyHint_; }
    QLineEdit* pathEdit() const { return pathEdit_; }
    WrappedLabel* outputHint() const { return outputHint_; }
    AnimatedProgressBar* progressBar() const { return progressBar_; }
    WrappedLabel* progressLabel() const { return progressLabel_; }
    QPushButton* cancelWorkButton() const { return cancelWorkButton_; }
    QPushButton* closeButton() const { return closeButton_; }
    QPushButton* exportButton() const { return exportButton_; }
    QParallelAnimationGroup* animationGroup() const { return animGroup_; }

    // _resolve_targets: the checked items for the selected scope, else every item through the
    // criteria (the date range, inclusive, when on).
    Items resolveTargets() const;
    // _checked_ids, in list order.
    std::vector<std::string> checkedIds() const;
    // _set_all_checked: the visible (matching) rows only.
    void setAllChecked(bool checked);
    QString currentFormat() const;
    QString defaultPath() const;
    // _content_height: the height the dialog needs for the sections now visible.
    int contentHeight();
    void resizeToContent();
    void refreshExportEnabled();
    // Whether an export is running.
    bool isBusy() const { return busy_; }
    // Whether a cancel was asked of the running export.
    bool cancelRequested() const;
    // Marks the dialog busy (or not) as an export does; for tests that stand in for a worker. Does
    // nothing in the empty and load-error states.
    void setBusy(bool busy);
    // The token of the newest export.
    std::uint64_t exportToken() const { return exportToken_; }

    // How the dialog asks and tells; QFileDialog and QMessageBox by default, replaced by tests.
    std::function<QString(QWidget* parent, const QString& title, const QString& start)>
        chooseFolder;
    std::function<QString(QWidget* parent, const QString& title, const QString& start,
                          const QString& filter)>
        chooseSaveFile;
    std::function<void(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                       const QString& text)>
        showMessage;

public Q_SLOTS:
    void reject() override;
    // _on_export: starts the export on io; tells when nothing matches instead.
    void startExport();
    void cancelWork();
    void browse();

Q_SIGNALS:
    void progress(int done, int total, const QString& title);
    void exportFinished(int count, const QString& output);
    void exportFailed(const QString& message);
    void exportCanceled();

protected:
    ExportDialogBase(ExportDialogCopy copy, ItemProvider provider, Executor& io,
                     QWidget* parent = nullptr);
    // Loads the items and builds the dialog; the subclass constructor's last call.
    void setup();

    void closeEvent(QCloseEvent* event) override;

    // ---- hooks ----
    // The "only items with ..." checkbox under the date range.
    virtual QCheckBox* buildCriteriaCheck() = 0;
    // The Markdown-only "Include ..." checkboxes; they start checked.
    virtual std::vector<QCheckBox*> buildIncludeChecks() = 0;
    // The keyword arguments the include checkboxes pass to the writers, as a JSON object.
    virtual Json includeOptions() const = 0;
    virtual QString itemLabel(const Json& item) const = 0;
    // The folder the default export file or folder is placed in.
    virtual std::filesystem::path exportRoot() const = 0;
    virtual Items filterItems(Items items, const std::optional<py::DateTime>& from,
                              const std::optional<py::DateTime>& to) const = 0;

    // The functions below are returned on the GUI thread as an export starts and called on io:
    // they must not reach back into the dialog.
    // Item to export entry, or none to skip it.
    using Collector = std::function<std::optional<Json>(const Json& item)>;
    virtual Collector entryCollector() const = 0;
    using TitleOf = std::function<QString(const Json& item)>;
    virtual TitleOf progressTitle() const = 0;
    using WriteFiles = std::function<void(const Items& entries, const std::string& format,
                                          const std::string& output, const Json& options)>;
    virtual WriteFiles perItemWriter() const = 0;
    using Render = std::function<std::string(const Items& entries, const std::string& format,
                                             const Json& options)>;
    virtual Render documentRenderer() const = 0;

private:
    void loadItems();
    void setupUi();
    std::pair<QVBoxLayout*, QFrame*> card(const QString& eyebrow);
    QLabel* metaLabel(const QString& text);
    Button* miniButton(const QString& text);
    QFrame* buildScopeCard();
    QWidget* buildFiltersPanel();
    QWidget* buildSelectedPanel();
    QListWidgetItem* makeListItem(const Json& item);
    QFrame* buildContentCard();
    QFrame* buildOutputCard();
    QString singleFileHint() const;
    void buildProgressRow(QVBoxLayout* layout);
    void buildButtonRow(QVBoxLayout* layout);

    void onScopeChanged();
    void filterSelectedList(const QString& text);
    void onDateRangeToggled(bool checked);
    void onFormatChanged();
    void onPerItemToggled(bool checked);
    std::vector<QWidget*> sections() const;
    QRect targetGeometry(const QRect& origin, int height) const;
    static int naturalHeight(QWidget* widget);
    void animateSections(const std::vector<QWidget*>& show, const std::vector<QWidget*>& hide);
    void finishSections(const std::vector<QWidget*>& show, const std::vector<QWidget*>& hide);
    void refreshSelectedCount();
    void refreshDefaultPath();
    QStringList defaultPaths() const;

    void onProgress(int done, int total, const QString& title);
    void onExportFinished(int count, const QString& summary);
    void onExportFailed(const QString& message);
    void onExportCanceled();

    ExportDialogCopy copy_;
    ItemProvider provider_;
    Executor& io_;
    Items items_;
    QString loadError_;
    bool busy_ = false;
    std::uint64_t exportToken_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    QParallelAnimationGroup* animGroup_ = nullptr;
    // Sections an animation is taking to visible (true) or hidden (false), in the order asked:
    // a control that interrupts a transition keeps its unfinished sections in the new one.
    std::vector<std::pair<QPointer<QWidget>, bool>> sectionTargets_;

    QFrame* scopeCard_ = nullptr;
    QFrame* contentCard_ = nullptr;
    QFrame* outputCard_ = nullptr;
    QRadioButton* allRadio_ = nullptr;
    QRadioButton* selectedRadio_ = nullptr;
    QWidget* filtersPanel_ = nullptr;
    QWidget* selectedPanel_ = nullptr;
    QCheckBox* dateRangeCheck_ = nullptr;
    QWidget* dateRangeRow_ = nullptr;
    NoWheelDateEdit* fromDate_ = nullptr;
    NoWheelDateEdit* toDate_ = nullptr;
    QLineEdit* selectedSearch_ = nullptr;
    QListWidget* selectedList_ = nullptr;
    QLabel* selectedCountLabel_ = nullptr;
    ElidingComboBox* formatCombo_ = nullptr;
    QCheckBox* perItemCheck_ = nullptr;
    std::vector<QCheckBox*> includeChecks_;
    WrappedLabel* markdownOnlyHint_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    WrappedLabel* outputHint_ = nullptr;
    AnimatedProgressBar* progressBar_ = nullptr;
    WrappedLabel* progressLabel_ = nullptr;
    QPushButton* cancelWorkButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
};

namespace export_detail {
// _write_document: document written to output in Python's text mode (UTF-8, "\n" as the
// platform's line ending), its folder made first. Throws std::system_error on failure.
void write_document(const std::filesystem::path& output, std::string_view document);
} // namespace export_detail

} // namespace fv
