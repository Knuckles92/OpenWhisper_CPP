#include "ui_downloads/model_row_widget.h"

#include <algorithm>

#include <QCheckBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "uimath/fuzzy_match.h"

namespace fv {
namespace {

// The rules only a model row has; the rest come from rowStyle.
constexpr const char* kStateRules = R"qss(
    QFrame#modelRow[active="true"] {
        background-color: @accent-tint;
        border: 1px solid @accent-tint-border;
    }
    QFrame#modelRow[active="true"]:hover {
        background-color: @accent-tint-strong;
        border: 1px solid @accent-tint-border-strong;
    })qss";

constexpr const char* kExtraRules = R"qss(
    QLabel#modelRowSize[muted="true"] {
        color: @slate-text-4;
    }
    QLabel#modelRowBadge[tone="active"] {
        background-color: rgba(@accent-rgb, 0.14);
        color: @accent-soft;
        border: 1px solid rgba(@accent-rgb, 0.28);
    }
    QLabel#modelRowBadge[tone="downloaded"] {
        background-color: rgba(@success-rgb, 0.12);
        color: @success-text-strong;
        border: 1px solid rgba(@success-rgb, 0.28);
    }
    QLabel#modelRowBadge[tone="queued"] {
        background-color: rgba(@accent-rgb, 0.08);
        color: @slate-text-3;
        border: 1px solid rgba(@accent-rgb, 0.18);
    }
    QCheckBox#modelSelectCheckbox {
        background-color: transparent;
        border: none;
    }
    QLabel#modelRowUsage {
        background-color: rgba(@accent-rgb, 0.10);
        color: @accent-soft;
        border: 1px solid rgba(@accent-rgb, 0.22);
        border-radius: 6px;
        padding: 2px 8px;
        font-size: 10px;
        font-weight: 600;
    }
    QPushButton#modelSetActiveButton {
        background-color: @slate-raised;
        color: @slate-text;
        border: 1px solid @slate-border-strong;
    }
    QPushButton#modelSetActiveButton:hover {
        background-color: @slate-raised;
        border: 1px solid @slate-border-hover;
    }
    QPushButton#modelDeleteButton:disabled {
        color: @slate-text-disabled;
        border: 1px solid @slate-border-subtle;
    }
)qss";

QString rowStyleSource() {
    return rowStyle(QStringLiteral("modelRow"), QStringLiteral("modelDownloadButton"),
                    QStringLiteral("modelDeleteButton"), QStringLiteral("modelRowProgress"),
                    QStringLiteral("modelSetActiveButton"), QString::fromLatin1(kStateRules),
                    QString::fromLatin1(kExtraRules));
}

QFont segoe(int pointSize, bool bold = false) {
    QFont font(QStringLiteral("Segoe UI"), pointSize);
    font.setBold(bold);
    return font;
}

} // namespace

ModelRowWidget::ModelRowWidget(const QString& modelName, QWidget* parent)
    : DownloadRow(modelName, QStringLiteral("modelRow"), rowStyleSource(),
                  QStringLiteral("Click to view technical details"),
                  QStringLiteral("%1 model").arg(modelName),
                  QStringLiteral("Open technical details. Model management actions are separate."),
                  parent),
      modelName_(modelName) {
    const std::string name = qt::toUtf8(modelName);
    const models::ModelRowFacts facts = models::model_row_facts(name, std::nullopt);
    repoId_ = qt::toQString(facts.repo_id);
    backend_ = facts.backend;

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 12, 10);
    layout->setSpacing(12);

    selectCheckbox_ = new QCheckBox;
    selectCheckbox_->setObjectName(QStringLiteral("modelSelectCheckbox"));
    selectCheckbox_->setToolTip(QStringLiteral("Select for batch download"));
    selectCheckbox_->setAccessibleName(QStringLiteral("Select %1 for download").arg(modelName));
    connect(selectCheckbox_, &QCheckBox::toggled, this,
            [this](bool checked) { Q_EMIT selectionToggled(modelName_, checked); });
    layout->addWidget(selectCheckbox_);

    auto* identity = new QVBoxLayout;
    identity->setSpacing(2);

    nameLabel_ = new ElidingLabel(qt::toQString(models::model_row_label(name)));
    nameLabel_->setToolTip(modelName);
    nameLabel_->setObjectName(QStringLiteral("modelRowName"));
    nameLabel_->setFont(segoe(10, true));
    identity->addWidget(nameLabel_);

    // Elides: this secondary line is long enough that a plain QLabel would raise the whole
    // window's minimum width past the list column.
    repoLabel_ = new ElidingLabel(qt::toQString(models::model_row_summary(name)));
    repoLabel_->setObjectName(QStringLiteral("modelRowSummary"));
    repoLabel_->setFont(segoe(8));
    repoLabel_->setToolTip(repoId_);
    identity->addWidget(repoLabel_);

    layout->addLayout(identity, 1);

    sizeLabel_ = new QLabel(QString());
    sizeLabel_->setObjectName(QStringLiteral("modelRowSize"));
    sizeLabel_->setFont(segoe(9));
    sizeLabel_->setMinimumWidth(72);
    sizeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(sizeLabel_);

    usageLabel_ = new QLabel(QString());
    usageLabel_->setObjectName(QStringLiteral("modelRowUsage"));
    usageLabel_->setAlignment(Qt::AlignCenter);
    usageLabel_->setFixedHeight(22);
    usageLabel_->setVisible(false);
    layout->addWidget(usageLabel_);

    badge_ = new QLabel(QString());
    badge_->setObjectName(QStringLiteral("modelRowBadge"));
    badge_->setAlignment(Qt::AlignCenter);
    badge_->setFixedHeight(22);
    layout->addWidget(badge_);

    downloadButton_ = new PrimaryButton(QStringLiteral("Download"));
    downloadButton_->setObjectName(QStringLiteral("modelDownloadButton"));
    compactButton(downloadButton_, 90);
    connect(downloadButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT downloadClicked(modelName_); });
    layout->addWidget(downloadButton_);

    setActiveButton_ = new Button(QStringLiteral("Set Active"));
    setActiveButton_->setObjectName(QStringLiteral("modelSetActiveButton"));
    compactButton(setActiveButton_, 90);
    connect(setActiveButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT setActiveClicked(modelName_); });
    layout->addWidget(setActiveButton_);

    deleteButton_ = new DangerButton(QStringLiteral("Delete"));
    deleteButton_->setObjectName(QStringLiteral("modelDeleteButton"));
    compactButton(deleteButton_, 72);
    connect(deleteButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT deleteClicked(modelName_); });
    layout->addWidget(deleteButton_);

    // An overlay, not a layout child: a hidden QProgressBar still inflates the catalog column's
    // minimum width past the Downloads viewport.
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("modelRowProgress"));
    progress_->setTextVisible(false);
    progress_->setRange(0, 0);
    progress_->hide();
}

