#include "ui_meeting/after_end_page.h"

#include <stdexcept>
#include <utility>

#include <QComboBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "ui_meeting/dashboard_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace after_end {

std::string rail_value(const Json& settings) {
    if (keys::meeting_end_report.read(settings)) {
        return "Report";
    }
    if (keys::meeting_end_polish.read(settings)) {
        return "Polish";
    }
    if (keys::meeting_end_redecode.read(settings)) {
        return "Re-decode";
    }
    return "Off";
}

std::string review_destination(const llm::JudgeRoute& route) {
    if (route.is_local()) {
        return route.name + " on this computer";
    }
    if (route.profile.has_value()) {
        return route.name + " (" + route.host() + ")";
    }
    if (route == llm::typesafe_route()) {
        return "TypeSafe";
    }
    return "TypeSafe through " + route.name;
}

std::string review_confirm_text(const llm::JudgeRoute& route) {
    const std::string separate = route.profile.has_value()
                                     ? std::string()
                                     : "This is a separate service from your meeting LLM. ";
    return "For future meetings with AI insights on, send relevant transcript excerpts, speaker "
           "names, and generated insights to " +
           review_destination(route) + " after the meeting? " + separate +
           "No audio is sent. Review is optional and does not block saving your recording.";
}

} // namespace after_end

namespace {

QString q(std::string_view text) {
    return qt::toQString(text);
}

SettingsBinder::Resolve boolOf(const BoolSetting& setting) {
    return [&setting](const Json& settings) {
        return Json(setting.read(settings));
    };
}

bool askQuestion(QWidget* parent, const QString& title, const QString& text) {
    return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

SettingsPage& pageOf(QWidget* parent) {
    auto* page = qobject_cast<SettingsPage*>(parent);
    if (page == nullptr || page->dialog() == nullptr) {
        throw std::logic_error("The Meeting Mode pages are built inside the Settings window.");
    }
    return *page;
}

} // namespace

AfterEndPage::AfterEndPage(SettingsPage& page, SettingsDialog& dialog, Confirm confirm,
                           ReviewRoute reviewRoute)
    : QWidget(&page), dialog_(dialog), confirm_(std::move(confirm)),
      reviewRoute_(std::move(reviewRoute)) {
    if (!confirm_) {
        confirm_ = askQuestion;
    }
    if (!reviewRoute_) {
        reviewRoute_ = [this] {
            return llm::route_from_settings(*dialog_.settingsSnapshot());
        };
    }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    SettingsBinder& bindings = dialog_.meetingBindings();

    redecode_ = new SettingTile(
        QStringLiteral("Re-transcribe the full recording"),
        QStringLiteral("After End, recut the continuous session audio on longer quiet gaps and "
                       "run Parakeet again. Live capture is unchanged."),
        ui::designIcon(QStringLiteral("microphone-blue.svg")));
    bindings.checkbox(redecode_->checkbox(), std::string(keys::meeting_end_redecode.key()),
                      boolOf(keys::meeting_end_redecode));

    polish_ = new SettingTile(QStringLiteral("Clean up the transcript with the LLM"),
                              QStringLiteral("Rewrites the finished transcript for readability. "
                                             "Needs AI insights on for the meeting."),
                              ui::designIcon(QStringLiteral("stack-purple.svg")));
    bindings.checkbox(polish_->checkbox(), std::string(keys::meeting_end_polish.key()),
                      boolOf(keys::meeting_end_polish));

    report_ = new SettingTile(QStringLiteral("Write the final report"),
                              QStringLiteral("Topic, summary, and cards, generated once live "
                                             "captions finish. Needs AI insights on for the "
                                             "meeting."),
                              ui::designIcon(QStringLiteral("check-green.svg")));
    connect(report_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool checked) { onReportToggled(checked); });

