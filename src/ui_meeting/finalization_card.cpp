#include "ui_meeting/finalization_card.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "ui_meeting/detail/payload.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

// The fonts OpenWhisper names outright in this card (the stylesheet may still set the size).
const QString kSegoeUi = QStringLiteral("Segoe UI");

// Stops a container from being compressed below its content height. The finalization boxes
// hold fixed-height rows, so the default shrinkable policy would let a short window squeeze
// them into overlapping text instead of reporting how much room they need.
void keepNaturalHeight(QWidget* widget) {
    widget->setSizePolicy(widget->sizePolicy().horizontalPolicy(), QSizePolicy::Minimum);
}

// A filled, initially hidden result action (meetingResultAction).
QPushButton* decisionButton(const QString& text, const QString& objectName,
                            const QString& tooltip) {
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    ui::setStyleProperty(button, ui::prop::meetingResultAction, true);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    button->hide();
    return button;
}

QFrame* divider() {
    auto* line = new QFrame();
    line->setObjectName(QStringLiteral("meetingFinalizationDivider"));
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

} // namespace

QPushButton* meetingLinkButton(const QString& text, const QString& objectName,
                               const QString& tooltip) {
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    ui::setStyleProperty(button, ui::prop::meetingLink, true);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    return button;
}

FinalizationCard::FinalizationCard(QWidget* parent) : Card(parent) {
    setObjectName(QStringLiteral("meetingFinalizationCard"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumHeight(0);
    keepNaturalHeight(this);
    ui::setStyleProperty(this, ui::prop::finalizationTone, QStringLiteral("neutral"));

    identityBox_ = new QWidget();
    identityBox_->setObjectName(QStringLiteral("meetingFinalizationIdentity"));
    auto* identityLayout = new QVBoxLayout(identityBox_);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(4);

    identityTitle_ = new WrappedLabel(QString(), nullptr, true);
    identityTitle_->setObjectName(QStringLiteral("meetingFinalizationIdentityTitle"));
    identityTitle_->setFont(QFont(kSegoeUi, 15, QFont::Bold));
    identityTitle_->setTextFormat(Qt::PlainText);
    identityLayout->addWidget(identityTitle_);

    auto* detailsRow = new QHBoxLayout();
    detailsRow->setSpacing(12);
    identityMeta_ = new WrappedLabel(QString());
    identityMeta_->setObjectName(QStringLiteral("meetingFinalizationIdentityMeta"));
    identityMeta_->setFont(QFont(kSegoeUi, 11));
    detailsRow->addWidget(identityMeta_, 1, Qt::AlignTop);

    auto* recordActions = new QVBoxLayout();
    recordActions->setSpacing(6);
    recordActions->setAlignment(Qt::AlignTop);
    reportButton_ = meetingLinkButton(QStringLiteral("Open report"),
                                      QStringLiteral("meetingFinalizationReportButton"),
                                      QStringLiteral("Open the final report in the web dashboard"));
    connect(reportButton_, &QPushButton::clicked, this, [this] { Q_EMIT openReportRequested(); });
    recordActions->addWidget(reportButton_, 0, Qt::AlignRight);
    retrySpeakersButton_ =
        meetingLinkButton(QStringLiteral("Re-run speakers"),
                          QStringLiteral("meetingFinalizationRetrySpeakersButton"));
    connect(retrySpeakersButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT retrySpeakersRequested(); });
    retrySpeakersButton_->hide();
    recordActions->addWidget(retrySpeakersButton_, 0, Qt::AlignRight);
    detailsRow->addLayout(recordActions);
    identityLayout->addLayout(detailsRow);
    cardLayout()->addWidget(identityBox_);

    cardLayout()->addWidget(divider());

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    title_ = new WrappedLabel(QStringLiteral("Final insights"));
    title_->setObjectName(QStringLiteral("meetingFinalizationTitle"));
    title_->setFont(QFont(kSegoeUi, 13, QFont::DemiBold));
    header->addWidget(title_, 1);

    stepBadge_ = new QLabel(QString());
    stepBadge_->setObjectName(QStringLiteral("meetingFinalizationStepBadge"));
    stepBadge_->setFont(QFont(kSegoeUi, 10, QFont::DemiBold));
    stepBadge_->setMinimumHeight(20);
    stepBadge_->hide();
    header->addWidget(stepBadge_);

    retryButton_ = decisionButton(QStringLiteral("Retry failed steps"),
                                  QStringLiteral("meetingFinalizationRetryButton"),
                                  QStringLiteral("Run every failed post-meeting step again."));
    connect(retryButton_, &QPushButton::clicked, this, [this] { Q_EMIT retryInsightsRequested(); });
    header->addWidget(retryButton_);
    cardLayout()->addLayout(header);

    progress_ = new QProgressBar();
    progress_->setObjectName(QStringLiteral("meetingFinalizationProgress"));
    progress_->setTextVisible(false);
    progress_->setAccessibleName(QStringLiteral("Meeting finalization progress"));
    progress_->setMaximumHeight(6);
    progress_->hide();
    cardLayout()->addWidget(progress_);

    activeBox_ = new QWidget();
    activeBox_->setObjectName(QStringLiteral("meetingFinalizationActiveBox"));
    auto* activeLayout = new QVBoxLayout(activeBox_);
    activeLayout->setContentsMargins(10, 8, 10, 8);
    activeLayout->setSpacing(4);
    message_ = new WrappedLabel(QString(), nullptr, true);
    message_->setObjectName(QStringLiteral("meetingFinalizationMessage"));
    message_->setAccessibleName(QStringLiteral("Meeting finalization status"));
    message_->setTextFormat(Qt::PlainText);
    activeLayout->addWidget(message_);
    detail_ = new WrappedLabel(QString());
    detail_->setObjectName(QStringLiteral("meetingFinalizationDetail"));
    activeLayout->addWidget(detail_);
    cardLayout()->addWidget(activeBox_);

    stepsWidget_ = new QWidget();
    stepsWidget_->setObjectName(QStringLiteral("meetingFinalizationStepsWidget"));
    stepsLayout_ = new QVBoxLayout(stepsWidget_);
    // The active box's inner padding, so both inset the same from the card: QSS padding does
    // not inset a plain widget's layout.
    stepsLayout_->setContentsMargins(10, 8, 10, 8);
    stepsLayout_->setSpacing(6);
    stepsWidget_->hide();
    keepNaturalHeight(stepsWidget_);
    cardLayout()->addWidget(stepsWidget_);

    // Secondary actions stay with the meeting record.
    footer_ = new QWidget();
    footer_->setObjectName(QStringLiteral("meetingFinalizationFooter"));
    auto* footerLayout = new QVBoxLayout(footer_);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(6);
    footerLayout->addWidget(divider());

    auto* actions = new QHBoxLayout();
    actions->setSpacing(16);
    backgroundButton_ = decisionButton(
        QStringLiteral("Continue in the background"),
        QStringLiteral("meetingFinalizationBackgroundButton"),
        QStringLiteral("Keep preparing this meeting in the background and return to Start "
                       "Meeting. Results will be available in Past Meetings."));
    connect(backgroundButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT backgroundRequested(); });
    actions->addWidget(backgroundButton_);
    keepLaterButton_ = decisionButton(
        QStringLiteral("Keep for later"), QStringLiteral("meetingFinalizationKeepLaterButton"),
        QStringLiteral("Hide this card and return to idle. The meeting stays in Past Meetings "
                       "until you open it again."));
    connect(keepLaterButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT deferInsightsRequested(); });
    actions->addWidget(keepLaterButton_);
    doneButton_ = decisionButton(
        QStringLiteral("Done"), QStringLiteral("meetingFinalizationDoneButton"),
        QStringLiteral("This meeting is already in Past Meetings. Hide this card and return to "
                       "idle."));
    connect(doneButton_, &QPushButton::clicked, this, [this] { Q_EMIT deferInsightsRequested(); });
    actions->addWidget(doneButton_);
    actions->addStretch();
    footerLayout->addLayout(actions);

    keepHint_ = new WrappedLabel(QString());
    keepHint_->setObjectName(QStringLiteral("meetingFinalizationKeepHint"));
    keepHint_->hide();
    footerLayout->addWidget(keepHint_);
    cardLayout()->addWidget(footer_);
}

