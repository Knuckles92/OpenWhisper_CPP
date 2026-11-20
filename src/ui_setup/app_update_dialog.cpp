#include "ui_setup/app_update_dialog.h"

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QTextDocument>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_setup/notes_view.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/release_notes.h"
#include "update/update_copy.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_setup.app_update_dialog"};

constexpr int kNotesMinHeight = 56;
constexpr int kNotesMaxHeight = 280;
constexpr int kButtonHeight = 40;
// Qt indents each list level by a full 40 px otherwise, which reads as a blockquote rather
// than a bullet list at this width.
constexpr int kListIndentWidth = 14;

constexpr std::array kPhases{
    update::DownloadPhase::Downloading, update::DownloadPhase::Verifying,
    update::DownloadPhase::Extracting,  update::DownloadPhase::Opening,
    update::DownloadPhase::Restarting,  update::DownloadPhase::RollingBack};

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

// Button defaults to a 140 px floor and measures its text only when the text is set again, so
// three buttons at once would overflow the dialog and clip the widest label.
void sizeFooterButton(Button* button) {
    button->setBaseMinimumSize(0, kButtonHeight);
}

} // namespace

AppUpdateDialog::AppUpdateDialog(std::optional<update::UpdateCheckResult> result, QString error,
                                 SettingsStore& settings, bool can_download, QWidget* parent)
    : QDialog(parent), result_(std::move(result)), error_(std::move(error)), settings_(settings),
      canDownload_(can_download) {
    setObjectName(QStringLiteral("appUpdateDialog"));
    setWindowTitle(QStringLiteral("%1 Updates").arg(text(std::string(identity().product))));
    setMinimumWidth(520);
    setModal(true);
    setupUi();
}

void AppUpdateDialog::setupUi() {
    const update::UpdateCheckResult* result = result_ ? &*result_ : nullptr;

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(10);

    titleLabel_ = new WrappedLabel(text(update::title_text(result)));
    titleLabel_->setObjectName(QStringLiteral("updateTitleLabel"));
    layout->addWidget(titleLabel_);

    bodyLabel_ = new WrappedLabel(text(update::body_text(result, qt::toUtf8(error_))));
    bodyLabel_->setObjectName(QStringLiteral("updateBodyLabel"));
    bodyLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (result == nullptr) {
        setBodyTone("error");
    }
    layout->addWidget(bodyLabel_);

    notesCard_ = buildNotesCard();
    layout->addWidget(notesCard_);

    const std::string hint = update::hint_text(result);
    hintLabel_ = new WrappedLabel(text(hint));
    hintLabel_->setObjectName(QStringLiteral("updateHintLabel"));
    hintLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (hint.empty()) {
        hintLabel_->hide();
    }
    layout->addWidget(hintLabel_);

    progressPanel_ = buildProgressPanel();
    layout->addWidget(progressPanel_);

    optionsWidget_ = new QWidget;
    optionsWidget_->setObjectName(QStringLiteral("updateOptions"));
    auto* options = new QVBoxLayout(optionsWidget_);
    options->setContentsMargins(0, 4, 0, 0);
    options->setSpacing(6);

    dontNotifyCheck_ = new QCheckBox(QStringLiteral("Don't notify me again"));
    dontNotifyCheck_->setObjectName(QStringLiteral("updateDontNotifyCheck"));
    dontNotifyCheck_->setChecked(!settings_.get(keys::update_notify_enabled));
    options->addWidget(dontNotifyCheck_);

    dontCheckCheck_ = new QCheckBox(QStringLiteral("Don't check automatically"));
    dontCheckCheck_->setObjectName(QStringLiteral("updateDontCheckCheck"));
    dontCheckCheck_->setChecked(!settings_.get(keys::update_check_enabled));
    options->addWidget(dontCheckCheck_);
    layout->addWidget(optionsWidget_);

    layout->addStretch(1);

    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("updateFooterDivider"));
    divider->setFixedHeight(1);
    layout->addWidget(divider);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 4, 0, 0);
    buttons->setSpacing(8);
    buttons->addStretch();

    setupButton_ = new Button(QStringLiteral("Use installer instead"));
    setupButton_->setObjectName(QStringLiteral("updateSetupButton"));
    sizeFooterButton(setupButton_);
    connect(setupButton_, &QPushButton::clicked, this, [this] { onUseSetup(); });
    setupButton_->hide();
    buttons->addWidget(setupButton_);

    laterButton_ = new Button(text(update::later_label(result)));
    laterButton_->setObjectName(QStringLiteral("updateLaterButton"));
    sizeFooterButton(laterButton_);
    connect(laterButton_, &QPushButton::clicked, this, [this] { onLater(); });
    buttons->addWidget(laterButton_);

    const std::string primary = update::primary_label(result);
    primaryButton_ = new PrimaryButton(text(primary));
    ui::setStyleProperty(primaryButton_, ui::prop::buttonId, QStringLiteral("updatePrimaryButton"));
    sizeFooterButton(primaryButton_);
    connect(primaryButton_, &QPushButton::clicked, this, [this] { onPrimary(); });
    if (primary.empty()) {
        primaryButton_->hide();
    } else {
        primaryButton_->setDefault(true);
    }
    buttons->addWidget(primaryButton_);

    layout->addLayout(buttons);
}

