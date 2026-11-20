#include "ui_history/export_dialog_base.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDate>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/log.h"
#include "base/utf8.h"
#include "ui_history/detail/export_relay.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace export_detail {

ExportRelay& ExportRelay::instance() {
    // Never deleted: a worker may emit on it after every dialog, and the application, is gone.
    static ExportRelay* const relay = new ExportRelay;
    return *relay;
}

std::uint64_t ExportRelay::nextToken() {
    static std::atomic<std::uint64_t> tokens{0};
    return tokens.fetch_add(1) + 1;
}

void write_document(const std::filesystem::path& output, std::string_view document) {
    const std::filesystem::path parent = output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::string bytes;
    bytes.reserve(document.size() + document.size() / 16);
    for (const char c : document) {
#ifdef _WIN32
        // Python's text mode writes "\n" as the platform's line ending.
        if (c == '\n') {
            bytes += '\r';
        }
#endif
        bytes += c;
    }
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                std::format("Can't write '{}'", path_to_utf8(output)));
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        throw std::system_error(EIO, std::generic_category(),
                                std::format("Can't write '{}'", path_to_utf8(output)));
    }
}

} // namespace export_detail

namespace {

using export_detail::ExportRelay;

constexpr log::Logger kLog{"ui_qt.dialogs.export_dialog_base"};

// _FORMAT_LABELS and _FORMAT_EXTENSIONS, in the combo's order.
struct FormatInfo {
    std::string_view key;
    const char* label;
    const char* extension;
};
constexpr std::array<FormatInfo, 3> kFormats = {{
    {export_format::markdown, "Markdown", "md"},
    {export_format::txt, "Plain transcript", "txt"},
    {export_format::json, "JSON", "json"},
}};

const FormatInfo* formatInfo(const QString& key) {
    for (const FormatInfo& info : kFormats) {
        if (qt::toQString(info.key) == key) {
            return &info;
        }
    }
    return nullptr;
}

// str(item.get("id") or ""): an item's id as text, "" when it is missing, null, empty, zero or
// false. A meeting's id may be a number, so a number is its decimal text, not "".
std::string idOf(const Json& item) {
    const auto found = item.find("id");
    if (found == item.end() || found->is_null()) {
        return {};
    }
    if (found->is_string()) {
        return found->get<std::string>();
    }
    if (found->is_boolean()) {
        return found->get<bool>() ? "True" : "";
    }
    if (found->is_number()) {
        return found->get<double>() == 0.0 ? std::string() : found->dump();
    }
    return found->empty() ? std::string() : found->dump();
}

QString joinPath(const std::filesystem::path& folder, const QString& name) {
    return qt::toQString(path_to_utf8(folder / path_from_utf8(qt::toUtf8(name))));
}

// The (widget, visible) entries of targets, in their order, keeping only widgets still alive.
std::vector<QWidget*> targetsWith(const std::vector<std::pair<QPointer<QWidget>, bool>>& targets,
                                  bool visible) {
    std::vector<QWidget*> out;
    for (const auto& [widget, shown] : targets) {
        if (widget != nullptr && shown == visible) {
            out.push_back(widget);
        }
    }
    return out;
}

} // namespace

