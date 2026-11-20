#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <QString>

#include "meeting/finalization_view.h"
#include "ui_widgets/cards.h"

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;

namespace fv {

class WrappedLabel;

// The result pane's card (objectName meetingFinalizationCard): the meeting that just ended, its
// wrap-up steps and what to do next (meeting_mode_tab.py _build_finalization_card and
// _render_finalization). From the top: the identity (title, "date · duration", Open report and
// Re-run speakers), the heading with its step badge and Retry failed steps, the progress bar,
// the message box, the step checklist and the footer (Continue in the background, Keep for
// later, Done and the keep hint). What each payload shows is meeting::decide_card's; this
// applies it. Affinity: GUI thread.
class FinalizationCard : public Card {
    Q_OBJECT
public:
    explicit FinalizationCard(QWidget* parent = nullptr);

    // _render_identity's result: the title ("Meeting result" when empty) and the meta line,
    // hidden when empty.
    void showIdentity(const QString& title, const QString& meta);
    // _render_finalization: copy, tone, progress, step rows and the actions for view. Step rows
    // are rebuilt only when what they show changes, so keyboard focus stays on a row's action.
    void render(const meeting::FinalizationView& view);
    // Continue in the background: shown while the wrap-up runs and may leave the stage.
    void setBackgroundOffered(bool offered);
    // The footer shows while the card does and any of its buttons is not hidden.
    void syncFooter(bool cardShown);

    QWidget* identityBox() const { return identityBox_; }
    WrappedLabel* identityTitle() const { return identityTitle_; }
    WrappedLabel* identityMeta() const { return identityMeta_; }
    QPushButton* reportButton() const { return reportButton_; }
    QPushButton* retrySpeakersButton() const { return retrySpeakersButton_; }
    WrappedLabel* title() const { return title_; }
    QLabel* stepBadge() const { return stepBadge_; }
    QPushButton* retryButton() const { return retryButton_; }
    QProgressBar* progress() const { return progress_; }
    QWidget* activeBox() const { return activeBox_; }
    WrappedLabel* message() const { return message_; }
    WrappedLabel* detail() const { return detail_; }
    QWidget* stepsWidget() const { return stepsWidget_; }
    QVBoxLayout* stepsLayout() const { return stepsLayout_; }
    QWidget* footer() const { return footer_; }
    QPushButton* backgroundButton() const { return backgroundButton_; }
    QPushButton* keepLaterButton() const { return keepLaterButton_; }
    QPushButton* doneButton() const { return doneButton_; }
    WrappedLabel* keepHint() const { return keepHint_; }

Q_SIGNALS:
    void openReportRequested();
    void retrySpeakersRequested();
    void retryInsightsRequested();
    void retryStepRequested(const QString& stepId);
    void backgroundRequested();
    void deferInsightsRequested();

private:
    // (id, status, name, detail) of each step as the payload gives them, whether rows offer
    // actions, whether a speaker re-run may run, and its tip.
    using StepKey = std::tuple<std::string, std::string, std::string, std::string>;
    struct StepsShown {
        std::vector<StepKey> steps;
        bool allowActions = false;
        bool speakerAllowed = false;
        std::string speakerTip;

        bool operator==(const StepsShown&) const = default;
    };

    void showSteps(const meeting::FinalizationView& view, const meeting::CardDecision& card,
                   bool allowActions);
    void addStepRow(const meeting::StepRow& step);

    QWidget* identityBox_;
    WrappedLabel* identityTitle_;
    WrappedLabel* identityMeta_;
    QPushButton* reportButton_;
    QPushButton* retrySpeakersButton_;
    WrappedLabel* title_;
    QLabel* stepBadge_;
    QPushButton* retryButton_;
    QProgressBar* progress_;
    QWidget* activeBox_;
    WrappedLabel* message_;
    WrappedLabel* detail_;
    QWidget* stepsWidget_;
    QVBoxLayout* stepsLayout_;
    QWidget* footer_;
    QPushButton* backgroundButton_;
    QPushButton* keepLaterButton_;
    QPushButton* doneButton_;
    WrappedLabel* keepHint_;
    std::optional<StepsShown> stepsShown_;
};

// A flat text action that reads as a link (meetingLink), sized to its label.
QPushButton* meetingLinkButton(const QString& text, const QString& objectName,
                               const QString& tooltip = {});

} // namespace fv