    review_ = new SettingTile(
        QStringLiteral("Review uncertain insights at the end"),
        QStringLiteral("For new meetings. Gives the Fast judgments judge relevant excerpts, "
                       "speaker names and insights to find ambiguities, and asks the three "
                       "highest-priority questions first. No audio is sent. Needs AI insights "
                       "and Fast judgments."),
        ui::designIcon(QStringLiteral("check-green.svg")));
    review_->addTitleBadge(QStringLiteral("Experimental"));
    connect(review_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool checked) { onReviewToggled(checked); });
    auto* reviewRow = new QHBoxLayout();
    sensitivity_ = new ElidingComboBox();
    sensitivity_->addItem(QStringLiteral("Normal"), QStringLiteral("normal"));
    sensitivity_->addItem(QStringLiteral("Thorough"), QStringLiteral("thorough"));
    bindings.combo(sensitivity_, std::string(keys::meeting_insight_review_sensitivity.key()),
                   [](const Json& settings) {
                       return Json(keys::meeting_insight_review_sensitivity.read(settings));
                   });
    reviewRow->addWidget(settingsField(QStringLiteral("Sensitivity"), sensitivity_));
    review_->addBodyLayout(reviewRow);

    page.addTileGroup(layout, QStringLiteral("After End"), {redecode_, polish_, report_, review_},
                      2,
                      QStringLiteral("Live captions stay on short chunks so text appears "
                                     "quickly. These steps run afterward."));

    ribbon_ = new SettingTile(QStringLiteral("Ribbon"),
                              QStringLiteral("Timeline walk. Adds timeline beats and polished "
                                             "minutes, which is the main token cost."),
                              ui::designIcon(QStringLiteral("stack-slate.svg")));
    brief_ = new SettingTile(QStringLiteral("Brief"),
                             QStringLiteral("One-page editorial summary. Reuses the same cards as "
                                            "Signal."),
                             ui::designIcon(QStringLiteral("notes-blue.svg")));
    signal_ = new SettingTile(QStringLiteral("Signal"),
                              QStringLiteral("One-screen glance. Reuses the same cards as Brief."),
                              ui::designIcon(QStringLiteral("broadcast-green.svg")));
    const std::pair<SettingTile*, std::string_view> views[] = {
        {ribbon_, keys::meeting_report_ribbon.key()},
        {brief_, keys::meeting_report_brief.key()},
        {signal_, keys::meeting_report_signal.key()},
    };
    for (const auto& [tile, key] : views) {
        connect(tile->checkbox(), &QAbstractButton::toggled, this,
                [this, key](bool) { onViewToggled(key); });
    }

    const SettingsPage::TileGroup group =
        page.addTileGroup(layout, QStringLiteral("Report views"), {ribbon_, brief_, signal_}, 2,
                          QStringLiteral("Each enabled view is generated at End. At least one "
                                         "view stays on."));
    viewsTitle_ = group.caption;
    viewsInfo_ = group.intro;

    viewsHint_ = settingsCaption(q(after_end::kViewsHint));
    viewsHint_->hide();
    layout->addWidget(viewsHint_);
}

const std::vector<std::string>& AfterEndPage::controlNames() {
    static const std::vector<std::string> names{
        "meeting_end_redecode_tile",   "meeting_end_redecode_check", "meeting_end_polish_tile",
        "meeting_end_polish_check",    "meeting_end_report_tile",    "meeting_end_report_check",
        "meeting_review_tile",         "meeting_review_check",       "meeting_review_sensitivity",
        "meeting_report_ribbon_tile",  "meeting_report_brief_tile",  "meeting_report_signal_tile",
        "meeting_report_ribbon_check", "meeting_report_brief_check", "meeting_report_signal_check",
        "meeting_report_views_title",  "meeting_report_views_info",  "meeting_report_views_hint"};
    return names;
}

QWidget* AfterEndPage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"meeting_end_redecode_tile", redecode_},
        {"meeting_end_redecode_check", redecode_->checkbox()},
        {"meeting_end_polish_tile", polish_},
        {"meeting_end_polish_check", polish_->checkbox()},
        {"meeting_end_report_tile", report_},
        {"meeting_end_report_check", report_->checkbox()},
        {"meeting_review_tile", review_},
        {"meeting_review_check", review_->checkbox()},
        {"meeting_review_sensitivity", sensitivity_},
        {"meeting_report_ribbon_tile", ribbon_},
        {"meeting_report_brief_tile", brief_},
        {"meeting_report_signal_tile", signal_},
        {"meeting_report_ribbon_check", ribbon_->checkbox()},
        {"meeting_report_brief_check", brief_->checkbox()},
        {"meeting_report_signal_check", signal_->checkbox()},
        {"meeting_report_views_title", viewsTitle_},
        {"meeting_report_views_info", viewsInfo_},
        {"meeting_report_views_hint", viewsHint_},
    };
    for (const auto& [key, widget] : controls) {
        if (key == name) {
            return widget;
        }
    }
    return nullptr;
}

void AfterEndPage::loadSettings(const Json& settings) {
    const bool reviewOn =
        keys::meeting_insight_review.read(settings) &&
        keys::meeting_insight_review_consent.read(settings) == enums::kInsightReviewConsent;
    review_->checkbox()->setChecked(reviewOn);
    report_->checkbox()->setChecked(keys::meeting_end_report.read(settings));
    ribbon_->checkbox()->setChecked(keys::meeting_report_ribbon.read(settings));
    brief_->checkbox()->setChecked(keys::meeting_report_brief.read(settings));
    signal_->checkbox()->setChecked(keys::meeting_report_signal.read(settings));
    guardViews();
    updateViewsEnabled();
}

void AfterEndPage::onReviewToggled(bool checked) {
    if (dialog_.isLoading()) {
        return;
    }
    if (checked) {
        const llm::JudgeRoute route = reviewRoute_();
        if (!confirm_(&dialog_, q(after_end::kReviewConfirmTitle),
                      q(after_end::review_confirm_text(route)))) {
            const QSignalBlocker blocker(review_->checkbox());
            review_->checkbox()->setChecked(false);
            return;
        }
    }
    // One write, so the consent and the switch never disagree on disk.
    Json updates = Json::object();
    updates[std::string(keys::meeting_insight_review_consent.key())] =
        checked ? std::string(enums::kInsightReviewConsent) : std::string();
    updates[std::string(keys::meeting_insight_review.key())] = checked;
    dialog_.persistMany(updates);
}