ExportDialogBase::ExportDialogBase(ExportDialogCopy copy, ItemProvider provider, Executor& io,
                                   QWidget* parent)
    : QDialog(parent), copy_(std::move(copy)), provider_(std::move(provider)), io_(io) {
    chooseFolder = [](QWidget* owner, const QString& caption, const QString& start) {
        return QFileDialog::getExistingDirectory(owner, caption, start);
    };
    chooseSaveFile = [](QWidget* owner, const QString& caption, const QString& start,
                        const QString& filter) {
        return QFileDialog::getSaveFileName(owner, caption, start, filter);
    };
    showMessage = [](QWidget* owner, QMessageBox::Icon icon, const QString& caption,
                     const QString& text) {
        if (icon == QMessageBox::Warning) {
            QMessageBox::warning(owner, caption, text);
        } else {
            QMessageBox::information(owner, caption, text);
        }
    };
    setMinimumWidth(600);
    setMaximumWidth(680);
    setModal(true);
    connect(this, &ExportDialogBase::progress, this, &ExportDialogBase::onProgress);
    connect(this, &ExportDialogBase::exportFinished, this, &ExportDialogBase::onExportFinished);
    connect(this, &ExportDialogBase::exportFailed, this, &ExportDialogBase::onExportFailed);
    connect(this, &ExportDialogBase::exportCanceled, this, &ExportDialogBase::onExportCanceled);
    // The relay serves every dialog; each passes on only its own export's reports, as its public
    // signals, on the GUI thread.
    ExportRelay& relay = ExportRelay::instance();
    connect(&relay, &ExportRelay::progress, this,
            [this](quint64 token, int done, int total, const QString& title) {
                if (token == exportToken_) {
                    Q_EMIT progress(done, total, title);
                }
            });
    connect(&relay, &ExportRelay::finished, this,
            [this](quint64 token, int count, const QString& output) {
                if (token == exportToken_) {
                    Q_EMIT exportFinished(count, output);
                }
            });
    connect(&relay, &ExportRelay::failed, this, [this](quint64 token, const QString& message) {
        if (token == exportToken_) {
            Q_EMIT exportFailed(message);
        }
    });
    connect(&relay, &ExportRelay::canceled, this, [this](quint64 token) {
        if (token == exportToken_) {
            Q_EMIT exportCanceled();
        }
    });
    setObjectName(copy_.dialogName);
    setWindowTitle(copy_.title);
}

// A running export carries on without the dialog: it holds only what startExport bound, and its
// reports reach no one.
ExportDialogBase::~ExportDialogBase() = default;

void ExportDialogBase::setup() {
    loadItems();
    setupUi();
}

void ExportDialogBase::loadItems() {
    try {
        items_ = provider_ ? provider_() : Items{};
        loadError_.clear();
    } catch (const std::exception& error) {
        kLog.error_with(error, copy_.loadLog);
        items_.clear();
        loadError_ = qt::toQString(std::string_view(error.what()));
    }
}

// ---- layout ----

void ExportDialogBase::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 20);
    layout->setSpacing(14);

    auto* heading = new QLabel(copy_.title);
    heading->setObjectName(QStringLiteral("modelManagerTitle"));
    layout->addWidget(heading);
    auto* subtitle = new WrappedLabel(copy_.subtitle);
    subtitle->setObjectName(QStringLiteral("modelManagerSubtitle"));
    layout->addWidget(subtitle);

    if (!loadError_.isEmpty() || items_.empty()) {
        const QString text = !loadError_.isEmpty()
                                 ? copy_.loadError + QStringLiteral(":\n") + loadError_
                                 : copy_.emptyText;
        auto* notice = new WrappedLabel(text);
        notice->setObjectName(QStringLiteral("infoLabel"));
        layout->addWidget(notice);
        layout->addStretch();
        auto* close = new Button(QStringLiteral("Close"));
        connect(close, &QPushButton::clicked, this, &ExportDialogBase::reject);
        auto* row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(close);
        layout->addLayout(row);
        return;
    }

    scopeCard_ = buildScopeCard();
    layout->addWidget(scopeCard_);
    contentCard_ = buildContentCard();
    layout->addWidget(contentCard_);
    outputCard_ = buildOutputCard();
    layout->addWidget(outputCard_);
    buildProgressRow(layout);
    buildButtonRow(layout);
    pathEdit_->setText(defaultPath());
    refreshExportEnabled();
}

std::pair<QVBoxLayout*, QFrame*> ExportDialogBase::card(const QString& eyebrow) {
    auto* frame = new QFrame(this);
    frame->setObjectName(copy_.cardName);
    auto* inner = new QVBoxLayout(frame);
    inner->setContentsMargins(16, 12, 16, 14);
    inner->setSpacing(10);
    auto* label = new QLabel(eyebrow);
    label->setObjectName(copy_.eyebrowName);
    inner->addWidget(label);
    return {inner, frame};
}

QLabel* ExportDialogBase::metaLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(copy_.metaName);
    return label;
}

Button* ExportDialogBase::miniButton(const QString& text) {
    auto* button = new Button(text);
    button->setObjectName(copy_.miniButtonName);
    return button;
}