QFrame* AppUpdateDialog::buildNotesCard() {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("updateNotesCard"));
    auto* inner = new QVBoxLayout(card);
    inner->setContentsMargins(16, 12, 6, 12);
    inner->setSpacing(6);

    auto* caption = new QLabel(QStringLiteral("WHAT'S NEW"));
    caption->setObjectName(QStringLiteral("updateNotesCaption"));
    inner->addWidget(caption);

    notesView_ = new NotesView;
    notesView_->setObjectName(QStringLiteral("updateNotesView"));
    notesView_->setOpenExternalLinks(true);
    notesView_->setFrameShape(QFrame::NoFrame);
    notesView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    notesView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    notesView_->setFixedHeight(kNotesMinHeight);
    notesView_->document()->setIndentWidth(kListIndentWidth);
    inner->addWidget(notesView_);

    // The notes exist only for an offered release; notes that render to nothing hide the card.
    std::string html;
    if (result_ && result_->status == update::UpdateStatus::UpdateAvailable && result_->release) {
        const StyleTable style{ui::currentPalette().tokens()};
        html = render_release_notes_html(result_->release->notes, style);
    }
    hasNotes_ = !html.empty();
    if (hasNotes_) {
        notesView_->setHtml(text(html));
    } else {
        card->hide();
    }
    return card;
}

QFrame* AppUpdateDialog::buildProgressPanel() {
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("updateProgressPanel"));
    auto* inner = new QVBoxLayout(panel);
    inner->setContentsMargins(16, 14, 16, 14);
    inner->setSpacing(10);

    progressBar_ = new AnimatedProgressBar;
    inner->addWidget(progressBar_);

    auto* detailRow = new QHBoxLayout;
    detailRow->setContentsMargins(0, 0, 0, 0);
    detailRow->setSpacing(8);

    progressDetail_ = new QLabel(QString());
    progressDetail_->setObjectName(QStringLiteral("updateProgressDetail"));
    detailRow->addWidget(progressDetail_);
    detailRow->addStretch();

    progressPercent_ = new QLabel(QString());
    progressPercent_->setObjectName(QStringLiteral("updateProgressPercent"));
    detailRow->addWidget(progressPercent_);

    inner->addLayout(detailRow);
    panel->hide();
    return panel;
}

void AppUpdateDialog::onLater() {
    if (busy_) {
        onCancel();
        return;
    }
    resultAction_ = ResultAction::Later;
    persist();
    reject();
}

// A cancel can arrive after the worker's last cancellation point, so "canceling" must never
// leave the dialog with no working button: the second press dismisses it, and the download
// unwinds on its own once the window is gone.
void AppUpdateDialog::onCancel() {
    if (cancelRequested_) {
        busy_ = false;
        reject();
        return;
    }
    cancelRequested_ = true;
    Q_EMIT cancelRequested();
    bodyLabel_->setText(QStringLiteral("Canceling the update…"));
    progressPercent_->setText(QString());
    progressBar_->setIndeterminate();
    laterButton_->setText(QStringLiteral("Close"));
}

void AppUpdateDialog::onUseSetup() {
    if (!result_ || !canDownload_) {
        return;
    }
    resultAction_ = ResultAction::UseSetup;
    setupButton_->hide();
    setDownloading();
    Q_EMIT setupRequested();
}

void AppUpdateDialog::onPrimary() {
    resultAction_ = ResultAction::Primary;
    persist();
    if (result_ && result_->can_apply && canDownload_) {
        setDownloading();
        Q_EMIT downloadRequested();
        return;
    }
    accept();
}

void AppUpdateDialog::reject() {
    if (busy_) {
        onCancel();
        return;
    }
    if (resultAction_ != ResultAction::Primary && resultAction_ != ResultAction::UseSetup) {
        resultAction_ = ResultAction::Later;
        persist();
    }
    QDialog::reject();
}

void AppUpdateDialog::persist() {
    if (persisted_) {
        return;
    }
    persisted_ = true;
    try {
        update::persist_prompt_choices(settings_, !dontNotifyCheck_->isChecked(),
                                       !dontCheckCheck_->isChecked());
    } catch (const std::exception& error) {
        kLog.warning("Could not persist update preferences: {}", error.what());
    }
}

