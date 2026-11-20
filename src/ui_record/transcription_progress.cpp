#include "ui_record/transcription_progress.h"

#include <chrono>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_theme/style_props.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

using dictation::ProgressStage;
using dictation::StepState;

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

UploadStepChip::UploadStepChip(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    dot_ = new QLabel;
    dot_->setObjectName(QStringLiteral("uploadStepDot"));
    dot_->setFixedSize(8, 8);
    layout->addWidget(dot_);

    title_ = new QLabel(title);
    title_->setObjectName(QStringLiteral("uploadStepTitle"));
    layout->addWidget(title_);

    setState(StepState::Pending);
}

void UploadStepChip::setState(StepState state) {
    if (stateSet_ && state == state_) {
        return;
    }
    state_ = state;
    stateSet_ = true;
    for (QLabel* widget : {dot_, title_}) {
        ui::setStyleProperty(widget, ui::prop::stepState, fromView(dictation::to_string(state)));
    }
}

TranscriptionProgressPanel::TranscriptionProgressPanel(QWidget* parent)
    : QFrame(parent), now_(monotonicSeconds) {
    setObjectName(QStringLiteral("uploadProgressPanel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    titleLabel_ = new QLabel;
    titleLabel_->setObjectName(QStringLiteral("uploadProgressTitle"));
    header->addWidget(titleLabel_);

    batchLabel_ = new ElidingLabel;
    batchLabel_->setObjectName(QStringLiteral("uploadProgressBatch"));
    batchLabel_->hide();
    header->addWidget(batchLabel_, 1);

    elapsedLabel_ = new QLabel(QStringLiteral("0:00"));
    elapsedLabel_->setObjectName(QStringLiteral("uploadProgressElapsed"));
    elapsedLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(elapsedLabel_);

    stopButton_ = new QPushButton(QStringLiteral("Stop"));
    stopButton_->setObjectName(QStringLiteral("uploadStopButton"));
    stopButton_->setFlat(true);
    stopButton_->setCursor(Qt::PointingHandCursor);
    connect(stopButton_, &QPushButton::clicked, this, &TranscriptionProgressPanel::stopClicked);
    header->addWidget(stopButton_);
    layout->addLayout(header);

    bar_ = new AnimatedProgressBar(nullptr, 6);
    layout->addWidget(bar_);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);

    for (std::size_t index = 0; index < dictation::STEP_TITLES.size(); ++index) {
        if (index > 0) {
            auto* connector = new QFrame;
            connector->setObjectName(QStringLiteral("uploadStepConnector"));
            connector->setFixedSize(14, 1);
            footer->addWidget(connector);
            connectors_.append(connector);
        }
        auto* chip = new UploadStepChip(fromView(dictation::STEP_TITLES[index]));
        footer->addWidget(chip);
        steps_.append(chip);
    }

    footer->addSpacing(8);
    detailLabel_ = new ElidingLabel;
    detailLabel_->setObjectName(QStringLiteral("uploadProgressDetail"));
    detailLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footer->addWidget(detailLabel_, 1);
    layout->addLayout(footer);

    clock_ = new QTimer(this);
    clock_->setInterval(kElapsedTickMs);
    connect(clock_, &QTimer::timeout, this, [this] { tick(); });
}

bool TranscriptionProgressPanel::isRunning() const {
    return stage_.has_value() && !dictation::is_terminal(*stage_);
}

void TranscriptionProgressPanel::start(bool withCleanup, int totalFiles) {
    batchTotal_ = std::max(1, totalFiles);
    batchPosition_ = 1;
    const bool showCleanup = dictation::visible_steps(withCleanup) > 2;
    steps_[2]->setVisible(showCleanup);
    connectors_[1]->setVisible(showCleanup);
    startedAt_ = now_();
    elapsedLabel_->setText(fromView(dictation::format_elapsed(0)));
    detailLabel_->setText(QString());
    batchLabel_->setText(QString());
    batchLabel_->setVisible(isBatch());
    stopButton_->setEnabled(true);
    if (isBatch()) {
        bar_->setFraction(0.0, false);
    } else {
        bar_->setIndeterminate();
    }
    clock_->start();
    setStage(ProgressStage::Preparing);
}

void TranscriptionProgressPanel::setBatchPosition(int position, int total,
                                                  const QString& filename) {
    const dictation::BatchPosition clamped = dictation::clamp_batch_position(position, total);
    batchTotal_ = clamped.total;
    batchPosition_ = clamped.position;
    batchLabel_->setText(
        QStringLiteral(u"File %1 of %2  ·  %3").arg(position).arg(total).arg(filename));
    batchLabel_->setVisible(isBatch());
    bar_->setFraction(dictation::batch_fraction(batchPosition_, batchTotal_, 0.0));
}

void TranscriptionProgressPanel::setStage(ProgressStage stage,
                                          const std::optional<QString>& detail) {
    stage_ = stage;
    titleLabel_->setText(fromView(dictation::stage_title(stage)));
    ui::setStyleProperty(titleLabel_, ui::prop::stage, fromView(dictation::to_string(stage)));
    if (detail) {
        detailLabel_->setText(*detail);
    }
    applySteps(stage);

    if (dictation::is_terminal(stage)) {
        clock_->stop();
        tick();
        stopButton_->setEnabled(false);
        if (stage == ProgressStage::Done) {
            bar_->setFraction(1.0);
        } else {
            bar_->reset();
        }
    } else if (isBatch()) {
        bar_->setFraction(dictation::batch_fraction(batchPosition_, batchTotal_,
                                                    dictation::batch_stage_weight(stage)));
    } else if (!bar_->isIndeterminate()) {
        bar_->setIndeterminate();
    }
}

void TranscriptionProgressPanel::setDetail(const QString& text) {
    detailLabel_->setText(text);
}

void TranscriptionProgressPanel::setStopping() {
    stopButton_->setEnabled(false);
    detailLabel_->setText(QStringLiteral(u"Canceling…"));
}

void TranscriptionProgressPanel::setStopEnabled(bool enabled) {
    stopButton_->setEnabled(enabled);
}

void TranscriptionProgressPanel::setLargeFile(double fileSizeMb) {
    setStage(ProgressStage::Splitting, QStringLiteral("%1 MB file").arg(fileSizeMb, 0, 'f', 1));
}

bool TranscriptionProgressPanel::applyOverlayState(appcore::OverlayState state) {
    const std::optional<ProgressStage> mapped = dictation::stage_for_overlay_state(state);
    if (!mapped) {
        return false;
    }
    setStage(*mapped);
    return true;
}

void TranscriptionProgressPanel::finish(bool success) {
    setStage(success ? ProgressStage::Done : ProgressStage::Failed);
}

void TranscriptionProgressPanel::setClock(std::function<double()> now) {
    now_ = std::move(now);
}

void TranscriptionProgressPanel::tick() {
    if (!startedAt_) {
        return;
    }
    elapsedLabel_->setText(fromView(dictation::format_elapsed(now_() - *startedAt_)));
}

void TranscriptionProgressPanel::applySteps(ProgressStage stage) {
    dictation::StepStates current;
    for (std::size_t index = 0; index < current.size(); ++index) {
        current[index] = steps_[static_cast<qsizetype>(index)]->state();
    }
    const dictation::StepStates next = dictation::step_states(stage, current);
    for (std::size_t index = 0; index < next.size(); ++index) {
        steps_[static_cast<qsizetype>(index)]->setState(next[index]);
    }
}

void TranscriptionProgressPanel::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    clock_->stop();
}

void TranscriptionProgressPanel::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    if (isRunning()) {
        clock_->start();
    }
}

} // namespace fv