QFrame* ExportDialogBase::buildScopeCard() {
    auto [inner, frame] = card(copy_.scopeEyebrow);

    auto* scopeRow = new QHBoxLayout;
    scopeRow->setSpacing(20);
    allRadio_ = new QRadioButton(copy_.allLabel);
    allRadio_->setChecked(true);
    selectedRadio_ = new QRadioButton(copy_.selectedLabel);
    auto* group = new QButtonGroup(this);
    group->addButton(allRadio_);
    group->addButton(selectedRadio_);
    connect(allRadio_, &QRadioButton::toggled, this, [this](bool) { onScopeChanged(); });
    scopeRow->addWidget(allRadio_);
    scopeRow->addWidget(selectedRadio_);
    scopeRow->addStretch();
    inner->addLayout(scopeRow);

    inner->addWidget(buildFiltersPanel());
    inner->addWidget(buildSelectedPanel());
    selectedPanel_->setVisible(false);
    return frame;
}

QWidget* ExportDialogBase::buildFiltersPanel() {
    filtersPanel_ = new QWidget;
    filtersPanel_->setObjectName(copy_.paneName);
    auto* panelLayout = new QVBoxLayout(filtersPanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(8);

    dateRangeCheck_ = new QCheckBox(QStringLiteral("Limit to a date range"));
    connect(dateRangeCheck_, &QCheckBox::toggled, this, &ExportDialogBase::onDateRangeToggled);
    panelLayout->addWidget(dateRangeCheck_);

    dateRangeRow_ = new QWidget;
    dateRangeRow_->setObjectName(copy_.paneName);
    auto* dateRow = new QHBoxLayout(dateRangeRow_);
    dateRow->setContentsMargins(28, 0, 0, 0);
    dateRow->setSpacing(8);
    QLabel* fromLabel = metaLabel(QStringLiteral("From"));
    fromDate_ = new NoWheelDateEdit;
    fromDate_->setDate(QDate::currentDate().addMonths(-1));
    QLabel* toLabel = metaLabel(QStringLiteral("To"));
    toDate_ = new NoWheelDateEdit;
    toDate_->setDate(QDate::currentDate());
    dateRow->addWidget(fromLabel);
    dateRow->addWidget(fromDate_, 1);
    dateRow->addWidget(toLabel);
    dateRow->addWidget(toDate_, 1);
    dateRangeRow_->setVisible(false);
    panelLayout->addWidget(dateRangeRow_);

    panelLayout->addWidget(buildCriteriaCheck());
    return filtersPanel_;
}

QWidget* ExportDialogBase::buildSelectedPanel() {
    selectedPanel_ = new QWidget;
    selectedPanel_->setObjectName(copy_.paneName);
    auto* panelLayout = new QVBoxLayout(selectedPanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(8);

    selectedSearch_ = new QLineEdit;
    selectedSearch_->setPlaceholderText(copy_.searchPlaceholder);
    selectedSearch_->setClearButtonEnabled(true);
    connect(selectedSearch_, &QLineEdit::textChanged, this, &ExportDialogBase::filterSelectedList);
    panelLayout->addWidget(selectedSearch_);

    selectedList_ = new QListWidget;
    selectedList_->setObjectName(copy_.listName);
    selectedList_->setMinimumHeight(kSelectedListHeight);
    selectedList_->setMaximumHeight(kSelectedListHeight);
    selectedList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    selectedList_->setTextElideMode(Qt::ElideRight);
    selectedList_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    selectedList_->setUniformItemSizes(true);
    for (const Json& item : items_) {
        selectedList_->addItem(makeListItem(item));
    }
    connect(selectedList_, &QListWidget::itemChanged, this,
            [this](QListWidgetItem*) { refreshExportEnabled(); });
    panelLayout->addWidget(selectedList_);

    auto* toggleRow = new QHBoxLayout;
    toggleRow->setSpacing(8);
    Button* selectAll = miniButton(QStringLiteral("Select all"));
    connect(selectAll, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    Button* clear = miniButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    toggleRow->addWidget(selectAll);
    toggleRow->addWidget(clear);
    toggleRow->addStretch();
    selectedCountLabel_ = metaLabel(QString());
    toggleRow->addWidget(selectedCountLabel_);
    panelLayout->addLayout(toggleRow);
    return selectedPanel_;
}

QListWidgetItem* ExportDialogBase::makeListItem(const Json& item) {
    const QString label = itemLabel(item);
    auto* made = new QListWidgetItem(label);
    made->setData(Qt::UserRole, qt::toQString(idOf(item)));
    made->setFlags(made->flags() | Qt::ItemIsUserCheckable);
    made->setCheckState(Qt::Unchecked);
    made->setToolTip(label);
    return made;
}

QFrame* ExportDialogBase::buildContentCard() {
    auto [inner, frame] = card(QStringLiteral("CONTENT"));
    auto* formatCaption = new QLabel(QStringLiteral("Format"));
    formatCaption->setObjectName(QStringLiteral("textModelFieldLabel"));
    formatCombo_ = new ElidingComboBox;
    formatCombo_->setMinimumHeight(36);
    for (const FormatInfo& info : kFormats) {
        formatCombo_->addItem(QString::fromUtf8(info.label), qt::toQString(info.key));
    }
    connect(formatCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { onFormatChanged(); });
    auto* field = new QVBoxLayout;
    field->setSpacing(5);
    field->addWidget(formatCaption);
    field->addWidget(formatCombo_);
    inner->addLayout(field);

    perItemCheck_ = new QCheckBox(QStringLiteral("One file per ") + copy_.noun);
    connect(perItemCheck_, &QCheckBox::toggled, this, &ExportDialogBase::onPerItemToggled);
    inner->addWidget(perItemCheck_);

    includeChecks_ = buildIncludeChecks();
    for (QCheckBox* check : includeChecks_) {
        check->setChecked(true);
        inner->addWidget(check);
    }

    markdownOnlyHint_ = new WrappedLabel(copy_.markdownHint);
    markdownOnlyHint_->setObjectName(QStringLiteral("infoLabel"));
    markdownOnlyHint_->setVisible(false);
    inner->addWidget(markdownOnlyHint_);
    return frame;
}

QFrame* ExportDialogBase::buildOutputCard() {
    auto [inner, frame] = card(QStringLiteral("OUTPUT"));
    auto* outputRow = new QHBoxLayout;
    outputRow->setSpacing(8);
    pathEdit_ = new QLineEdit;
    pathEdit_->setPlaceholderText(QStringLiteral("Choose where to save…"));
    pathEdit_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(pathEdit_, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshExportEnabled(); });
    Button* browseButton = miniButton(QStringLiteral("Browse…"));
    connect(browseButton, &QPushButton::clicked, this, &ExportDialogBase::browse);
    outputRow->addWidget(pathEdit_, 1);
    outputRow->addWidget(browseButton);
    inner->addLayout(outputRow);

    outputHint_ = new WrappedLabel(singleFileHint());
    outputHint_->setObjectName(QStringLiteral("infoLabel"));
    inner->addWidget(outputHint_);
    return frame;
}

QString ExportDialogBase::singleFileHint() const {
    return QStringLiteral("Everything is written to a single file. Turn on “One file per %1” to "
                          "fill a folder instead.")
        .arg(copy_.noun);
}

void ExportDialogBase::buildProgressRow(QVBoxLayout* layout) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    progressBar_ = new AnimatedProgressBar;
    progressBar_->reset();
    progressBar_->setVisible(false);
    progressLabel_ = new WrappedLabel(QString());
    progressLabel_->setObjectName(copy_.progressName);
    row->addWidget(progressBar_, 1);
    row->addWidget(progressLabel_, 1);
    layout->addLayout(row);
}

void ExportDialogBase::buildButtonRow(QVBoxLayout* layout) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addStretch();
    cancelWorkButton_ = new Button(QStringLiteral("Cancel"));
    connect(cancelWorkButton_, &QPushButton::clicked, this, &ExportDialogBase::cancelWork);
    cancelWorkButton_->setVisible(false);
    closeButton_ = new Button(QStringLiteral("Close"));
    connect(closeButton_, &QPushButton::clicked, this, &ExportDialogBase::reject);
    exportButton_ = new PrimaryButton(QStringLiteral("Export"));
    exportButton_->setDefault(true);
    connect(exportButton_, &QPushButton::clicked, this, &ExportDialogBase::startExport);
    row->addWidget(cancelWorkButton_);
    row->addWidget(closeButton_);
    row->addWidget(exportButton_);
    layout->addLayout(row);
}

