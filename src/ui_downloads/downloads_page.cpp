#include "ui_downloads/downloads_page.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/log.h"
#include "models/consent_text.h"
#include "models/hf_access_policy.h"
#include "models/model_details.h"
#include "models/speech_catalog.h"
#include "settings/store.h"
#include "ui_downloads/batch_download_dialog.h"
#include "ui_downloads/component_details_dialog.h"
#include "ui_downloads/component_row_widget.h"
#include "ui_downloads/model_row_widget.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/file_reveal.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/fuzzy_match.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_downloads"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string u(const QString& text) {
    return qt::toUtf8(text);
}

// The bundled profile of a catalog model; null for a name the catalog has not.
const models::ModelDetails* detailsOf(const QString& modelName) {
    return models::find_model_details(u(modelName));
}

} // namespace

// What a scan worker may touch: never the page itself, which it reaches only through deliver().
struct DownloadsPage::Mailbox {
    std::mutex mutex;
    DownloadsPage* page = nullptr;
    qt::TokenGate gate;

    // From the io executor: runs fn(page) on the GUI thread if the page still lives and token is
    // still the newest scan when it runs.
    template <class Fn> void deliver(std::uint64_t token, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (page == nullptr) {
            return;
        }
        gate.deliver(page, token,
                     [target = page, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }
};

// _RowContext: what each row's refresh needs, gathered once per refresh.
struct DownloadsPage::RowContext {
    std::optional<std::string> loadedModel;
    std::string dictationModel;
    std::string meetingModel;
    bool downloadsBlocked = false;
    bool slotBusy = false;
    bool selectionLocked = false;
};

DownloadsPage::DownloadsPage(SettingsStore& settings, Executor& io, DownloadsPageOptions options,
                             bool deferBuild, QWidget* parent)
    : QWidget(parent), runMessageBox([](QMessageBox& box) { return box.exec(); }),
      runDialog([](QDialog& dialog) { return dialog.exec(); }),
      openUrl([](const QUrl& url) { QDesktopServices::openUrl(url); }), settings_(settings),
      io_(io), options_(std::move(options)), mailbox_(std::make_shared<Mailbox>()) {
    mailbox_->page = this;
    setObjectName(QStringLiteral("downloadsPage"));
    if (!deferBuild) {
        ensureUi();
    }
}

DownloadsPage::~DownloadsPage() {
    std::lock_guard lock(mailbox_->mutex);
    mailbox_->page = nullptr;
}

void DownloadsPage::ensureUi() {
    if (uiBuilt_) {
        return;
    }
    uiBuilt_ = true;
    setupUi();
    applyInventory(inventory_);
}

// ---- construction ----

void DownloadsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Models and components are different jobs: one-line tabs, since the model list needs the
    // height.
    tabs_ = new SegmentedBar(
        {{QStringLiteral("Speech models"), QString()}, {QStringLiteral("Components"), QString()}},
        true);
    tabs_->setAccessibleName(QStringLiteral("Downloads section"));
    connect(tabs_, &SegmentedBar::currentIndexChanged, this, &DownloadsPage::showSection);
    layout->addWidget(tabs_);

    modelsSection_ = new QWidget();
    modelsSection_->setObjectName(QStringLiteral("downloadsModelsSection"));
    auto* modelsLayout = new QVBoxLayout(modelsSection_);
    modelsLayout->setContentsMargins(0, 0, 0, 0);
    modelsLayout->setSpacing(6);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(8);
    // Elides rather than wraps: the folder path is long and would push the list down two lines.
    statsLabel_ = new ElidingLabel(QString());
    statsLabel_->setObjectName(QStringLiteral("downloadsSubtitle"));
    headerRow->addWidget(statsLabel_, 1);

    downloadAllButton_ = new Button(QStringLiteral("Download all…"));
    downloadAllButton_->setObjectName(QStringLiteral("downloadsToolButton"));
    fitCompactButton(downloadAllButton_, 0);
    downloadAllButton_->setToolTip(QStringLiteral("Queue every model that is not downloaded yet"));
    connect(downloadAllButton_, &QPushButton::clicked, this, [this] {
        const std::vector<std::string> names = missingNames();
        if (!names.empty()) {
            requestBatch(names);
        }
    });
    headerRow->addWidget(downloadAllButton_);

    openFolderButton_ = new Button(QStringLiteral("Open folder"));
    openFolderButton_->setObjectName(QStringLiteral("downloadsToolButton"));
    fitCompactButton(openFolderButton_, 110);
    openFolderButton_->setToolTip(
        QStringLiteral("Open the folder where downloaded models are stored"));
    connect(openFolderButton_, &QPushButton::clicked, this, &DownloadsPage::openCacheFolder);
    headerRow->addWidget(openFolderButton_);
    modelsLayout->addLayout(headerRow);

    // Settings places the download policy here, next to the catalog it governs.
    policyRow_ = new QHBoxLayout();
    policyRow_->setContentsMargins(0, 0, 0, 0);
    policyRow_->setSpacing(10);
    modelsLayout->addLayout(policyRow_);

    envBanner_ = new QLabel(QStringLiteral("Downloads are disabled by the HF_HUB_OFFLINE "
                                           "environment variable set outside this application."));
    envBanner_->setObjectName(QStringLiteral("downloadsEnvBanner"));
    envBanner_->setWordWrap(true);
    envBanner_->setVisible(false);
    modelsLayout->addWidget(envBanner_);

    split_ = new QHBoxLayout();
    split_->setSpacing(12);
    catalogColumn_ = buildCatalogColumn();
    split_->addWidget(catalogColumn_, 1);
    inspector_ = buildInspector();
    split_->addWidget(inspector_);
    modelsLayout->addLayout(split_, 1);
    layout->addWidget(modelsSection_, 1);

    componentsSection_ = buildComponentsSection();
    componentsSection_->hide();
    layout->addWidget(componentsSection_, 1);
    // A platform with nothing installable has nothing to switch to.
    tabs_->setVisible(!componentRows_.empty());

    // Shared by both tabs: batch progress and component install results.
    auto* messageRow = new QHBoxLayout();
    messageRow->setSpacing(8);
    messageLabel_ = new QLabel(message_);
    messageLabel_->setObjectName(QStringLiteral("downloadsMessage"));
    messageRow->addWidget(messageLabel_, 1);
    stopBatchButton_ = new Button(QStringLiteral("Stop after current"));
    stopBatchButton_->setObjectName(QStringLiteral("downloadsToolButton"));
    fitCompactButton(stopBatchButton_, 0);
    stopBatchButton_->setToolTip(
        QStringLiteral("Finish the model now downloading, then stop the queue"));
    connect(stopBatchButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT batchCancelRequested(); });
    stopBatchButton_->setVisible(false);
    messageRow->addWidget(stopBatchButton_);
    layout->addLayout(messageRow);
}

void DownloadsPage::addPolicyControl(const QString& label, QWidget* control) {
    ensureUi();
    auto* caption = new QLabel(label);
    caption->setObjectName(QStringLiteral("downloadsPolicyLabel"));
    policyRow_->addWidget(caption);
    policyRow_->addWidget(control, 1);
    policyRow_->addStretch();
}