void AppUpdateDialog::setDownloading() {
    const update::UpdateCheckResult* result = result_ ? &*result_ : nullptr;
    busy_ = true;
    cancelRequested_ = false;
    primaryButton_->setEnabled(false);
    setupButton_->setEnabled(false);
    laterButton_->setEnabled(true);
    laterButton_->setText(QStringLiteral("Cancel"));
    dontNotifyCheck_->setEnabled(false);
    dontCheckCheck_->setEnabled(false);
    setBodyTone("");
    notesCard_->hide();
    hintLabel_->hide();
    optionsWidget_->hide();
    progressDetail_->setText(
        text(update::progress_detail_text(result, update::DownloadPhase::Downloading, 0, 0)));
    progressPercent_->setText(QString());
    progressBar_->setIndeterminate();
    progressPanel_->show();
    bodyLabel_->setText(text(*update::phase_text(update::DownloadPhase::Downloading)));
    resizeToContent();
}

void AppUpdateDialog::setProgress(const QString& phase, qint64 done, qint64 total) {
    // An unknown name behaves as "opening" does: no body line of its own, the release name as
    // the detail.
    update::DownloadPhase known = update::DownloadPhase::Opening;
    const std::string name = phase.toStdString();
    for (const auto candidate : kPhases) {
        if (update::phase_name(candidate) == name) {
            known = candidate;
            break;
        }
    }
    setProgress(known, done, total);
}

void AppUpdateDialog::setProgress(update::DownloadPhase phase, qint64 done, qint64 total) {
    const update::UpdateCheckResult* result = result_ ? &*result_ : nullptr;
    if (phase == update::DownloadPhase::Restarting) {
        markHandedOff();
    }
    progressPanel_->show();
    if (total > 0) {
        const double fraction =
            std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0);
        progressBar_->setFraction(fraction);
        progressPercent_->setText(QStringLiteral("%1%").arg(static_cast<int>(fraction * 100)));
    } else {
        progressBar_->setIndeterminate();
        progressPercent_->setText(QString());
    }
    progressDetail_->setText(text(update::progress_detail_text(result, phase, done, total)));
    if (const auto line = update::phase_text(phase)) {
        bodyLabel_->setText(text(*line));
    }
}

void AppUpdateDialog::markHandedOff() {
    busy_ = false;
    cancelRequested_ = false;
    laterButton_->setEnabled(false);
}

void AppUpdateDialog::setBodyTone(const char* tone) {
    ui::setStyleProperty(bodyLabel_, ui::prop::tone, QString::fromLatin1(tone));
}

void AppUpdateDialog::setError(const QString& message, bool offerSetup) {
    const update::UpdateCheckResult* result = result_ ? &*result_ : nullptr;
    busy_ = false;
    cancelRequested_ = false;
    // The choices are editable again, so a later close must save them.
    persisted_ = false;
    setBodyTone("error");
    bodyLabel_->setText(message.isEmpty() ? QStringLiteral("The download failed.") : message);
    progressPanel_->hide();
    progressBar_->reset();
    optionsWidget_->show();
    if (hasNotes_) {
        notesCard_->show();
    }
    if (!update::hint_text(result).empty()) {
        hintLabel_->show();
    }
    primaryButton_->setEnabled(true);
    laterButton_->setEnabled(true);
    laterButton_->setText(text(update::later_label(result)));
    dontNotifyCheck_->setEnabled(true);
    dontCheckCheck_->setEnabled(true);
    resultAction_ = ResultAction::Later;
    const bool canOfferSetup =
        offerSetup && result != nullptr && result->apply_mode == update::ApplyMode::Native &&
        result->release && result->release->setup_asset && result->release->setup_asset->sha256 &&
        !result->release->setup_asset->sha256->empty();
    setupButton_->setVisible(canOfferSetup);
    setupButton_->setEnabled(true);
    resizeToContent();
}

void AppUpdateDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    fitNotesHeight();
}

void AppUpdateDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    fitNotesHeight();
}

// A short changelog in a fixed-height box is mostly empty background, and a long one has to
// scroll somewhere, so the view tracks its own document height between a floor and a ceiling.
void AppUpdateDialog::fitNotesHeight() {
    if (notesCard_->isHidden()) {
        return;
    }
    QTextDocument* document = notesView_->document();
    const int width = notesView_->viewport()->width();
    if (width <= 0) {
        return;
    }
    document->setTextWidth(width);
    const int needed = static_cast<int>(document->size().height() + 2 * document->documentMargin());
    const int height = std::max(kNotesMinHeight, std::min(kNotesMaxHeight, needed));
    if (height != notesView_->height()) {
        notesView_->setFixedHeight(height);
    }
}

void AppUpdateDialog::resizeToContent() {
    if (!isVisible()) {
        return;
    }
    layout()->activate();
    const int width = std::max(this->width(), minimumSizeHint().width());
    resize(width, sizeHint().height());
}

} // namespace fv