// ---- selection and criteria ----

void ExportDialogBase::onScopeChanged() {
    const bool selected = selectedRadio_->isChecked();
    refreshExportEnabled();
    if (selected) {
        animateSections({selectedPanel_}, {filtersPanel_});
    } else {
        animateSections({filtersPanel_}, {selectedPanel_});
    }
}

void ExportDialogBase::filterSelectedList(const QString& text) {
    const QString query = text.trimmed().toLower();
    for (int row = 0; row < selectedList_->count(); ++row) {
        QListWidgetItem* item = selectedList_->item(row);
        item->setHidden(!query.isEmpty() && !item->text().toLower().contains(query));
    }
}

void ExportDialogBase::setAllChecked(bool checked) {
    const Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    for (int row = 0; row < selectedList_->count(); ++row) {
        QListWidgetItem* item = selectedList_->item(row);
        if (item->isHidden()) {
            continue;
        }
        item->setCheckState(state);
    }
}

void ExportDialogBase::onDateRangeToggled(bool checked) {
    if (checked) {
        animateSections({dateRangeRow_}, {});
    } else {
        animateSections({}, {dateRangeRow_});
    }
}

void ExportDialogBase::onFormatChanged() {
    const bool markdown = currentFormat() == qt::toQString(export_format::markdown);
    for (QCheckBox* check : includeChecks_) {
        check->setEnabled(markdown);
    }
    refreshDefaultPath();
    if (markdown) {
        animateSections({}, {markdownOnlyHint_});
    } else {
        animateSections({markdownOnlyHint_}, {});
    }
}