void ModelRowWidget::updateState(const models::ModelRowInput& input) {
    updateState(models::model_row_state(qt::toUtf8(modelName_), input));
    // Python keeps the persisted selection here whether or not the files are on disk; only the
    // tint needs both.
    isCached_ = input.info.has_value();
    isActive_ = input.is_active;
}

void ModelRowWidget::updateState(const models::ModelRowState& state) {
    isCached_ = !state.download_visible;
    isActive_ = state.active_style;
    ui::setStyleProperty(this, ui::prop::active, state.active_style);

    sizeLabel_->setText(qt::toQString(state.size_text));
    ui::setStyleProperty(sizeLabel_, ui::prop::muted, state.size_muted);
    sortSizeBytes_ = state.sort_size_bytes;

    setBadge(qt::toQString(state.badge_text), qt::toQString(state.badge_tone));
    if (state.progress_visible) {
        showProgress();
    } else {
        hideProgress();
    }
    badge_->setVisible(state.badge_visible);

    selectCheckbox_->setVisible(state.checkbox_visible);
    selectCheckbox_->setEnabled(state.checkbox_enabled);
    downloadButton_->setVisible(state.download_visible);
    downloadButton_->setEnabled(state.download_enabled);
    downloadButton_->setToolTip(qt::toQString(state.download_tooltip));

    setActiveButton_->setVisible(state.set_active_visible);
    setActiveButton_->setEnabled(state.set_active_enabled);

    deleteButton_->setVisible(state.delete_visible);
    deleteButton_->setEnabled(state.delete_enabled);
    deleteButton_->setToolTip(qt::toQString(state.delete_tooltip));
}

void ModelRowWidget::showProgress() {
    progress_->show();
    placeProgress();
}

void ModelRowWidget::hideProgress() {
    progress_->hide();
    progress_->setValue(0);
}

void ModelRowWidget::placeProgress() {
    if (!progress_->isVisible()) {
        return;
    }
    const QMargins margins = layout()->contentsMargins();
    constexpr int kHeight = 6;
    progress_->setGeometry(margins.left(), height() - margins.bottom() - kHeight,
                           std::max(0, width() - margins.left() - margins.right()), kHeight);
}

void ModelRowWidget::resizeEvent(QResizeEvent* event) {
    DownloadRow::resizeEvent(event);
    placeProgress();
}

void ModelRowWidget::setProgress(std::int64_t done, std::int64_t total) {
    showProgress();
    const models::RowProgress progress = models::model_row_progress(done, total);
    if (progress.indeterminate) {
        progress_->setRange(0, 0);
        return;
    }
    progress_->setRange(0, 100);
    progress_->setValue(progress.percent);
    if (progress.size_text) {
        sizeLabel_->setText(qt::toQString(*progress.size_text));
    }
}

void ModelRowWidget::setUsage(const QString& text) {
    usageLabel_->setText(text);
    usageLabel_->setVisible(!text.isEmpty());
}

models::ModelRowFacts ModelRowWidget::facts() const {
    models::ModelRowFacts facts;
    facts.model_name = qt::toUtf8(modelName_);
    facts.repo_id = qt::toUtf8(repoId_);
    facts.backend = backend_;
    facts.is_cached = isCached_;
    facts.sort_size_bytes = sortSizeBytes_;
    return facts;
}

bool ModelRowWidget::matchesFilter(const QString& text) const {
    return models::matches_filter(qt::toUtf8(text), facts(), &fuzzy_match);
}

} // namespace fv
