#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QLabel>
#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"
#include "llm/judge.h"

class QComboBox;

namespace fv {

class AppContext;
class SettingsDialog;
class SettingsPage;
class SettingTile;
class WrappedLabel;

// The words of Settings → Meeting Mode → After the meeting (settings_meeting_after.py and
// settings_dialog.py's _meeting_after_rail_value), without a widget.
// Affinity: thread-safe (pure functions).
namespace after_end {

inline constexpr std::string_view kReviewConfirmTitle = "Enable experimental insight review?";
inline constexpr std::string_view kViewsHint = "At least one view is required. Ribbon stays on.";

// _meeting_after_rail_value: "Report", "Polish", "Re-decode" or "Off", the first step on.
std::string rail_value(const Json& settings);

// Who the insight review sends excerpts to over `route`: "{name} on this computer" for a local
// route, "{name} ({host})" for a text model, "TypeSafe" for Jev through TypeSafe, else "TypeSafe
// through {name}".
std::string review_destination(const llm::JudgeRoute& route);

// The question asked before the review is switched on (QMessageBox.question's text).
std::string review_confirm_text(const llm::JudgeRoute& route);

} // namespace after_end

// Settings → Meeting Mode → After the meeting (settings_meeting_after.py MeetingAfterSection):
// the After End group (re-transcribe the recording, clean up the transcript, write the final
// report, review uncertain insights with its sensitivity) and the Report views group (Ribbon,
// Brief, Signal) with its hint.
//
// Re-transcribe, clean up and the sensitivity only save their key (the dialog's meeting binder).
// The report views are disabled while "Write the final report" is off; when the last one is
// switched off, Ribbon is put back on without a signal and the hint says so. The review switch
// is on only when the setting and its consent string agree; switching it on asks first, and the
// answer writes both keys in one update, so they never disagree on disk.
// Affinity: GUI thread.
class AfterEndPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // QMessageBox.question with Yes and No, No the default: true for Yes.
    using Confirm = std::function<bool(QWidget* parent, const QString& title, const QString& text)>;
    // The Fast judgments route the review would use (selected_route).
    using ReviewRoute = std::function<llm::JudgeRoute()>;

    // Unset hooks: a QMessageBox parented to the Settings window, and the route the settings
    // name (llm::route_from_settings of the dialog's snapshot).
    AfterEndPage(SettingsPage& page, SettingsDialog& dialog, Confirm confirm = {},
                 ReviewRoute reviewRoute = {});

    // Replace the hooks of a page built by the Settings window (a test answers the question).
    void setConfirm(Confirm confirm) { confirm_ = std::move(confirm); }
    void setReviewRoute(ReviewRoute route) { reviewRoute_ = std::move(route); }

    SettingTile* redecodeTile() const { return redecode_; }
    SettingTile* polishTile() const { return polish_; }
    SettingTile* reportTile() const { return report_; }
    SettingTile* reviewTile() const { return review_; }
    QComboBox* reviewSensitivity() const { return sensitivity_; }
    SettingTile* ribbonTile() const { return ribbon_; }
    SettingTile* briefTile() const { return brief_; }
    SettingTile* signalTile() const { return signal_; }
    QLabel* viewsTitle() const { return viewsTitle_; }
    WrappedLabel* viewsInfo() const { return viewsInfo_; }
    WrappedLabel* viewsHint() const { return viewsHint_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;

    // CONTROLS of settings_meeting_after.py.
    static const std::vector<std::string>& controlNames();

private:
    void onReviewToggled(bool checked);
    void onReportToggled(bool checked);
    void onViewToggled(std::string_view key);
    void updateViewsEnabled();
    void guardViews();

    SettingsDialog& dialog_;
    Confirm confirm_;
    ReviewRoute reviewRoute_;

    SettingTile* redecode_ = nullptr;
    SettingTile* polish_ = nullptr;
    SettingTile* report_ = nullptr;
    SettingTile* review_ = nullptr;
    QComboBox* sensitivity_ = nullptr;
    SettingTile* ribbon_ = nullptr;
    SettingTile* brief_ = nullptr;
    SettingTile* signal_ = nullptr;
    QLabel* viewsTitle_ = nullptr;
    WrappedLabel* viewsInfo_ = nullptr;
    WrappedLabel* viewsHint_ = nullptr;
};

// Registers Settings → Meeting Mode → After the meeting (destinations::meeting_after) and
// Dashboard (destinations::meeting_dashboard) with their rail values, search fields and control
// names. Throws std::invalid_argument when either destination is registered already.
// Affinity: GUI thread.
void registerMeetingSettingsPages(AppContext& context);

} // namespace fv
