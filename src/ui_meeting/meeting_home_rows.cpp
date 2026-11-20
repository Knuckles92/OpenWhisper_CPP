#include "ui_meeting/meeting_home_rows.h"

#include <string>

#include <QPointer>
#include <QPushButton>
#include <QVariant>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "llm/profiles.h"
#include "models/meeting_speech.h"
#include "settings/keys.h"
#include "ui_downloads/model_assignments.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

QString q(std::string_view text) {
    return qt::toQString(text);
}

HomePage::Resolver boolOf(const BoolSetting& setting) {
    return [&setting](const Json& settings) {
        return QVariant(setting.read(settings));
    };
}

void addMeetingRows(HomePage& home) {
    // _page_row: a row whose button opens a page.
    auto* change = neutralButton(new Button(QStringLiteral("Change")));
    fitCompactButton(change);
    const QString destination = q(destinations::meeting_voice);
    SettingsDialog* dialog = &home.dialog();
    QObject::connect(change, &QPushButton::clicked, &home,
                     [dialog, destination] { dialog->selectDestination(destination); });
    QPointer<WrappedLabel> detail =
        home.addRow(HomeCardId::Meetings, QStringLiteral("Voice & speakers"), QString(), change);
    // Named for what it does, which the row title alone doesn't say.
    change->setAccessibleName(QStringLiteral("Change meeting voice and speakers"));
    home.addRefreshHook([detail](const Json& settings) {
        if (detail != nullptr) {
            detail->setText(
                q(meeting_model_label(settings) + " · " + meeting_voice_detail(settings)));
        }
    });

    home.toggle(HomeCardId::Meetings, QStringLiteral("Clean up the transcript"),
                QStringLiteral("Improve readability after the meeting. Requires AI insights."),
                std::string(destinations::meeting_after), "meeting_end_polish_check",
                boolOf(keys::meeting_end_polish), std::string(keys::meeting_end_polish.key()));
    home.toggle(HomeCardId::Meetings, QStringLiteral("Write a final report"),
                QStringLiteral("Create a summary and action items. Requires AI insights."),
                std::string(destinations::meeting_after), "meeting_end_report_check",
                boolOf(keys::meeting_end_report), std::string(keys::meeting_end_report.key()));

    auto* configure = neutralButton(new Button(QStringLiteral("Set up")));
    fitCompactButton(configure);
    QObject::connect(configure, &QPushButton::clicked, &home, [dialog] {
        dialog->selectDestination(q(destinations::meeting_intelligence));
    });
    QPointer<WrappedLabel> insights =
        home.addRow(HomeCardId::Meetings, QStringLiteral("AI insights"), QString(), configure);
    configure->setAccessibleName(QStringLiteral("Set up AI insights"));
    home.addRefreshHook([insights](const Json& settings) {
        if (insights) {
            insights->setText(q(meeting_intelligence_home_line(settings)));
        }
    });
}

} // namespace

std::vector<HomeWhereItem> meeting_where_items(const Json& settings) {
    std::vector<HomeWhereItem> items;
    if (meeting_preferred_host(settings)) {
        items.push_back(
            {HomeRows::kWhereMeetingVoice,
             QStringLiteral("Meeting voice (audio sent to paired computer when confirmed)"), true});
    } else {
        const bool needsHost = models::resolve_meeting_asr_source(settings) == "remote";
        items.push_back({HomeRows::kWhereMeetingVoice,
                         needsHost ? QStringLiteral("Meeting voice (confirm paired computer)")
                                   : QStringLiteral("Meeting voice"),
                         false});
    }
    if (speaker_id_is_remote(settings)) {
        items.push_back({HomeRows::kWhereSpeakerLabels,
                         QStringLiteral("Speaker labels (system audio after End)"), true});
    }
    // _provider_is_remote: a provider that cannot be looked up counts as leaving.
    const auto providerIsRemote = [&settings](const std::string& provider) {
        try {
            const auto profile = llm::get_profile(provider, settings);
            return !profile.has_value() || !profile->is_local();
        } catch (const std::exception&) {
            return true;
        }
    };
    const bool remote = meeting_intelligence_is_remote(settings, providerIsRemote);
    items.push_back({HomeRows::kWhereMeetingIntelligence,
                     remote ? QStringLiteral("Meeting intelligence (transcript text, when enabled)")
                            : QStringLiteral("Meeting intelligence"),
                     remote});
    return items;
}

void registerMeetingHomeRows(HomeRows& rows) {
    rows.addRows(HomeCardId::Meetings, addMeetingRows);
    rows.addWhereItems(meeting_where_items);
}

} // namespace fv