QWidget* DownloadsPage::buildCatalogColumn() {
    auto* column = new QWidget();
    column->setObjectName(QStringLiteral("downloadsCatalogColumn"));
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(8);

    toolbarLayout_ = new QHBoxLayout();
    toolbarLayout_->setContentsMargins(0, 0, 0, 0);
    toolbarLayout_->setSpacing(8);
    filterEdit_ = new QLineEdit();
    filterEdit_->setObjectName(QStringLiteral("modelManagerSearch"));
    filterEdit_->setPlaceholderText(QStringLiteral("Search models"));
    filterEdit_->setClearButtonEnabled(true);
    connect(filterEdit_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    toolbarLayout_->addWidget(filterEdit_, 1);

    // Three eliding combos share the row with the search box, so a wide item never squeezes the
    // search field; the popups still show every item in full. One entry per speech backend, so
    // "all the Parakeet models" is one click.
    backendFilterCombo_ = new ElidingComboBox();
    backendFilterCombo_->setObjectName(QStringLiteral("modelManagerBackendFilter"));
    for (const std::string& value : models::backend_filter_values()) {
        if (value == "all") {
            backendFilterCombo_->addItem(QStringLiteral("All backends"), QStringLiteral("all"));
            continue;
        }
        const models::SpeechBackend* backend = models::backend(value);
        const QString label = backend != nullptr ? q(backend->label) : q(value);
        backendFilterCombo_->addItem(label, q(value));
        // backend_picker_tooltip: the label itself off Apple Silicon.
        backendFilterCombo_->setItemData(backendFilterCombo_->count() - 1, label, Qt::ToolTipRole);
    }
    backendFilterCombo_->setToolTip(QStringLiteral("Show one speech backend's models"));
    connect(backendFilterCombo_, &QComboBox::currentIndexChanged, this, [this] { applyFilter(); });
    toolbarLayout_->addWidget(backendFilterCombo_);

    statusFilterCombo_ = new ElidingComboBox();
    statusFilterCombo_->setObjectName(QStringLiteral("modelManagerStatusFilter"));
    statusFilterCombo_->addItem(QStringLiteral("All"), QStringLiteral("all"));
    statusFilterCombo_->addItem(QStringLiteral("Downloaded"), QStringLiteral("downloaded"));
    statusFilterCombo_->addItem(QStringLiteral("Not downloaded"), QStringLiteral("not_downloaded"));
    statusFilterCombo_->setToolTip(QStringLiteral("Filter by download status"));
    connect(statusFilterCombo_, &QComboBox::currentIndexChanged, this, [this] { applyFilter(); });
    toolbarLayout_->addWidget(statusFilterCombo_);

    sortCombo_ = new ElidingComboBox();
    sortCombo_->setObjectName(QStringLiteral("modelManagerSort"));
    sortCombo_->addItem(QStringLiteral("Recommended"), q(models::sort_mode::kRecommended));
    sortCombo_->addItem(QStringLiteral("Downloaded first"), q(models::sort_mode::kDownloaded));
    sortCombo_->addItem(QStringLiteral("Smallest first"), q(models::sort_mode::kSize));
    sortCombo_->addItem(QStringLiteral("Name A-Z"), q(models::sort_mode::kName));
    sortCombo_->addItem(QStringLiteral("By backend"), q(models::sort_mode::kBackend));
    sortCombo_->setToolTip(QStringLiteral("Sort model list"));
    connect(sortCombo_, &QComboBox::currentIndexChanged, this, [this] { applyFilter(); });
    toolbarLayout_->addWidget(sortCombo_);
    columnLayout->addLayout(toolbarLayout_);
    columnLayout->addWidget(buildSelectionBar());
    columnLayout->addWidget(buildList(), 1);
    return column;
}

QWidget* DownloadsPage::buildSelectionBar() {
    auto* bar = new QWidget();
    bar->setObjectName(QStringLiteral("downloadsSelectionBar"));
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(10);

    selectAllButton_ = new Button(QStringLiteral("Select all"));
    selectAllButton_->setObjectName(QStringLiteral("downloadsFlatButton"));
    fitCompactButton(selectAllButton_, 0);
    connect(selectAllButton_, &QPushButton::clicked, this, &DownloadsPage::selectAllMissing);
    barLayout->addWidget(selectAllButton_);

    clearSelectionButton_ = new Button(QStringLiteral("Clear"));
    clearSelectionButton_->setObjectName(QStringLiteral("downloadsFlatButton"));
    fitCompactButton(clearSelectionButton_, 0);
    connect(clearSelectionButton_, &QPushButton::clicked, this, &DownloadsPage::clearSelection);
    barLayout->addWidget(clearSelectionButton_);

    selectionSummary_ = new ElidingLabel(QString());
    selectionSummary_->setObjectName(QStringLiteral("downloadsSelectionSummary"));
    barLayout->addWidget(selectionSummary_, 1);

    downloadSelectedButton_ = new PrimaryButton(QStringLiteral("Download selected"));
    fitCompactButton(downloadSelectedButton_, 0);
    connect(downloadSelectedButton_, &QPushButton::clicked, this, [this] {
        const std::vector<std::string> names = selectedNames();
        if (!names.empty()) {
            requestBatch(names);
        }
    });
    barLayout->addWidget(downloadSelectedButton_);
    return bar;
}

QWidget* DownloadsPage::buildList() {
    libraryScrollArea_ = new QScrollArea();
    libraryScrollArea_->setObjectName(QStringLiteral("modelManagerLibraryScroll"));
    libraryScrollArea_->setWidgetResizable(true);
    // Low floor: at Settings' smallest size the filters, the batch bar and the tabs already fill
    // most of the page, and the list scrolls anyway.
    libraryScrollArea_->setMinimumHeight(40);
    libraryScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    libraryScrollArea_->setFrameShape(QFrame::NoFrame);

    auto* listContainer = new QWidget();
    listContainer->setObjectName(QStringLiteral("downloadsListContainer"));
    listLayout_ = new QVBoxLayout(listContainer);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(6);

    for (const std::string& name : models::model_row_names()) {
        auto* row = new ModelRowWidget(q(name));
        connect(row, &ModelRowWidget::downloadClicked, this, &DownloadsPage::onDownloadClicked);
        connect(row, &ModelRowWidget::deleteClicked, this, &DownloadsPage::onDeleteClicked);
        connect(row, &DownloadRow::detailsRequested, this, &DownloadsPage::selectModel);
        connect(row, &ModelRowWidget::selectionToggled, this, &DownloadsPage::onSelectionToggled);
        rows_.emplace_back(q(name), row);
        listLayout_->addWidget(row);
    }

    emptyLabel_ = new QLabel(QStringLiteral("No models match"));
    emptyLabel_->setObjectName(QStringLiteral("infoLabel"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setVisible(false);
    listLayout_->addWidget(emptyLabel_);
    listLayout_->addStretch();

    libraryScrollArea_->setWidget(listContainer);
    // The bar's own show and hide realign the filters the moment a result stops needing it. The
    // bar is shown before the viewport makes room for it, so the viewport's resize realigns them
    // again once it has.
    libraryScrollArea_->verticalScrollBar()->installEventFilter(this);
    libraryScrollArea_->viewport()->installEventFilter(this);
    return libraryScrollArea_;
}

// Keeps the filters and the rows they filter on one right edge. The gap between the scroll area
// and its viewport is the scroll bar's real width, zero while it is hidden.
void DownloadsPage::syncToolbarGutter() {
    const int reserved =
        std::max(0, libraryScrollArea_->width() - libraryScrollArea_->viewport()->width());
    if (toolbarLayout_->contentsMargins().right() != reserved) {
        toolbarLayout_->setContentsMargins(0, 0, reserved, 0);
    }
}

bool DownloadsPage::eventFilter(QObject* watched, QEvent* event) {
    if (uiBuilt_ && watched == libraryScrollArea_->verticalScrollBar() &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::Resize)) {
        syncToolbarGutter();
    } else if (uiBuilt_ && watched == libraryScrollArea_->viewport() &&
               event->type() == QEvent::Resize) {
        syncToolbarGutter();
    }
    return QWidget::eventFilter(watched, event);
}

void DownloadsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (uiBuilt_) {
        syncToolbarGutter();
        updateInspectorMode();
    }
}

void DownloadsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (uiBuilt_) {
        syncToolbarGutter();
        updateInspectorMode();
    }
}