void ExportDialogBase::onPerItemToggled(bool checked) {
    outputHint_->setText(
        checked ? QStringLiteral("Each %1 becomes its own file inside the chosen folder.")
                      .arg(copy_.noun)
                : singleFileHint());
    refreshDefaultPath();
}

std::vector<QWidget*> ExportDialogBase::sections() const {
    std::vector<QWidget*> out;
    for (QWidget* widget :
         {static_cast<QWidget*>(dateRangeRow_), static_cast<QWidget*>(markdownOnlyHint_),
          filtersPanel_, selectedPanel_, static_cast<QWidget*>(scopeCard_),
          static_cast<QWidget*>(contentCard_), static_cast<QWidget*>(outputCard_)}) {
        if (widget != nullptr) {
            out.push_back(widget);
        }
    }
    return out;
}

ExportDialogBase::Items ExportDialogBase::resolveTargets() const {
    // Filters only accompany the "all" scope; an explicit selection is exported exactly as
    // checked.
    if (selectedRadio_->isChecked()) {
        const std::vector<std::string> checked = checkedIds();
        Items out;
        for (const Json& item : items_) {
            const std::string id = idOf(item);
            if (std::find(checked.begin(), checked.end(), id) != checked.end()) {
                out.push_back(item);
            }
        }
        return out;
    }
    std::optional<py::DateTime> from;
    std::optional<py::DateTime> to;
    if (dateRangeCheck_->isChecked()) {
        const QDate start = fromDate_->date();
        const QDate end = toDate_->date();
        // datetime.combine(day, time.min) and (day, time.max without microseconds).
        from = py::DateTime{start.year(), start.month(), start.day(), 0, 0, 0, 0, std::nullopt};
        to = py::DateTime{end.year(), end.month(), end.day(), 23, 59, 59, 0, std::nullopt};
    }
    return filterItems(items_, from, to);
}

// ---- sizing and animation ----

int ExportDialogBase::contentHeight() {
    // Measuring has to activate the layout, which would otherwise resize the window to the result
    // on the spot (visible as a one-frame jump ahead of an animation); SetNoConstraint keeps the
    // pass read-only.
    QLayout* outer = layout();
    const QLayout::SizeConstraint constraint = outer->sizeConstraint();
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    for (QWidget* widget : sections()) {
        if (QLayout* inner = widget->layout()) {
            inner->invalidate();
            inner->activate();
        }
        widget->updateGeometry();
        widget->adjustSize();
    }
    outer->invalidate();
    outer->activate();
    updateGeometry();
    const int height = sizeHint().height();
    outer->setSizeConstraint(constraint);
    return height;
}

void ExportDialogBase::resizeToContent() {
    resize(width(), contentHeight());
}

QRect ExportDialogBase::targetGeometry(const QRect& origin, int height) const {
    QScreen* screen = QApplication::screenAt(frameGeometry().center());
    if (screen == nullptr) {
        screen = QApplication::primaryScreen();
    }
    if (screen != nullptr) {
        height = std::min(height, screen->availableGeometry().height());
    }
    return QRect(origin.x(), origin.y(), origin.width(), height);
}