void AfterEndPage::onReportToggled(bool checked) {
    updateViewsEnabled();
    dialog_.persist(keys::meeting_end_report.key(), checked);
}

void AfterEndPage::onViewToggled(std::string_view key) {
    guardViews();
    bool checked = false;
    if (key == keys::meeting_report_ribbon.key()) {
        checked = ribbon_->checkbox()->isChecked();
    } else if (key == keys::meeting_report_brief.key()) {
        checked = brief_->checkbox()->isChecked();
    } else {
        checked = signal_->checkbox()->isChecked();
    }
    dialog_.persist(key, checked);
}

void AfterEndPage::updateViewsEnabled() {
    const bool enabled = report_->checkbox()->isChecked();
    viewsTitle_->setEnabled(enabled);
    viewsInfo_->setEnabled(enabled);
    for (SettingTile* tile : {ribbon_, brief_, signal_}) {
        tile->setEnabled(enabled);
    }
    if (!enabled) {
        viewsHint_->hide();
    }
}

void AfterEndPage::guardViews() {
    if (ribbon_->checkbox()->isChecked() || brief_->checkbox()->isChecked() ||
        signal_->checkbox()->isChecked()) {
        viewsHint_->hide();
        return;
    }
    {
        const QSignalBlocker blocker(ribbon_->checkbox());
        ribbon_->checkbox()->setChecked(true);
    }
    viewsHint_->show();
}

void registerMeetingSettingsPages(AppContext& context) {
    SettingsPageRegistry& registry = context.settingsPages();
    {
        SettingsPageSpec spec;
        spec.destination = destinations::meeting_after;
        spec.title = QStringLiteral("After the meeting");
        spec.subtitle =
            QStringLiteral("Steps that run after End, once live captions have finished.");
        spec.create = [](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            return new AfterEndPage(page, *page.dialog());
        };
        spec.railValue = [](const Json& settings) {
            return q(after_end::rail_value(settings));
        };
        // PAGE_SEARCH_FIELDS['meeting_after'] (the review's search copy is not its tile's).
        spec.searchFields = {
            {"meeting_end_redecode_tile", QStringLiteral("Re-transcribe the full recording"),
             QStringLiteral("After End, recut the continuous session audio on longer quiet gaps "
                            "and run Parakeet again. Live capture is unchanged.")},
            {"meeting_end_polish_tile", QStringLiteral("Clean up the transcript with the LLM"),
             QStringLiteral("Rewrites the finished transcript for readability. Needs AI insights "
                            "on for the meeting.")},
            {"meeting_end_report_tile", QStringLiteral("Write the final report"),
             QStringLiteral("Topic, summary, and cards, generated once live captions finish. "
                            "Needs AI insights on for the meeting.")},
            {"meeting_review_tile", QStringLiteral("Review uncertain insights at the end"),
             QStringLiteral("Optional, for new meetings. Gives the Fast judgments judge relevant "
                            "transcript excerpts, speaker names, and insights to identify "
                            "ambiguities. Start with the three highest-priority questions, then "
                            "choose Review more to see the rest. No audio is sent. Requires AI "
                            "insights and Fast judgments on the Fast judgments page.")},
            {"meeting_report_ribbon_tile", QStringLiteral("Ribbon"),
             QStringLiteral("Timeline walk. Adds timeline beats and polished minutes, which is "
                            "the main token cost.")},
            {"meeting_report_brief_tile", QStringLiteral("Brief"),
             QStringLiteral("One-page editorial summary. Reuses the same cards as Signal.")},
            {"meeting_report_signal_tile", QStringLiteral("Signal"),
             QStringLiteral("One-screen glance. Reuses the same cards as Brief.")},
        };
        spec.controls = AfterEndPage::controlNames();
        registry.addPage(std::move(spec));
    }
    {
        SettingsPageSpec spec;
        spec.destination = destinations::meeting_dashboard;
        spec.title = QStringLiteral("Dashboard");
        spec.subtitle =
            QStringLiteral("Who can open the live meeting dashboard, and on which port.");
        spec.create = [](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            return new DashboardPage(page, *page.dialog());
        };
        spec.railValue = [](const Json& settings) {
            return q(dashboard::rail_value(settings));
        };
        spec.searchFields = {
            {"meeting_bind_tile", QStringLiteral("Who can open the dashboard"),
             QStringLiteral("Localhost keeps the live dashboard on this computer. Sharing serves "
                            "it to other devices on your local network.")},
            {"meeting_port_tile", QStringLiteral("Dashboard port"),
             QStringLiteral("Automatic lets the meeting server pick a free port each session. "
                            "Pick a fixed port only if you need a stable link.")},
        };
        spec.controls = DashboardPage::controlNames();
        registry.addPage(std::move(spec));
    }
}

} // namespace fv