// ---- inspector docking ----

// The width the catalog needs before its rows start eliding sizes.
int DownloadsPage::catalogMinWidth() const {
    int widest = 0;
    bool anyShown = false;
    for (const auto& [name, row] : rows_) {
        if (!row->isHidden()) {
            anyShown = true;
            widest = std::max(widest, row->minimumSizeHint().width());
        }
    }
    if (!anyShown) {
        for (const auto& [name, row] : rows_) {
            widest = std::max(widest, row->minimumSizeHint().width());
        }
    }
    const int scrollBar = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int toolbar = toolbarLayout_->minimumSize().width();
    return std::max(widest + scrollBar + 8, toolbar);
}

// Docks the profile beside the catalog when both fit, else floats it.
void DownloadsPage::updateInspectorMode() {
    const int needed = catalogMinWidth() + split_->spacing() + kInspectorWidth;
    const bool docked = width() >= needed;
    if (docked != inspectorDocked_) {
        inspectorDocked_ = docked;
        if (docked) {
            inspector_->setGraphicsEffect(nullptr);
            split_->addWidget(inspector_);
        } else {
            split_->removeWidget(inspector_);
            auto* shadow = new QGraphicsDropShadowEffect(inspector_);
            shadow->setBlurRadius(28);
            shadow->setOffset(-6, 0);
            shadow->setColor(QColor(0, 0, 0, 110));
            inspector_->setGraphicsEffect(shadow);
        }
        ui::setStyleProperty(inspector_, ui::prop::overlay, !docked);
        inspectorCloseButton_->setVisible(!docked);
    }
    if (docked) {
        inspector_->setVisible(true);
        return;
    }
    const QRect column = catalogColumn_->geometry();
    inspector_->setGeometry(QRect(column.right() - kInspectorWidth + 1, column.top(),
                                  kInspectorWidth, column.height()));
    inspector_->setVisible(inspectorOpen_);
    if (inspectorOpen_) {
        inspector_->raise();
    }
}

void DownloadsPage::closeInspector() {
    inspectorOpen_ = false;
    if (uiBuilt_ && !inspectorDocked_) {
        inspector_->setVisible(false);
    }
}

// The side profile that replaced the Model Details dialog.
QFrame* DownloadsPage::buildInspector() {
    auto* panel = new QFrame();
    panel->setObjectName(QStringLiteral("downloadsInspector"));
    panel->setFixedWidth(kInspectorWidth);
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(16, 14, 16, 14);
    outer->setSpacing(10);

    auto* eyebrowRow = new QHBoxLayout();
    eyebrowRow->setContentsMargins(0, 0, 0, 0);
    auto* eyebrow = new QLabel(QStringLiteral("Selected model"));
    eyebrow->setObjectName(QStringLiteral("settingsTileGroupTitle"));
    eyebrowRow->addWidget(eyebrow);
    eyebrowRow->addStretch();
    // Only shown while the profile floats over the catalog.
    inspectorCloseButton_ = new QPushButton(QStringLiteral("✕"));
    inspectorCloseButton_->setObjectName(QStringLiteral("downloadsInspectorClose"));
    inspectorCloseButton_->setCursor(Qt::PointingHandCursor);
    inspectorCloseButton_->setToolTip(QStringLiteral("Close the model profile"));
    inspectorCloseButton_->setFixedSize(26, 26);
    connect(inspectorCloseButton_, &QPushButton::clicked, this, &DownloadsPage::closeInspector);
    inspectorCloseButton_->setVisible(false);
    eyebrowRow->addWidget(inspectorCloseButton_);
    outer->addLayout(eyebrowRow);

    inspectorName_ = new ElidingLabel(QString());
    inspectorName_->setObjectName(QStringLiteral("downloadsInspectorName"));
    outer->addWidget(inspectorName_);

    // Shown instead of the profile until a row is picked.
    inspectorEmpty_ =
        new EmptyState(QStringLiteral("No model selected"),
                       QStringLiteral("Select a model to see its technical profile."));
    inspectorEmpty_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    outer->addWidget(inspectorEmpty_);

    inspectorTags_ = new ElidingLabel(QString());
    inspectorTags_->setObjectName(QStringLiteral("downloadsInspectorTags"));
    inspectorTags_->setVisible(false);
    outer->addWidget(inspectorTags_, 0, Qt::AlignLeft);

    // The profile prose is open-ended, so this column scrolls; the window chrome does not.
    auto* detailScroll = new QScrollArea();
    detailScroll->setObjectName(QStringLiteral("downloadsInspectorScroll"));
    detailScroll->setWidgetResizable(true);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailScroll->setFrameShape(QFrame::NoFrame);
    // A resizable scroll area propagates its content's minimum height, which is taller than the
    // panel; a small floor of its own keeps it from squeezing the name, facts and links, and lets
    // the page fit at the window's minimum height.
    detailScroll->setMinimumHeight(60);
    inspectorDetailScroll_ = detailScroll;

    auto* content = new QWidget();
    content->setObjectName(QStringLiteral("downloadsInspectorContent"));
    // Horizontal scrolling is off, so the content must be exactly as wide as the viewport, not
    // as wide as its widest unbreakable word.
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* detail = new QVBoxLayout(content);
    detail->setContentsMargins(0, 0, 6, 0);
    detail->setSpacing(12);

    inspectorDescription_ = new WrappedLabel(QString());
    inspectorDescription_->setObjectName(QStringLiteral("downloadsInspectorBody"));
    detail->addWidget(inspectorDescription_);

    inspectorFacts_ = new QGridLayout();
    inspectorFacts_->setContentsMargins(0, 0, 0, 0);
    inspectorFacts_->setHorizontalSpacing(12);
    inspectorFacts_->setVerticalSpacing(5);
    inspectorFacts_->setColumnStretch(1, 1);
    detail->addLayout(inspectorFacts_);

    inspectorBestForHeading_ = new QLabel(QStringLiteral("Best for"));
    inspectorBestForHeading_->setObjectName(QStringLiteral("downloadsSectionTitle"));
    inspectorBestFor_ = new WrappedLabel(QString());
    inspectorBestFor_->setObjectName(QStringLiteral("downloadsInspectorBody"));
    detail->addWidget(inspectorBestForHeading_);
    detail->addWidget(inspectorBestFor_);

    inspectorTradeoffsHeading_ = new QLabel(QStringLiteral("Tradeoffs"));
    inspectorTradeoffsHeading_->setObjectName(QStringLiteral("downloadsSectionTitle"));
    inspectorTradeoffs_ = new WrappedLabel(QString());
    inspectorTradeoffs_->setObjectName(QStringLiteral("downloadsInspectorBody"));
    detail->addWidget(inspectorTradeoffsHeading_);
    detail->addWidget(inspectorTradeoffs_);

    inspectorSourceNote_ = new WrappedLabel(
        QStringLiteral("Technical figures are bundled from the linked upstream model cards. "
                       "Speed and memory vary with hardware, compute type, audio, and decoding "
                       "settings."));
    inspectorSourceNote_->setText(inspectorSourceNote_->text() + QStringLiteral("\n\n") +
                                  q(models::model_security_notice()));
    inspectorSourceNote_->setObjectName(QStringLiteral("downloadsSourceNote"));
    detail->addWidget(inspectorSourceNote_);
    detail->addStretch();

    detailScroll->setWidget(content);
    outer->addWidget(detailScroll, 1);

    inspectorUsage_ = new QLabel(QString());
    inspectorUsage_->setObjectName(QStringLiteral("downloadsInspectorUsage"));
    inspectorUsage_->setVisible(false);
    outer->addWidget(inspectorUsage_);

    inspectorRepoButton_ = new Button(QStringLiteral("Hugging Face ↗"));
    inspectorRepoButton_->setObjectName(QStringLiteral("downloadsRepoButton"));
    fitCompactButton(inspectorRepoButton_, 0);
    connect(inspectorRepoButton_, &QPushButton::clicked, this, [this] {
        if (details_ != nullptr) {
            openDetailsUrl(q(details_->repository_url));
        }
    });

    inspectorOriginButton_ = new Button(QStringLiteral("Original ↗"));
    inspectorOriginButton_->setObjectName(QStringLiteral("downloadsOriginButton"));
    fitCompactButton(inspectorOriginButton_, 0);
    connect(inspectorOriginButton_, &QPushButton::clicked, this, [this] {
        if (details_ != nullptr) {
            openDetailsUrl(q(details_->origin_url));
        }
    });

    inspectorLicenseButton_ = new Button(QStringLiteral("License ↗"));
    inspectorLicenseButton_->setObjectName(QStringLiteral("downloadsLicenseButton"));
    fitCompactButton(inspectorLicenseButton_, 0);
    connect(inspectorLicenseButton_, &QPushButton::clicked, this, [this] {
        if (details_ != nullptr) {
            openDetailsUrl(
                q(details_->license_url.empty() ? details_->origin_url : details_->license_url));
        }
    });
    // The 300 px column cannot hold three labels side by side at larger font scales; the row
    // wraps rather than letting the buttons overlap.
    inspectorLinks_ =
        new ButtonRow({inspectorRepoButton_, inspectorOriginButton_, inspectorLicenseButton_});
    outer->addWidget(inspectorLinks_);
    // Takes the spare height only while the profile (stretch 1) is hidden, so the empty state
    // stays at the top.
    outer->addStretch(0);

    details_ = nullptr;
    setInspectorEnabled(false);
    return panel;
}