int ExportDialogBase::naturalHeight(QWidget* widget) {
    widget->setMaximumHeight(kUnlimitedHeight);
    if (QLayout* inner = widget->layout()) {
        inner->activate();
    }
    widget->adjustSize();
    return std::max(widget->sizeHint().height(), widget->minimumSizeHint().height());
}

void ExportDialogBase::animateSections(const std::vector<QWidget*>& showAsked,
                                       const std::vector<QWidget*>& hideAsked) {
    // A different control can interrupt a transition; keep its unfinished sections in the new
    // animation so none stay visible or height-clamped.
    auto target = [this](QWidget* widget, bool visible) {
        for (auto& [held, shown] : sectionTargets_) {
            if (held == widget) {
                shown = visible;
                return;
            }
        }
        sectionTargets_.emplace_back(widget, visible);
    };
    for (QWidget* widget : showAsked) {
        target(widget, true);
    }
    for (QWidget* widget : hideAsked) {
        target(widget, false);
    }
    const std::vector<QWidget*> show = targetsWith(sectionTargets_, true);
    const std::vector<QWidget*> hide = targetsWith(sectionTargets_, false);
    if (animGroup_ == nullptr) {
        animGroup_ = new QParallelAnimationGroup(this);
    }
    QParallelAnimationGroup* group = animGroup_;
    group->stop();
    group->clear();
    QObject::disconnect(group, &QAbstractAnimation::finished, nullptr, nullptr);

    if (!isVisible()) {
        for (QWidget* widget : hide) {
            widget->setVisible(false);
        }
        for (QWidget* widget : show) {
            widget->setVisible(true);
        }
        finishSections(show, hide);
        return;
    }

    // Interrupting a swap resumes from the height on screen rather than restarting from the
    // section's collapsed or natural extreme.
    const QRect origin = geometry();
    std::map<QWidget*, int> starts;
    for (const auto* list : {&show, &hide}) {
        for (QWidget* widget : *list) {
            starts[widget] = widget->isVisible() ? widget->height() : 0;
        }
    }
    for (QWidget* widget : hide) {
        widget->setVisible(false);
    }
    for (QWidget* widget : show) {
        widget->setVisible(true);
    }
    std::map<QWidget*, int> ends;
    for (QWidget* widget : show) {
        ends[widget] = naturalHeight(widget);
    }
    const int targetHeight = contentHeight();

    for (const auto* list : {&show, &hide}) {
        for (QWidget* widget : *list) {
            widget->setVisible(true);
            widget->setMinimumHeight(0);
            widget->setMaximumHeight(starts[widget]);
            QPropertyAnimation* animation = createMaxHeightAnimation(widget, group);
            animation->setStartValue(starts[widget]);
            const auto end = ends.find(widget);
            animation->setEndValue(end != ends.end() ? end->second : 0);
            group->addAnimation(animation);
        }
    }

    // The window follows the animation, not the layout's own sizing, until the sections reach
    // their final heights. The layout's minimum height is released too, or every frame of a
    // collapse clamps to the pre-swap minimum and the window only snaps down once it ends.
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    setMinimumHeight(0);

    auto* geometryAnimation = new QPropertyAnimation(this, "geometry", group);
    geometryAnimation->setDuration(kCollapseDurationMs);
    geometryAnimation->setEasingCurve(kCollapseEasing);
    geometryAnimation->setStartValue(origin);
    geometryAnimation->setEndValue(targetGeometry(origin, targetHeight));
    group->addAnimation(geometryAnimation);

    std::vector<QPointer<QWidget>> shown(show.begin(), show.end());
    std::vector<QPointer<QWidget>> hidden(hide.begin(), hide.end());
    connect(
        group, &QAbstractAnimation::finished, this,
        [this, shown, hidden] {
            std::vector<QWidget*> showing;
            std::vector<QWidget*> hiding;
            for (const QPointer<QWidget>& widget : shown) {
                if (widget != nullptr) {
                    showing.push_back(widget);
                }
            }
            for (const QPointer<QWidget>& widget : hidden) {
                if (widget != nullptr) {
                    hiding.push_back(widget);
                }
            }
            finishSections(showing, hiding);
        },
        Qt::SingleShotConnection);
    group->start();
}