void FinalizationCard::showIdentity(const QString& title, const QString& meta) {
    identityTitle_->setText(title.isEmpty() ? QStringLiteral("Meeting result") : title);
    identityMeta_->setText(meta);
    identityMeta_->setVisible(!meta.isEmpty());
}

void FinalizationCard::render(const meeting::FinalizationView& view) {
    // The background button is the layout state's; decide_card is asked without it.
    const meeting::CardDecision card = meeting::decide_card(view, false);
    const bool running = view.status == "running";

    reportButton_->setEnabled(card.report_enabled);
    reportButton_->setToolTip(qt::toQString(card.report_tip));
    title_->setText(qt::toQString(card.title));
    message_->setText(qt::toQString(card.message));
    setAccessibleName(title_->text());
    setAccessibleDescription(message_->text());
    keepHint_->setText(qt::toQString(card.keep_hint));
    retrySpeakersButton_->setToolTip(qt::toQString(card.rerun_speakers_tip));

    if (running) {
        progress_->show();
        retryButton_->hide();
        retrySpeakersButton_->hide();
        keepHint_->hide();
        keepLaterButton_->hide();
        doneButton_->hide();
        activeBox_->show();
        stepBadge_->setText(qt::toQString(card.step_badge));
        progress_->setRange(card.progress_minimum, card.progress_maximum);
        if (card.progress_maximum > card.progress_minimum) {
            progress_->setValue(card.progress_value);
        }
        stepBadge_->show();
    } else {
        stepBadge_->hide();
        progress_->hide();
        progress_->setRange(0, 1);
        progress_->setValue(0);
        retryButton_->setVisible(card.show_retry);
        if (card.show_retry) {
            retryButton_->setEnabled(true);
        }
        retrySpeakersButton_->setVisible(card.show_rerun_speakers);
        retrySpeakersButton_->setEnabled(card.rerun_speakers_enabled);
        keepHint_->setVisible(card.show_keep_later);
        keepLaterButton_->setVisible(card.show_keep_later);
        doneButton_->setVisible(card.show_done);
        activeBox_->setVisible(card.show_active_box);
    }
    // The current step's detail line, unless it repeats the message: its text changes only
    // when it shows.
    if (card.show_detail) {
        detail_->setText(qt::toQString(card.detail));
        detail_->show();
    } else {
        detail_->hide();
    }
    if (card.show_steps) {
        showSteps(view, card, !running);
        stepsWidget_->show();
    } else {
        stepsWidget_->hide();
    }

    ui::setStyleProperty(this, ui::prop::finalizationTone, qt::toQString(card.tone));
    // The title's colour follows its card's tone.
    ui::repolish(this);
    ui::repolish(title_);
}