// The optional components, on their own tab with room to breathe.
QScrollArea* DownloadsPage::buildComponentsSection() {
    auto* scroll = new QScrollArea();
    scroll->setObjectName(QStringLiteral("downloadsComponentsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* container = new QWidget();
    container->setObjectName(QStringLiteral("downloadsComponentsContainer"));
    auto* column = new QVBoxLayout(container);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(buildComponentStrip());
    column->addStretch();
    scroll->setWidget(container);
    return scroll;
}

void DownloadsPage::showSection(int index) {
    modelsSection_->setVisible(index == 0);
    componentsSection_->setVisible(index == 1);
}

// The components strip; hidden on a platform with nothing installable, so no heading advertises
// an empty section.
QFrame* DownloadsPage::buildComponentStrip() {
    auto* strip = new QFrame();
    strip->setObjectName(QStringLiteral("downloadsComponentStrip"));
    auto* layout = new QVBoxLayout(strip);
    layout->setContentsMargins(0, 2, 0, 12);
    layout->setSpacing(8);

    std::vector<models::ComponentInfo> infos;
    if (options_.listComponents) {
        try {
            infos = options_.listComponents();
        } catch (const std::exception& error) {
            kLog.error_with(error, "Couldn't list the optional components");
        }
    }
    if (infos.empty()) {
        strip->setVisible(false);
        return strip;
    }

    auto* caption = new WrappedLabel(
        QStringLiteral("Optional add-ons, downloaded on demand so the installer stays small."));
    caption->setObjectName(QStringLiteral("infoLabel"));
    layout->addWidget(caption);

    for (const models::ComponentInfo& info : infos) {
        auto* row = new ComponentRowWidget(q(info.component_id));
        connect(row, &ComponentRowWidget::installClicked, this,
                &DownloadsPage::componentInstallRequested);
        connect(row, &ComponentRowWidget::cancelClicked, this,
                &DownloadsPage::componentCancelRequested);
        connect(row, &ComponentRowWidget::removeClicked, this,
                &DownloadsPage::confirmComponentRemoval);
        connect(row, &DownloadRow::detailsRequested, this, &DownloadsPage::showComponentDetails);
        componentRows_.emplace_back(q(info.component_id), row);
        layout->addWidget(row);
    }
    return strip;
}

// ---- inspector ----

void DownloadsPage::selectModel(const QString& modelName) {
    ensureUi();
    if (modelName == QStringLiteral("auto") || row(modelName) == nullptr) {
        return;
    }
    selectedModel_ = modelName;
    details_ = detailsOf(modelName);
    renderInspector();
    for (const auto& [name, each] : rows_) {
        ui::setStyleProperty(each, ui::prop::selected, name == modelName);
    }
    inspectorOpen_ = true;
    updateInspectorMode();
}

void DownloadsPage::revealModel(const QString& modelName) {
    ModelRowWidget* found = row(modelName);
    if (found == nullptr) {
        return;
    }
    if (found->isHidden()) {
        filterEdit_->clear();
        backendFilterCombo_->setCurrentIndex(0);
        statusFilterCombo_->setCurrentIndex(0);
    }
    tabs_->setCurrentIndex(0);
    selectModel(modelName);
    libraryScrollArea_->ensureWidgetVisible(found, 0, 12);
}

void DownloadsPage::showBackend(const QString& backend) {
    ensureUi();
    tabs_->setCurrentIndex(0);
    const int index =
        backendFilterCombo_->findData(backend.isEmpty() ? QStringLiteral("all") : backend);
    backendFilterCombo_->setCurrentIndex(std::max(0, index));
}

void DownloadsPage::renderInspector() {
    if (details_ == nullptr) {
        setInspectorEnabled(false);
        return;
    }
    setInspectorEnabled(true);
    const models::ModelDetails& details = *details_;
    const models::SpeechModel* model = models::model(details.model_name);
    inspectorName_->setText(model != nullptr ? q(model->label) : q(details.model_name));
    inspectorName_->setToolTip(q(details.model_name));
    const QString tags = q(details.compact_tags());
    inspectorTags_->setText(tags);
    inspectorTags_->setVisible(!tags.isEmpty());
    inspectorDescription_->setText(q(details.description));
    inspectorBestFor_->setText(q(details.best_for));
    QStringList tradeoffs;
    for (const std::string& item : details.limitations) {
        tradeoffs.append(QStringLiteral("• ") + q(item));
    }
    inspectorTradeoffs_->setText(tradeoffs.join(QLatin1Char('\n')));
    const bool onHuggingFace = details.repository_url.find("huggingface.co/") != std::string::npos;
    inspectorRepoButton_->setText(onHuggingFace ? QStringLiteral("Hugging Face ↗")
                                                : QStringLiteral("Repository ↗"));
    inspectorRepoButton_->setToolTip(q(details.repository_url));
    inspectorOriginButton_->setToolTip(q(details.origin_url));
    inspectorLicenseButton_->setToolTip(
        q(details.license_url.empty() ? details.origin_url : details.license_url));
    inspectorLinks_->refresh();

    std::string hosts;
    for (const std::string& host : details.download_hosts) {
        hosts += (hosts.empty() ? "" : ", ") + host;
    }
    // Every catalog model has a bundled memory guidance; OpenWhisper's per-GPU guidance is
    // Whisper's only.
    const std::vector<std::pair<QString, std::string>> facts{
        {QStringLiteral("Origin"), details.origin_name},
        {QStringLiteral("Repository"), details.repository_id},
        {QStringLiteral("Maintainer"), details.maintainer},
        {QStringLiteral("Family"), details.family},
        {QStringLiteral("Languages"), details.language_support},
        {QStringLiteral("Tasks"), details.task_support},
        {QStringLiteral("Parameters"), details.parameter_count},
        {QStringLiteral("Published speed"), details.relative_performance},
        {QStringLiteral("Memory guidance"), details.memory_guidance},
        {QStringLiteral("Download size"), details.download_size()},
        {QStringLiteral("Local format"), details.runtime_format},
        {QStringLiteral("License"), details.license},
        {QStringLiteral("Download host"), hosts.empty() ? std::string("Local folder") : hosts},
        {QStringLiteral("Version"),
         details.revision.empty() ? std::string("Selected by model source") : details.revision},
        {QStringLiteral("Download checks"), details.verification},
    };
    while (inspectorFacts_->count() > 0) {
        QLayoutItem* item = inspectorFacts_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    factLabels_.clear();
    int index = 0;
    for (const auto& [caption, value] : facts) {
        auto* captionLabel = new QLabel(caption);
        captionLabel->setObjectName(QStringLiteral("downloadsFactLabel"));
        captionLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        // Values include commit hashes, which have nowhere to wrap unless told they may.
        auto* valueLabel = new WrappedLabel(q(value), nullptr, true);
        valueLabel->setObjectName(QStringLiteral("downloadsFactValue"));
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        inspectorFacts_->addWidget(captionLabel, index, 0);
        inspectorFacts_->addWidget(valueLabel, index, 1);
        factLabels_[caption] = valueLabel;
        ++index;
    }
}

void DownloadsPage::setInspectorEnabled(bool enabled) {
    if (!enabled) {
        inspectorName_->setText(QString());
        inspectorDescription_->setText(QString());
        inspectorTags_->setVisible(false);
        inspectorBestFor_->setText(QString());
        inspectorTradeoffs_->setText(QString());
    }
    inspectorEmpty_->setVisible(!enabled);
    inspectorName_->setVisible(enabled);
    inspectorDetailScroll_->setVisible(enabled);
    for (QWidget* widget : std::initializer_list<QWidget*>{
             inspectorBestForHeading_, inspectorBestFor_, inspectorTradeoffsHeading_,
             inspectorTradeoffs_, inspectorSourceNote_, inspectorRepoButton_,
             inspectorOriginButton_, inspectorLicenseButton_}) {
        widget->setVisible(enabled);
    }
    inspectorLinks_->refresh();
}

void DownloadsPage::openDetailsUrl(const QString& url) {
    if (openUrl) {
        openUrl(QUrl(url));
    }
}

// Every catalog model lives under the speech models folder, so that is what opens.
void DownloadsPage::openCacheFolder() {
    openFolderInFileManager(options_.modelsFolder);
}

// ---- catalog actions ----

void DownloadsPage::onDownloadClicked(const QString& modelName) {
    Q_EMIT downloadRequested(modelName);
}

int DownloadsPage::askQuestion(const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    return runMessageBox ? runMessageBox(box) : box.exec();
}

void DownloadsPage::onDeleteClicked(const QString& modelName) {
    const models::Confirmation confirmation = models::delete_model_confirmation(u(modelName));
    if (askQuestion(q(confirmation.title), q(confirmation.text)) == QMessageBox::Yes) {
        Q_EMIT deleteRequested(modelName);
    }
}

// ---- batch selection ----

void DownloadsPage::onSelectionToggled(const QString& modelName, bool checked) {
    if (checked) {
        selectedModels_.insert(u(modelName));
    } else {
        selectedModels_.erase(u(modelName));
    }
    updateSelectionBar();
}

// Checks every row that still needs a download (downloaded rows excluded).
void DownloadsPage::selectAllMissing() {
    ensureUi();
    const std::vector<std::string> missing = missingNames();
    for (const auto& [name, each] : rows_) {
        each->selectCheckbox()->setChecked(std::ranges::find(missing, u(name)) != missing.end());
    }
}

void DownloadsPage::clearSelection() {
    ensureUi();
    for (const auto& [name, each] : rows_) {
        each->selectCheckbox()->setChecked(false);
    }
}

// The selection in catalog order: the order the batch downloads in.
std::vector<std::string> DownloadsPage::selectedNames() const {
    std::vector<std::string> names;
    for (const auto& [name, each] : rows_) {
        if (selectedModels_.contains(u(name))) {
            names.push_back(u(name));
        }
    }
    return names;
}

// Every catalog model without local files, in catalog order.
std::vector<std::string> DownloadsPage::missingNames() const {
    std::vector<std::string> names;
    for (const auto& [name, each] : rows_) {
        if (!each->isCached()) {
            names.push_back(u(name));
        }
    }
    return names;
}

void DownloadsPage::updateSelectionBar() {
    models::SelectionBarInput input;
    for (const std::string& name : selectedNames()) {
        if (ModelRowWidget* each = row(q(name))) {
            input.selected_sizes.push_back(each->sortSizeBytes());
        }
    }
    input.any_missing = !missingNames().empty();
    input.busy = queue_.slot_busy();
    input.downloads_blocked = downloadsBlocked_;
    const models::SelectionBar bar = models::selection_summary(input);
    selectionSummary_->setText(q(bar.summary));
    downloadSelectedButton_->setText(q(bar.download_text));
    downloadSelectedButton_->setEnabled(bar.download_enabled);
    downloadSelectedButton_->setToolTip(q(bar.download_tooltip));
    clearSelectionButton_->setEnabled(bar.clear_enabled);
    selectAllButton_->setEnabled(bar.select_all_enabled);
    downloadAllButton_->setEnabled(bar.download_all_enabled);
    downloadAllButton_->setVisible(bar.download_all_visible);
}

// Confirms sizes and destination, then hands the plan to the app.
void DownloadsPage::requestBatch(const std::vector<std::string>& modelNames) {
    BatchDownloadDialog dialog(modelNames, options_.modelsFolder, this);
    const int result = runDialog ? runDialog(dialog) : dialog.exec();
    if (result == QDialog::Accepted) {
        QStringList names;
        for (const std::string& name : modelNames) {
            names.append(q(name));
        }
        Q_EMIT batchDownloadRequested(names);
    }
}

void DownloadsPage::focusComponent(const QString& componentId) {
    ComponentRowWidget* found = componentRow(componentId);
    if (found == nullptr) {
        return;
    }
    tabs_->setCurrentIndex(1);
    for (const auto& [id, each] : componentRows_) {
        ui::setStyleProperty(each, ui::prop::selected, id == componentId);
    }
    // The tab was just shown: laid out, the row has a position to scroll to.
    componentsSection_->widget()->layout()->activate();
    componentsSection_->ensureWidgetVisible(found, 0, 12);
    found->setFocus(Qt::OtherFocusReason);
}

// The bundled profile popup of one component row.
void DownloadsPage::showComponentDetails(const QString& componentId) {
    if (componentRow(componentId) == nullptr) {
        return;
    }
    focusComponent(componentId);
    try {
        const models::ComponentInfo info = options_.describeComponent
                                               ? options_.describeComponent(u(componentId))
                                               : models::ComponentInfo{};
        ComponentDetailsDialog dialog(u(componentId), info, this);
        dialog.setUrlOpener(openUrl);
        if (runDialog) {
            runDialog(dialog);
        } else {
            dialog.exec();
        }
    } catch (const std::exception& error) {
        kLog.error_with(error,
                        std::format("Couldn't show the profile of component {}", u(componentId)));
    }
    if (ComponentRowWidget* selected = componentRow(componentId)) {
        ui::setStyleProperty(selected, ui::prop::selected, false);
    }
}

// Asks before deleting a multi-gigabyte component.
void DownloadsPage::confirmComponentRemoval(const QString& componentId) {
    if (!options_.describeComponent) {
        return;
    }
    // A slot: what the coordinator throws must not reach the event loop.
    models::ComponentInfo info;
    try {
        info = options_.describeComponent(u(componentId));
    } catch (const std::exception& error) {
        kLog.error_with(error,
                        std::format("Couldn't read the state of component {}", u(componentId)));
        return;
    }
    const models::Confirmation confirmation =
        models::remove_component_confirmation(info.display_name, info.install_bytes);
    if (askQuestion(q(confirmation.title), q(confirmation.text)) == QMessageBox::Yes) {
        Q_EMIT componentRemoveRequested(componentId);
    }
}

// ---- state, driven by the presenter ----

void DownloadsPage::refresh(bool scan) {
    if (!options_.backgroundScan) {
        inventoryLoading_ = false;
        models::CachedModelsSnapshot::Inventory found;
        if (options_.scanInventory) {
            try {
                found = options_.scanInventory();
            } catch (const std::exception& error) {
                kLog.warning("Couldn't read the downloaded models: {}", error.what());
            }
        }
        applyInventory(found);
        return;
    }
    const std::optional<models::CachedModelsSnapshot::Inventory> cached =
        options_.peekInventory ? options_.peekInventory() : std::nullopt;
    inventoryLoading_ = !cached.has_value() && scan;
    applyInventory(cached.value_or(models::CachedModelsSnapshot::Inventory{}));
    if (!scan) {
        return;
    }
    const std::uint64_t token = mailbox_->gate.next();
    // Bound here: the worker reaches the page only through the mailbox.
    io_.post([mailbox = mailbox_, lookup = options_.scanInventory, token] {
        models::CachedModelsSnapshot::Inventory found;
        if (lookup) {
            try {
                found = lookup();
            } catch (const std::exception& error) {
                kLog.warning("Couldn't read the downloaded models: {}", error.what());
            }
        }
        mailbox->deliver(token, [found = std::move(found)](DownloadsPage& page) mutable {
            page.onScanFinished(std::move(found));
        });
    });
}

void DownloadsPage::onScanFinished(models::CachedModelsSnapshot::Inventory inventory) {
    inventoryLoading_ = false;
    applyInventory(inventory);
}

Json DownloadsPage::settingsSnapshot() const {
    try {
        return *settings_.snapshot();
    } catch (const std::exception& error) {
        kLog.warning("Could not load settings for the Downloads page: {}", error.what());
        return Json::object();
    }
}

models::ModelsInUse DownloadsPage::modelsInUse(const Json& settings) const {
    std::string meeting;
    if (options_.meetingModel) {
        meeting = options_.meetingModel(settings);
    }
    std::optional<std::string> loaded;
    if (options_.loadedModel) {
        loaded = options_.loadedModel();
    }
    if (loaded && loaded->empty()) {
        loaded.reset();
    }
    return models::models_in_use(settings, std::move(meeting), std::move(loaded));
}

const EnvReader& DownloadsPage::env() const {
    return options_.env != nullptr ? *options_.env : system_env();
}

// _refresh_cached_model_state.
void DownloadsPage::applyInventory(const models::CachedModelsSnapshot::Inventory& inventory) {
    inventory_ = inventory;
    if (!uiBuilt_) {
        Q_EMIT inventoryChanged();
        return;
    }
    const Json settings = settingsSnapshot();
    const models::ModelsInUse inUse = modelsInUse(settings);
    const bool blocked = models::is_hf_hub_offline_env_set(env());
    downloadsBlocked_ = blocked;
    envBanner_->setVisible(blocked);

    RowContext context;
    context.loadedModel = inUse.loaded;
    context.dictationModel = inUse.dictation;
    context.meetingModel = inUse.meeting;
    context.downloadsBlocked = blocked;
    context.slotBusy = queue_.slot_busy();
    context.selectionLocked = context.slotBusy || blocked || inventoryLoading_;
    for (const auto& [name, each] : rows_) {
        std::optional<models::CachedModelInfo> info;
        if (const auto found = inventory_.find(u(each->repoId())); found != inventory_.end()) {
            info = found->second;
        }
        refreshRow(each, info, context);
    }

    showCacheTotals();
    refreshComponents();
    applyFilter();
    if (selectedModel_) {
        inspectorUsage_->setText(
            q(models::usage_text(u(*selectedModel_), inUse.dictation, inUse.meeting)));
        inspectorUsage_->setVisible(!inspectorUsage_->text().isEmpty());
    }
    stopBatchButton_->setVisible(!queue_.batch_queue().empty());
    updateSelectionBar();
    if (inventoryLoading_) {
        selectAllButton_->setEnabled(false);
        downloadAllButton_->setEnabled(false);
        downloadSelectedButton_->setEnabled(false);
    }
    Q_EMIT inventoryChanged();
}

void DownloadsPage::refreshRow(ModelRowWidget* each,
                               const std::optional<models::CachedModelInfo>& info,
                               const RowContext& context) {
    const std::string name = u(each->modelName());
    models::ModelRowInput input;
    // OpenWhisper's revision pins are Whisper's: a speech model's inventory entry stands.
    input.info = models::current_cache_info(info, "");
    input.is_active = false;
    // One repository per catalog model, so the loaded repository is the loaded model's row.
    input.is_loaded = context.loadedModel.has_value() && *context.loadedModel == name;
    input.downloading = queue_.downloading_model() == name;
    input.downloads_blocked = context.downloadsBlocked;
    input.download_slot_busy = context.slotBusy || inventoryLoading_;
    input.queued = queue_.is_queued(name);
    input.selection_enabled = !context.selectionLocked;
    each->updateState(input);
    // Assignment lives on each feature's page; this one only downloads.
    each->setActiveButton()->setVisible(false);
    each->setUsage(q(models::usage_text(name, context.dictationModel, context.meetingModel)));
    if (info) {
        // A model that gained files elsewhere must not stay ticked.
        each->selectCheckbox()->setChecked(false);
        selectedModels_.erase(name);
    }
}

// _show_cache_totals: the distinct repositories seen among the rows and their bytes.
void DownloadsPage::showCacheTotals() {
    const models::StorageSummary summary = storageSummary();
    const int total = static_cast<int>(rows_.size());
    tabs_->setTitle(0, q(models::models_tab_title(inventoryLoading_, summary.downloaded, total)));
    statsLabel_->setText(q(models::models_stats_text(inventoryLoading_, summary.downloaded, total,
                                                     summary.bytes, u(options_.modelsFolder))));
}

models::StorageSummary DownloadsPage::storageSummary() const {
    return models::storage_summary(inventory_);
}

QString DownloadsPage::railValue() const {
    return q(models::downloads_rail_value(queue_, inventoryLoading_, storageSummary()));
}

void DownloadsPage::refreshComponents() {
    if (!uiBuilt_) {
        return;
    }
    std::vector<models::ComponentInfo> infos;
    for (const auto& [id, each] : componentRows_) {
        models::ComponentInfo info;
        try {
            info = options_.describeComponent ? options_.describeComponent(u(id))
                                              : models::ComponentInfo{};
        } catch (const std::exception& error) {
            kLog.error_with(error, std::format("Couldn't read the state of component {}", u(id)));
            continue;
        }
        const bool installing = options_.isInstalling && options_.isInstalling(u(id));
        each->updateState(info, installing);
        infos.push_back(std::move(info));
    }
    if (const auto title = models::components_tab_title(infos)) {
        tabs_->setTitle(1, q(*title));
    }
}

void DownloadsPage::setComponentProgress(const QString& componentId, const QString& phase,
                                         std::int64_t done, std::int64_t total) {
    if (!uiBuilt_) {
        return;
    }
    if (ComponentRowWidget* found = componentRow(componentId)) {
        found->setProgress(u(phase), done, total);
    }
}

void DownloadsPage::finishComponentInstall(const QString& componentId, bool success,
                                           const QString& message) {
    (void)componentId;
    (void)success;
    refreshComponents();
    if (!message.isEmpty()) {
        setMessage(message);
    }
}

void DownloadsPage::setMessage(const QString& text) {
    message_ = text;
    if (uiBuilt_) {
        messageLabel_->setText(text);
    }
}

void DownloadsPage::setDownloading(const QString& modelName) {
    setMessage(q(queue_.set_downloading(u(modelName))));
    refresh();
}

void DownloadsPage::setDownloadProgress(const QString& modelName, std::int64_t done,
                                        std::int64_t total) {
    if (uiBuilt_) {
        if (ModelRowWidget* found = row(modelName)) {
            found->setProgress(done, total);
        }
    }
    const models::DownloadQueue::Progress progress =
        queue_.set_download_progress(u(modelName), done, total);
    if (progress.message) {
        setMessage(q(*progress.message));
    }
    if (progress.rail_changed) {
        Q_EMIT progressChanged();
    }
}

void DownloadsPage::finishDownload(const QString& modelName, bool success) {
    if (const auto message = queue_.finish_download(u(modelName), success)) {
        setMessage(q(*message));
    }
    refresh();
}

void DownloadsPage::beginBatch(const QStringList& modelNames) {
    std::vector<std::string> names;
    for (const QString& name : modelNames) {
        names.push_back(u(name));
    }
    const std::vector<std::string> known = models::model_row_names();
    setMessage(q(queue_.begin_batch(names, [&known](std::string_view name) {
        return std::ranges::find(known, name) != known.end();
    })));
    refresh();
}

void DownloadsPage::finishBatch(int completed, int planned) {
    setMessage(q(queue_.finish_batch(completed, planned)));
    refresh();
}

void DownloadsPage::showDeleteResult(const QString& modelName, bool success, const QString& error) {
    setMessage(q(models::delete_result_message(u(modelName), success, u(error))));
}

// ---- filtering ----

void DownloadsPage::applyFilter() {
    const std::string needle = u(filterEdit_->text());
    const std::string status = u(statusFilterCombo_->currentData().toString());
    const std::string backend = u(backendFilterCombo_->currentData().toString());
    const std::string mode = u(sortCombo_->currentData().toString());
    std::vector<models::ModelRowFacts> facts;
    for (const auto& [name, each] : rows_) {
        facts.push_back(each->facts());
    }
    bool anyVisible = false;
    int index = 0;
    for (const models::ModelRowFacts& fact : models::sorted_rows(std::move(facts), mode)) {
        ModelRowWidget* each = row(q(fact.model_name));
        listLayout_->insertWidget(index++, each);
        const bool visible = models::row_visible(fact, needle, backend, status, &fuzzy_match);
        each->setVisible(visible);
        anyVisible = anyVisible || visible;
    }
    emptyLabel_->setVisible(!anyVisible);
}

// ---- SettingsPageModule ----

std::optional<std::vector<appcore::SearchEntry>> DownloadsPage::searchEntries() {
    if (!uiBuilt_) {
        return std::nullopt;
    }
    std::vector<appcore::SearchEntry> entries;
    for (const auto& [name, row] : rows_) {
        const std::string key = u(name);
        const models::SpeechModel* model = models::model(key);
        const models::SpeechBackend* backend = models::backend(row->facts().backend);
        appcore::SearchEntry entry;
        entry.kind = appcore::SearchKind::Model;
        entry.title = model != nullptr ? std::string(model->label) : key;
        // " · ".join of the parts that are not empty.
        entry.detail = "Downloads";
        for (const std::string& part :
             {u(row->sizeLabel()->text()), models::model_row_summary(key)}) {
            if (!part.empty()) {
                entry.detail += " · " + part;
            }
        }
        if (row->usageLabel()->isVisibleTo(row) && !row->usageLabel()->text().isEmpty()) {
            entry.badges.push_back(u(row->usageLabel()->text()));
        }
        if (row->isCached()) {
            entry.badges.emplace_back("Downloaded");
        }
        entry.destination = std::string(destinations::downloads);
        entry.model_name = key;
        entry.keywords =
            key + " " + (backend != nullptr ? std::string(backend->label) : "Whisper") + " model";
        entry.icon = "download-blue.svg";
        entries.push_back(std::move(entry));
    }
    for (const auto& [componentId, row] : componentRows_) {
        std::string title;
        std::string summary;
        bool usable = false;
        try {
            if (!options_.describeComponent) {
                throw std::runtime_error("no component service");
            }
            const models::ComponentInfo info = options_.describeComponent(u(componentId));
            title = info.display_name;
            summary = info.summary;
            usable = info.is_usable();
        } catch (const std::exception& error) {
            kLog.debug("Could not describe component {} for search; using its row label: {}",
                       u(componentId), error.what());
            title = u(row->nameLabel()->text());
        }
        appcore::SearchEntry entry;
        entry.kind = appcore::SearchKind::Model;
        entry.title = title;
        // f"Downloads › Components · {summary}".rstrip(" ·")
        entry.detail = "Downloads › Components · " + summary;
        while (!entry.detail.empty() &&
               (entry.detail.back() == ' ' || entry.detail.ends_with("·"))) {
            entry.detail.erase(entry.detail.size() - (entry.detail.back() == ' ' ? 1 : 2));
        }
        entry.destination = std::string(destinations::downloads);
        entry.component_id = u(componentId);
        if (usable) {
            entry.badges.emplace_back("Installed");
        }
        entry.keywords = "component runtime add-on";
        entry.icon = "box-blue.svg";
        entries.push_back(std::move(entry));
    }
    return entries;
}

void DownloadsPage::loadSettings(const Json& settings) {
    if (policyLoader_) {
        policyLoader_(settings);
    }
}

// The window's refresh: a hidden window reuses the last shared scan; showing it rescans.
void DownloadsPage::refreshPage() {
    refresh(window() != nullptr && window()->isVisible());
}

QWidget* DownloadsPage::namedControl(std::string_view name) {
    if (name == "hf_policy_combo") {
        return policyControl_;
    }
    return nullptr;
}

// ---- widgets ----

SegmentedBar* DownloadsPage::tabs() {
    ensureUi();
    return tabs_;
}
QWidget* DownloadsPage::modelsSection() {
    ensureUi();
    return modelsSection_;
}
QScrollArea* DownloadsPage::componentsSection() {
    ensureUi();
    return componentsSection_;
}
ElidingLabel* DownloadsPage::statsLabel() {
    ensureUi();
    return statsLabel_;
}
Button* DownloadsPage::downloadAllButton() {
    ensureUi();
    return downloadAllButton_;
}
Button* DownloadsPage::openFolderButton() {
    ensureUi();
    return openFolderButton_;
}
QHBoxLayout* DownloadsPage::policyRow() {
    ensureUi();
    return policyRow_;
}
QLabel* DownloadsPage::envBanner() {
    ensureUi();
    return envBanner_;
}
QWidget* DownloadsPage::catalogColumn() {
    ensureUi();
    return catalogColumn_;
}
QLineEdit* DownloadsPage::filterEdit() {
    ensureUi();
    return filterEdit_;
}
ElidingComboBox* DownloadsPage::backendFilterCombo() {
    ensureUi();
    return backendFilterCombo_;
}
ElidingComboBox* DownloadsPage::statusFilterCombo() {
    ensureUi();
    return statusFilterCombo_;
}
ElidingComboBox* DownloadsPage::sortCombo() {
    ensureUi();
    return sortCombo_;
}
QHBoxLayout* DownloadsPage::toolbarLayout() {
    ensureUi();
    return toolbarLayout_;
}
Button* DownloadsPage::selectAllButton() {
    ensureUi();
    return selectAllButton_;
}
Button* DownloadsPage::clearSelectionButton() {
    ensureUi();
    return clearSelectionButton_;
}
ElidingLabel* DownloadsPage::selectionSummary() {
    ensureUi();
    return selectionSummary_;
}
Button* DownloadsPage::downloadSelectedButton() {
    ensureUi();
    return downloadSelectedButton_;
}
QScrollArea* DownloadsPage::libraryScrollArea() {
    ensureUi();
    return libraryScrollArea_;
}
QVBoxLayout* DownloadsPage::listLayout() {
    ensureUi();
    return listLayout_;
}
QLabel* DownloadsPage::emptyLabel() {
    ensureUi();
    return emptyLabel_;
}
QLabel* DownloadsPage::messageLabel() {
    ensureUi();
    return messageLabel_;
}
Button* DownloadsPage::stopBatchButton() {
    ensureUi();
    return stopBatchButton_;
}
const std::vector<std::pair<QString, ModelRowWidget*>>& DownloadsPage::rows() {
    ensureUi();
    return rows_;
}
ModelRowWidget* DownloadsPage::row(const QString& modelName) {
    ensureUi();
    for (const auto& [name, each] : rows_) {
        if (name == modelName) {
            return each;
        }
    }
    return nullptr;
}
const std::vector<std::pair<QString, ComponentRowWidget*>>& DownloadsPage::componentRows() {
    ensureUi();
    return componentRows_;
}
ComponentRowWidget* DownloadsPage::componentRow(const QString& componentId) {
    ensureUi();
    for (const auto& [id, each] : componentRows_) {
        if (id == componentId) {
            return each;
        }
    }
    return nullptr;
}
QStringList DownloadsPage::rowOrder() {
    ensureUi();
    QStringList order;
    for (int index = 0; index < listLayout_->count(); ++index) {
        if (auto* each = qobject_cast<ModelRowWidget*>(listLayout_->itemAt(index)->widget())) {
            order.append(each->modelName());
        }
    }
    return order;
}
QFrame* DownloadsPage::inspector() {
    ensureUi();
    return inspector_;
}
QPushButton* DownloadsPage::inspectorCloseButton() {
    ensureUi();
    return inspectorCloseButton_;
}
ElidingLabel* DownloadsPage::inspectorName() {
    ensureUi();
    return inspectorName_;
}
EmptyState* DownloadsPage::inspectorEmpty() {
    ensureUi();
    return inspectorEmpty_;
}
ElidingLabel* DownloadsPage::inspectorTags() {
    ensureUi();
    return inspectorTags_;
}
QScrollArea* DownloadsPage::inspectorDetailScroll() {
    ensureUi();
    return inspectorDetailScroll_;
}
WrappedLabel* DownloadsPage::inspectorDescription() {
    ensureUi();
    return inspectorDescription_;
}
QLabel* DownloadsPage::inspectorBestForHeading() {
    ensureUi();
    return inspectorBestForHeading_;
}
WrappedLabel* DownloadsPage::inspectorBestFor() {
    ensureUi();
    return inspectorBestFor_;
}
QLabel* DownloadsPage::inspectorTradeoffsHeading() {
    ensureUi();
    return inspectorTradeoffsHeading_;
}
WrappedLabel* DownloadsPage::inspectorTradeoffs() {
    ensureUi();
    return inspectorTradeoffs_;
}
WrappedLabel* DownloadsPage::inspectorSourceNote() {
    ensureUi();
    return inspectorSourceNote_;
}
QLabel* DownloadsPage::inspectorUsage() {
    ensureUi();
    return inspectorUsage_;
}
Button* DownloadsPage::inspectorRepoButton() {
    ensureUi();
    return inspectorRepoButton_;
}
Button* DownloadsPage::inspectorOriginButton() {
    ensureUi();
    return inspectorOriginButton_;
}
Button* DownloadsPage::inspectorLicenseButton() {
    ensureUi();
    return inspectorLicenseButton_;
}
ButtonRow* DownloadsPage::inspectorLinks() {
    ensureUi();
    return inspectorLinks_;
}
WrappedLabel* DownloadsPage::factLabel(const QString& caption) {
    ensureUi();
    const auto found = factLabels_.find(caption);
    return found == factLabels_.end() ? nullptr : found->second;
}

} // namespace fv