void ExportDialogBase::finishSections(const std::vector<QWidget*>& show,
                                      const std::vector<QWidget*>& hide) {
    sectionTargets_.clear();
    for (QWidget* widget : hide) {
        widget->setVisible(false);
    }
    for (const auto* list : {&show, &hide}) {
        for (QWidget* widget : *list) {
            widget->setMaximumHeight(kUnlimitedHeight);
        }
    }
    layout()->setSizeConstraint(QLayout::SetDefaultConstraint);
    resizeToContent();
}

// ---- output path ----

void ExportDialogBase::refreshExportEnabled() {
    refreshSelectedCount();
    if (exportButton_ == nullptr) {
        return;
    }
    if (busy_) {
        exportButton_->setEnabled(false);
        return;
    }
    bool ready = !pathEdit_->text().trimmed().isEmpty();
    if (selectedRadio_->isChecked()) {
        ready = ready && !checkedIds().empty();
    }
    exportButton_->setEnabled(ready);
}

void ExportDialogBase::refreshSelectedCount() {
    if (selectedCountLabel_ == nullptr) {
        return;
    }
    selectedCountLabel_->setText(
        QStringLiteral("%1 of %2 selected").arg(checkedIds().size()).arg(selectedList_->count()));
}

void ExportDialogBase::refreshDefaultPath() {
    if (pathEdit_ == nullptr) {
        return;
    }
    const QString current = pathEdit_->text().trimmed();
    if (!current.isEmpty() && !defaultPaths().contains(current)) {
        return;
    }
    pathEdit_->setText(defaultPath());
}

QStringList ExportDialogBase::defaultPaths() const {
    const std::filesystem::path folder = exportRoot();
    QStringList paths = {joinPath(folder, copy_.defaultDirName)};
    for (const FormatInfo& info : kFormats) {
        paths << joinPath(folder, copy_.defaultFileStem + QStringLiteral(".") +
                                      QString::fromLatin1(info.extension));
    }
    return paths;
}

QString ExportDialogBase::defaultPath() const {
    const std::filesystem::path folder = exportRoot();
    if (perItemCheck_->isChecked()) {
        return joinPath(folder, copy_.defaultDirName);
    }
    const FormatInfo* info = formatInfo(currentFormat());
    return joinPath(folder, copy_.defaultFileStem + QStringLiteral(".") +
                                QString::fromLatin1(info != nullptr ? info->extension : "md"));
}

QString ExportDialogBase::currentFormat() const {
    const QString chosen = formatCombo_->currentData().toString();
    return chosen.isEmpty() ? qt::toQString(export_format::markdown) : chosen;
}

void ExportDialogBase::browse() {
    if (perItemCheck_->isChecked()) {
        const QString folder =
            chooseFolder(this, QStringLiteral("Choose Export Folder"), pathEdit_->text());
        if (!folder.isEmpty()) {
            pathEdit_->setText(folder);
        }
        return;
    }
    const FormatInfo* info = formatInfo(currentFormat());
    const QString extension = QString::fromLatin1(info != nullptr ? info->extension : "md");
    const QString label = QString::fromUtf8(info != nullptr ? info->label : "File");
    QString path = chooseSaveFile(this, copy_.title, defaultPath(),
                                  QStringLiteral("%1 (*.%2);;All Files (*)").arg(label, extension));
    if (path.isEmpty()) {
        return;
    }
    if (path_from_utf8(qt::toUtf8(path)).extension().empty()) {
        path += QStringLiteral(".") + extension;
    }
    pathEdit_->setText(path);
}

std::vector<std::string> ExportDialogBase::checkedIds() const {
    std::vector<std::string> ids;
    if (selectedList_ == nullptr) {
        return ids;
    }
    for (int row = 0; row < selectedList_->count(); ++row) {
        QListWidgetItem* item = selectedList_->item(row);
        if (item->checkState() != Qt::Checked) {
            continue;
        }
        const QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty()) {
            ids.push_back(qt::toUtf8(id));
        }
    }
    return ids;
}

// ---- export ----