void FinalizationCard::setBackgroundOffered(bool offered) {
    backgroundButton_->setVisible(offered);
}

void FinalizationCard::syncFooter(bool cardShown) {
    bool anyButton = false;
    for (const QPushButton* button : footer_->findChildren<QPushButton*>()) {
        anyButton = anyButton || !button->isHidden();
    }
    footer_->setVisible(cardShown && anyButton);
}

void FinalizationCard::showSteps(const meeting::FinalizationView& view,
                                 const meeting::CardDecision& card, bool allowActions) {
    // Every state update lands here. Rebuilding rows that would come out the same takes
    // keyboard focus off a Retry button for nothing.
    StepsShown shown;
    for (const Json& step : view.steps) {
        using ui_meeting::detail::get;
        using ui_meeting::detail::strOrEmpty;
        const bool hasStatus = step.is_object() && step.contains("status");
        shown.steps.emplace_back(strOrEmpty(get(step, "id")),
                                 hasStatus ? ui_meeting::detail::pyStr(step.at("status"))
                                           : std::string("pending"),
                                 strOrEmpty(get(step, "name")), strOrEmpty(get(step, "detail")));
    }
    shown.allowActions = allowActions;
    shown.speakerAllowed = card.rerun_speakers_enabled;
    shown.speakerTip = card.rerun_speakers_tip;
    if (stepsShown_ == shown) {
        return;
    }
    stepsShown_ = std::move(shown);
    while (stepsLayout_->count() > 0) {
        QLayoutItem* item = stepsLayout_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    for (const meeting::StepRow& step : card.steps) {
        addStepRow(step);
    }
}

void FinalizationCard::addStepRow(const meeting::StepRow& step) {
    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("meetingFinalizationStepRow"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 3, 0, 3);
    rowLayout->setSpacing(8);

    const QString name = qt::toQString(step.name);
    row->setAccessibleName(QStringLiteral("%1: %2").arg(name, qt::toQString(step.status)));
    row->setAccessibleDescription(qt::toQString(step.detail));

    auto* icon = new QLabel(qt::toQString(step.icon.glyph));
    icon->setObjectName(QStringLiteral("meetingFinalizationStepIcon"));
    icon->setFont(QFont(kSegoeUi, 11, QFont::Bold));
    ui::setLocalStyleSheet(icon, QStringLiteral("color: %1;").arg(qt::toQString(step.icon.color)));
    icon->setFixedWidth(16);
    rowLayout->addWidget(icon);

    auto* nameLabel = new WrappedLabel(name, nullptr, true);
    nameLabel->setObjectName(QStringLiteral("meetingFinalizationStepName"));
    nameLabel->setTextFormat(Qt::PlainText);
    QFont nameFont(kSegoeUi, 11);
    if (step.running) {
        nameFont.setWeight(QFont::DemiBold);
    }
    nameLabel->setFont(nameFont);
    rowLayout->addWidget(nameLabel, 1);

    auto* detailLabel = new QLabel(qt::toQString(step.detail_text));
    detailLabel->setObjectName(QStringLiteral("meetingFinalizationStepDetail"));
    detailLabel->setFont(QFont(kSegoeUi, 10));
    rowLayout->addWidget(detailLabel);

    if (!step.action_label.empty()) {
        const QString label = qt::toQString(step.action_label);
        auto* action = new QPushButton(label);
        action->setObjectName(QStringLiteral("meetingFinalizationStepAction"));
        action->setCursor(Qt::PointingHandCursor);
        action->setFlat(true);
        action->setAccessibleName(QStringLiteral("%1 %2").arg(label, name));
        const QString stepId = qt::toQString(step.id);
        connect(action, &QPushButton::clicked, this,
                [this, stepId] { Q_EMIT retryStepRequested(stepId); });
        if (!step.action_enabled) {
            action->setEnabled(false);
            action->setToolTip(qt::toQString(step.action_tooltip));
        }
        rowLayout->addWidget(action);
    }

    stepsLayout_->addWidget(row);
}

} // namespace fv