void ExportDialogBase::startExport() {
    if (busy_) {
        return;
    }
    Items targets = resolveTargets();
    if (targets.empty()) {
        showMessage(this, QMessageBox::Information, copy_.title,
                    QStringLiteral("No %1 match the selected criteria.").arg(copy_.nounPlural));
        return;
    }
    const std::string format = qt::toUtf8(currentFormat());
    const bool perItem = perItemCheck_->isChecked();
    const QString output = pathEdit_->text().trimmed();
    const Json options = includeOptions();
    // Bound here: the worker must not reach back into the dialog.
    Collector collect = entryCollector();
    TitleOf titleOf = progressTitle();
    WriteFiles writeFiles = perItemWriter();
    Render render = documentRenderer();
    const std::string failedLog = copy_.failedLog;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> cancel = cancel_;
    exportToken_ = ExportRelay::nextToken();
    const std::uint64_t token = exportToken_;
    const int total = static_cast<int>(targets.size());
    setBusy(true);
    Q_EMIT progress(0, total, QString());

    io_.post([targets = std::move(targets), format, perItem, output, options, collect, titleOf,
              writeFiles, render, failedLog, cancel, token, total] {
        ExportRelay& relay = ExportRelay::instance();
        try {
            Items entries;
            for (int index = 0; index < total; ++index) {
                if (cancel->load()) {
                    Q_EMIT relay.canceled(token);
                    return;
                }
                const Json& item = targets[static_cast<std::size_t>(index)];
                if (std::optional<Json> entry = collect(item)) {
                    entries.push_back(std::move(*entry));
                }
                Q_EMIT relay.progress(token, index + 1, total, titleOf(item));
            }
            if (cancel->load()) {
                Q_EMIT relay.canceled(token);
                return;
            }
            const std::string target = qt::toUtf8(output);
            if (perItem) {
                writeFiles(entries, format, target, options);
            } else {
                export_detail::write_document(path_from_utf8(target),
                                              render(entries, format, options));
            }
            Q_EMIT relay.finished(token, static_cast<int>(entries.size()), output);
        } catch (const std::exception& error) {
            kLog.error_with(error, failedLog);
            Q_EMIT relay.failed(token, qt::toQString(std::string_view(error.what())));
        }
    });
}

bool ExportDialogBase::cancelRequested() const {
    return cancel_->load();
}

void ExportDialogBase::setBusy(bool busy) {
    if (exportButton_ == nullptr) {
        // The empty and load-error states export nothing.
        return;
    }
    busy_ = busy;
    exportButton_->setVisible(!busy);
    closeButton_->setVisible(!busy);
    cancelWorkButton_->setVisible(busy);
    cancelWorkButton_->setEnabled(true);
    progressBar_->setVisible(busy);
    if (!busy) {
        progressLabel_->setText(QString());
        progressBar_->reset();
    }
    refreshExportEnabled();
    resizeToContent();
}

void ExportDialogBase::onProgress(int done, int total, const QString& title) {
    if (total <= 0) {
        progressBar_->setIndeterminate();
    } else {
        progressBar_->setFraction(static_cast<double>(done) / total);
    }
    QString label = QStringLiteral("Exporting %1/%2").arg(done).arg(total);
    if (!title.isEmpty()) {
        label += QStringLiteral(" — ") + title;
    }
    progressLabel_->setText(label);
}

void ExportDialogBase::onExportFinished(int count, const QString& summary) {
    setBusy(false);
    const QString& noun = count == 1 ? copy_.noun : copy_.nounPlural;
    showMessage(this, QMessageBox::Information, copy_.title,
                QStringLiteral("Exported %1 %2 to:\n%3").arg(count).arg(noun, summary));
}

void ExportDialogBase::onExportFailed(const QString& message) {
    setBusy(false);
    showMessage(this, QMessageBox::Warning, QStringLiteral("Export Failed"),
                copy_.failedText + QStringLiteral(":\n") + message);
}

void ExportDialogBase::onExportCanceled() {
    setBusy(false);
    progressLabel_->setText(QStringLiteral("Export canceled — nothing was written"));
}

void ExportDialogBase::cancelWork() {
    cancel_->store(true);
    cancelWorkButton_->setEnabled(false);
    progressLabel_->setText(QStringLiteral("Canceling…"));
}

void ExportDialogBase::reject() {
    if (busy_) {
        cancelWork();
        return;
    }
    QDialog::reject();
}

void ExportDialogBase::closeEvent(QCloseEvent* event) {
    if (busy_) {
        cancelWork();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

} // namespace fv
