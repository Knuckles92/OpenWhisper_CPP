#include "ui_meeting/meeting_presenter.h"

#include <algorithm>
#include <map>
#include <utility>

#include <QAbstractButton>
#include <QAction>
#include <QEvent>
#include <QJsonDocument>
#include <QMessageBox>
#include <QStringList>
#include <QTimer>

#include "appcore/activity.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "clipboard/temporary_clipboard.h"
#include "dictation/hotkey_runtime.h"
#include "history/calendar_source.h"
#include "meeting/platform_policy.h"
#include "meeting/runtime.h"
#include "meeting/setup_check.h"
#include "meeting_agent/agent_settings.h"
#include "models/meeting_speech.h"
#include "remote/pairing_settings.h"
#include "remote/record_sync_control.h"
#include "settings/keys.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_sidebar.h"
#include "ui_meeting/meeting_consent_dialog.h"
#include "ui_meeting/meeting_controls.h"
#include "ui_meeting/meeting_export_dialog.h"
#include "ui_meeting/meeting_intro_dialog.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_meeting/meeting_recovery_dialog.h"
#include "ui_meeting/meeting_setup_dialog.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_meeting/past_meetings_source.h"
#include "ui_meeting/recent_meeting_row.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/tabbed_content.h"

namespace fv {
namespace {

bool flag(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_boolean() && value->get<bool>();
}

QString field(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_string()
               ? qt::toQString(value->get_ref<const std::string&>())
               : QString();
}

// _start_remote_listing's merged(): this computer's meetings, those the host also keeps marked
// "Also on <host>", with the host's own (searched there with the same query) merged in by start
// time. A local meeting whose host copy is listed also learns that copy's pin and revision, which
// "Delete meeting copy" names; a host meeting without a pin cannot be fetched and is left out.
// Blocks on the host: io only.
std::vector<Json> mergeHostMeetings(history::RemoteRecordSource& records, std::vector<Json> local,
                                    const QString& query, int limit) {
    std::vector<std::string> ids;
    for (const auto& meeting : local) {
        ids.push_back(qt::toUtf8(field(meeting, "id")));
    }
    const auto copies = records.copies_on_host("meeting", ids);
    auto items = records.list_remote("meeting", qt::toUtf8(query), limit);
    std::string host;
    for (const auto& item : items) {
        if (host = qt::toUtf8(field(item, "stored_on")); !host.empty()) {
            break;
        }
    }
    if (host.empty()) {
        try {
            host = records.host_name();
        } catch (const std::exception&) {
            // The name only labels the pill.
        }
    }
    std::map<QString, std::size_t> localById;
    for (std::size_t i = 0; i < local.size(); ++i) {
        localById.try_emplace(field(local[i], "id"), i);
        if (std::ranges::find(copies, ids[i]) != copies.end()) {
            local[i]["also_on"] = host.empty() ? std::string("the host") : host;
        }
    }
    std::vector<Json> merged = std::move(local);
    for (auto& item : items) {
        const auto pin = field(item, "_stored_on_fingerprint");
        if (pin.isEmpty()) {
            continue;
        }
        if (const auto matching = localById.find(field(item, "id")); matching != localById.end()) {
            auto& entry = merged[matching->second];
            if (const auto name = field(item, "stored_on"), revision = field(item, "revision");
                !name.isEmpty() && !revision.isEmpty()) {
                entry["_host_copy_name"] = qt::toUtf8(name);
                entry["_host_copy_title"] = qt::toUtf8(field(item, "title"));
                entry["_host_copy_fingerprint"] = qt::toUtf8(pin);
                entry["_host_copy_revision"] = qt::toUtf8(revision);
            }
            continue;
        }
        merged.push_back(std::move(item));
    }
    std::ranges::stable_sort(merged, [](const Json& lhs, const Json& rhs) {
        return field(lhs, "started_at") > field(rhs, "started_at");
    });
    return merged;
}

} // namespace

MeetingPresenter::MeetingPresenter(AppContext& context, MainWindow& window,
                                   meeting::MeetingRuntime& runtime, meeting::SetupDeps setup,
                                   std::shared_ptr<meeting_state::SqlMeetingRepository> repository,
                                   std::filesystem::path root,
                                   std::shared_ptr<MeetingConsentGrants> grants)
    : MeetingPresenter(context, window, runtimeControls(runtime), std::move(setup),
                       std::move(repository), std::move(root), std::move(grants)) {}

MeetingPresenter::MeetingPresenter(AppContext& context, MainWindow& window,
                                   std::shared_ptr<MeetingControls> controls,
                                   meeting::SetupDeps setup,
                                   std::shared_ptr<meeting_state::SqlMeetingRepository> repository,
                                   std::filesystem::path root,
                                   std::shared_ptr<MeetingConsentGrants> grants, Hooks hooks)
    : QObject(&window), context_(context), window_(window), controls_(std::move(controls)),
      runtime_(*controls_), hooks_(std::move(hooks)), setup_(std::move(setup)),
      repository_(std::move(repository)), root_(std::move(root)), grants_(std::move(grants)) {}

const meeting::PlatformInfo& MeetingPresenter::platform() const {
    return hooks_.platform ? *hooks_.platform : meeting::current_platform();
}

void MeetingPresenter::install() {
    context_.screens().define(screen_key::meeting, order::tab::meeting_mode,
                              QStringLiteral("Meeting Mode"),
                              [this](QWidget* parent) { return buildTab(parent); });
    window_.installEventFilter(this);
    if (auto* tabs = window_.tabbedContent()) {
        tabs->setMeetingTabLocked(
            !meeting::meeting_mode_supported(platform()) &&
                !context_.settings().get(keys::meeting_unsupported_platform_ack),
            qt::toQString(meeting::meeting_unsupported_os_name(platform().platform)));
        connect(tabs, &TabbedContentWidget::lockedMeetingTabActivated, this, [this, tabs] {
            if (acknowledgePlatform()) {
                tabs->unlockMeetingTab();
                tabs->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
            }
        });
    }
    connect(&window_, &MainWindow::meetingDashboardRequested, this,
            [this] { runtime_.open_dashboard(); });
    connect(&window_, &MainWindow::pastMeetingRequested, this,
            [this](const QString& id) { runtime_.show_past_meeting(qt::toUtf8(id)); });
    bindHistory();
    bindRuntime();
    if (grants_) {
        runtime_.set_consent_gate([presenter = QPointer<MeetingPresenter>(this)](
                                      bool cloud, std::function<void(bool)> answer) {
            if (!cloud || !presenter) {
                return false;
            }
            qt::post(presenter.data(), [presenter, answer = std::move(answer)] {
                if (presenter) {
                    answer(presenter->askIntelligenceConsent());
                }
            });
            return true;
        });
    }
}

QWidget* MeetingPresenter::buildTab(QWidget* parent) {
    MeetingModeTabOptions options;
    options.settings = *context_.settings().snapshot();
    options.settings[std::string(keys::meeting_cloud_last_enabled.key())] = false;
    options.clock = &context_.clock();
    options.platform = platform();
    options.speechNote = qt::toQString(models::resolve_meeting_speech(options.settings).note);
    auto* tab = new MeetingModeTab(options, parent);
    tab_ = tab;
    tab->setAiInsightsAvailable(grants_ != nullptr);
    refreshSpeechDestination();
    refreshIntelligenceDestination();
    connect(tab, &MeetingModeTab::aiDestinationWanted, this, [this] {
        refreshSpeechDestination();
        refreshIntelligenceDestination();
    });
    subscriptions_.push_back(context_.settings().watch(
        {keys::selected_model.key(), keys::local_asr_models.key(),
         keys::meeting_speech_separate.key(), keys::meeting_asr_model.key(),
         keys::meeting_asr_source.key(), keys::developer_mode.key()},
        context_.ui(), [this, tab = QPointer<MeetingModeTab>(tab)](const Json&) {
            if (tab) {
                refreshSpeechDestination();
                tab->setDeveloperMode(keys::developer_mode.read(*context_.settings().snapshot()));
            }
        }));
    tab->setRecentMeetingsReader(
        context_.io(), [repository = repository_] { return readRecentMeetings(*repository); });
    tab->setMeetingState(state_);
    tab->setStatusText(status_);
    tab->installEventFilter(this);
    bindTab(*tab);
    return tab;
}

void MeetingPresenter::bindTab(MeetingModeTab& tab) {
    connect(&tab, &MeetingModeTab::startRequested, this,
            [this](bool cloud, const QString& brief) { start(brief, cloud); });
    connect(&tab, &MeetingModeTab::setupRequested, this, [this] {
        if (acknowledgePlatform()) {
            runSetup(false);
        }
    });
    // Demo mode seeds canned transcript data and never opens capture: the platform disclosure
    // stays, but audio is neither probed nor remediated.
    connect(&tab, &MeetingModeTab::demoRequested, this, [this](bool) {
        if (acknowledgePlatform()) {
            runtime_.start_demo_meeting(false, "disabled");
        }
    });
    connect(&tab, &MeetingModeTab::pauseRequested, this, [this] { runtime_.pause(); });
    connect(&tab, &MeetingModeTab::resumeRequested, this, [this] { runtime_.resume(); });
    connect(&tab, &MeetingModeTab::endRequested, this, [this] { end(); });
    connect(&tab, &MeetingModeTab::openDashboardRequested, this,
            [this] { runtime_.open_dashboard(); });
    connect(&tab, &MeetingModeTab::openReportRequested, this, [this] { runtime_.open_report(); });
    connect(&tab, &MeetingModeTab::copyGuestLinkRequested, this,
            [this] { runtime_.copy_guest_link(); });
    connect(&tab, &MeetingModeTab::cloudToggled, this,
            [this](bool enabled) { toggleLiveIntelligence(enabled); });
    connect(&tab, &MeetingModeTab::aiSettingsRequested, this, [this] {
        Q_EMIT window_.settingsDestinationRequested(
            qt::toQString(destinations::meeting_intelligence));
    });
    connect(&tab, &MeetingModeTab::retryInsightsRequested, this,
            [this] { runtime_.retry_insights(); });
    connect(&tab, &MeetingModeTab::retrySpeakersRequested, this,
            [this] { runtime_.retry_speakers(); });
    connect(&tab, &MeetingModeTab::retryStepRequested, this,
            [this](const QString& step) { runtime_.retry_step(qt::toUtf8(step)); });
    connect(&tab, &MeetingModeTab::backgroundRequested, this,
            [this] { runtime_.continue_in_background(); });
    connect(&tab, &MeetingModeTab::deferInsightsRequested, this,
            [this] { runtime_.defer_finalization_card(); });
    connect(&tab, &MeetingModeTab::pastMeetingRequested, this,
            [this](const QString& id) { runtime_.show_past_meeting(qt::toUtf8(id)); });
}

void MeetingPresenter::bindRuntime() {
    auto& ui = context_.ui();
    const auto events = runtime_.events();
    subscriptions_.push_back(
        events.playback_stop_requested.connect(ui, [](std::string) { playback::stopAll(); }));
    subscriptions_.push_back(events.meeting_state_changed.connect(
        ui, [this](Json patch) { applyState(std::move(patch)); }));
    subscriptions_.push_back(events.meeting_status_update.connect(
        ui, [this](std::string text) { status(qt::toQString(text)); }));
    subscriptions_.push_back(events.status_update.connect(
        ui, [this](std::string text) { window_.setStatus(qt::toQString(text)); }));
    subscriptions_.push_back(events.meeting_error.connect(ui, [this](std::string error) {
        const QString text = error.empty() ? QStringLiteral("Meeting error") : qt::toQString(error);
        status(text);
        if (auto* lifecycle = context_.find<ShellLifecycle>();
            lifecycle != nullptr && lifecycle->offerRestartForFailure &&
            lifecycle->offerRestartForFailure(text)) {
            return;
        }
        auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Meeting Error"), text,
                                    QMessageBox::Ok, &window_);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    }));
    subscriptions_.push_back(events.meeting_server_started.connect(ui, [this](Json result) {
        updateDashboard(!field(result, "host_url").isEmpty() || !field(result, "url").isEmpty());
        runtime_.open_dashboard();
    }));
    subscriptions_.push_back(events.meeting_recovery_found.connect(
        ui, [this](std::vector<Json> rows) { showRecovery(rows); }));
    subscriptions_.push_back(events.meeting_recovery_scan_failed.connect(
        ui, [this](std::string error) { showScanError(qt::toQString(error)); }));
    subscriptions_.push_back(events.meeting_guest_link_ready.connect(
        ui, [this](std::string link) { copy(link, QStringLiteral("Guest link copied")); }));
    // The hotkey's start runs the same readiness gates once (_on_platform_ack_requested).
    subscriptions_.push_back(
        events.meeting_platform_ack_requested.connect(ui, [this] { start(); }));
    if (auto* history = context_.find<HistoryPanel>()) {
        subscriptions_.push_back(events.past_meetings_refresh_requested.connect(
            ui, [history] { history->refreshPastMeetings(); }));
    }
    if (auto* hotkeys = context_.find<dictation::HotkeyRuntime>()) {
        subscriptions_.push_back(hotkeys->meeting_toggle_requested.connect(
            ui, [this] { runtime_.toggle_from_hotkey(); }));
    }
}

void MeetingPresenter::bindHistory() {
    auto* history = context_.find<HistoryPanel>();
    if (history == nullptr || history->sidebar() == nullptr) {
        return;
    }
    auto source = std::make_shared<SqlPastMeetingsSource>(repository_);
    auto* remoteRecords = context_.find<history::RemoteRecordSource>();
    auto* actions = context_.find<remote::RecordSyncControl>();
    constexpr auto key = "past_meetings";
    context_.screens().addSidePanel(
        {order::side_panel::past_meetings, QStringLiteral("Past Meetings"),
         [this, source, remoteRecords](QWidget* parent) {
             PastMeetingsOptions options;
             options.settings = &context_.settings();
             options.clock = &context_.clock();
             if (remoteRecords) {
                 options.hostWanted = [remoteRecords] {
                     return remoteRecords->listing_wanted("meeting");
                 };
                 options.hostMerge = [remoteRecords](std::vector<Json> local, const QString& query,
                                                     int limit) {
                     return mergeHostMeetings(*remoteRecords, std::move(local), query, limit);
                 };
             }
             return new PastMeetingsPanel(pastMeetingsProvider(source), context_.io(), options,
                                          parent);
         },
         key});
    panel_ = qobject_cast<PastMeetingsPanel*>(context_.screens().ensure(key, history->sidebar()));
    history->sidebar()->setPageWidget(HistorySidebar::Page::Meetings, panel_);
    connect(panel_, &PastMeetingsPanel::meetingSelected, this,
            [this](const QString& id) { runtime_.show_past_meeting(qt::toUtf8(id)); });
    panel_->setSpeakerUploadAvailable(runtime_.can_offer_speaker_upload());
    connect(panel_, &PastMeetingsPanel::speakerUploadRequested, this,
            [this](const QString& id) { identifySavedSpeakers(id); });
    connect(panel_, &PastMeetingsPanel::copyTranscriptRequested, this,
            [this](const QString& id) { copyPastMeeting(id); });
    connect(panel_, &PastMeetingsPanel::deleteMeetingRequested, this,
            [this](const QString& id, bool audio) {
                runtime_.delete_past_meeting(qt::toUtf8(id), audio);
            });
    connect(panel_, &PastMeetingsPanel::clearMeetingsRequested, this,
            [this](bool audio) { runtime_.clear_past_meetings(audio); });
    if (actions) {
        connect(panel_, &PastMeetingsPanel::hostCopyDeleteRequested, this,
                [this, actions](const QString& id, const QString& pin, const QString& revision) {
                    QPointer<PastMeetingsPanel> guard(panel_);
                    context_.io().post([actions, guard, ui = &context_.ui(), id, pin, revision] {
                        QString error;
                        try {
                            actions->delete_remote_version("meeting", qt::toUtf8(id),
                                                           qt::toUtf8(pin), qt::toUtf8(revision));
                        } catch (const std::exception& problem) {
                            error = qt::toQString(problem.what());
                        }
                        ui->post([guard, id, error] {
                            if (guard) {
                                guard->onHostFetchDone(id, QStringLiteral("delete"), error);
                            }
                        });
                    });
                });
        connect(panel_, &PastMeetingsPanel::hostFetchRequested, this,
                [this, actions](const QString& id, const QString& host, const QString& then) {
                    const auto pin = panel_->pairedHostFingerprint(id);
                    QPointer<PastMeetingsPanel> guard(panel_);
                    context_.io().post([actions, guard, ui = &context_.ui(), id, host, then, pin] {
                        QString error;
                        try {
                            if (then == QStringLiteral("delete")) {
                                (void)actions->delete_remote_from("meeting", qt::toUtf8(id), pin);
                            } else {
                                const auto progress = [guard, ui, id, host](std::int64_t got,
                                                                            std::int64_t total) {
                                    if (total == 0) {
                                        return;
                                    }
                                    const auto text = QStringLiteral("Getting it from %1… %2%")
                                                          .arg(host)
                                                          .arg(got * 100 / total);
                                    ui->post([guard, id, text] {
                                        if (guard) {
                                            guard->onHostFetchProgress(id, text);
                                        }
                                    });
                                };
                                actions->check_out_from("meeting", qt::toUtf8(id), pin, progress);
                            }
                        } catch (const std::exception& problem) {
                            error = qt::toQString(problem.what());
                        }
                        ui->post([guard, id, then, error] {
                            if (guard) {
                                const auto action =
                                    then == QStringLiteral("open") && !guard->isVisible()
                                        ? QStringLiteral("copied")
                                        : then;
                                guard->onHostFetchDone(id, action, error);
                            }
                        });
                    });
                });
    }
    connect(panel_, &PastMeetingsPanel::calendarRequested, history, &HistoryPanel::openCalendar);
    connect(panel_, &PastMeetingsPanel::openMeetingsFolderRequested, this,
            [this] { openMeetingsFolder(root_); });
    connect(panel_, &PastMeetingsPanel::exportRequested, this, [this] { exportPastMeetings(); });
}

void MeetingPresenter::attachTray(SystemTray& tray) {
    tray_ = &tray;
    tray.setMeetingActive(flag(state_, "active"), flag(state_, "dashboard_available"));
    connect(&tray, &SystemTray::meetingToggleRequested, this, [this] {
        if (runtime_.is_active()) {
            end();
        } else {
            start();
        }
    });
    connect(&tray, &SystemTray::meetingDashboardRequested, this,
            [this] { runtime_.open_dashboard(); });
}

bool MeetingPresenter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show && (watched == &window_ || watched == tab_.data())) {
        scheduleIntro();
    }
    return QObject::eventFilter(watched, event);
}

void MeetingPresenter::scheduleIntro() {
    if (introScheduled_) {
        return;
    }
    introScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        introScheduled_ = false;
        if (!window_.isVisible() || !tab_ || !tab_->isVisible() || runtime_.is_claimed() ||
            window_.tabbedContent()->meetingTabIsLocked() || setupOpen_) {
            return;
        }
        maybeShowMeetingModeIntro(context_.settings(), &window_, hooks_.runIntro);
    });
}

bool MeetingPresenter::acknowledgePlatform() {
    if (meeting::meeting_mode_supported(platform()) ||
        context_.settings().get(keys::meeting_unsupported_platform_ack)) {
        return true;
    }
    const QString platform =
        qt::toQString(meeting::meeting_unsupported_os_name(this->platform().platform));
    if (QMessageBox::question(
            &window_, QStringLiteral("Meeting Mode"),
            QStringLiteral("Meeting Mode is not officially supported on %1. Continue anyway?")
                .arg(platform),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return false;
    }
    try {
        context_.settings().save_setting(keys::meeting_unsupported_platform_ack.key(), true);
    } catch (const std::exception&) {
        status(QStringLiteral("Could not save the Meeting Mode preference."));
    }
    return true;
}

std::optional<std::string> MeetingPresenter::runSetup(bool starting) {
    if (setupOpen_ || runtime_.is_claimed()) {
        return std::nullopt;
    }
    if (window_.isRecording()) {
        status(QStringLiteral("Finish Quick Record before checking meeting audio."));
        return std::nullopt;
    }
    struct ResetSetup {
        ActivityState& activity;
        bool& open;
        ~ResetSetup() {
            open = false;
            activity.set_meeting_setup_open(false);
        }
    };
    auto& activity = context_.activity();
    setupOpen_ = true;
    ResetSetup reset{activity, setupOpen_};
    activity.set_meeting_setup_open(true);
    if (hooks_.openSetup) {
        return hooks_.openSetup(starting);
    }
    MeetingSetupDialog::Hooks hooks;
    hooks.settings = &context_.settings();
    hooks.worker = &context_.io();
    MeetingSetupDialog dialog(setup_, context_.audio(), starting, &window_, std::move(hooks));
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return qt::toUtf8(dialog.systemAudioPolicy());
}

void MeetingPresenter::start(const QString& brief, bool cloud) {
    if (setupOpen_ || runtime_.is_claimed() || !acknowledgePlatform()) {
        return;
    }
    if (auto* tabs = window_.tabbedContent()) {
        tabs->unlockMeetingTab();
    }
    // ensure_meeting_start_readiness: the first start runs the shared setup, whose answer is the
    // session's system-audio policy. Windows needs no further grant or remediation.
    std::string policy = "auto";
    if (meeting::meeting_setup_available(platform()) &&
        !meeting::setup_complete(*context_.settings().snapshot())) {
        auto setupPolicy = runSetup(true);
        if (!setupPolicy) {
            return;
        }
        policy = std::move(*setupPolicy);
    }
    // Busy elsewhere: the runtime refuses the start with its own message, before any consent.
    const auto activity = context_.activity().snapshot();
    if (activity.recording || activity.transcribing || activity.backup_in_progress ||
        activity.host_update_active) {
        runtime_.start_meeting(false, std::move(policy), qt::toUtf8(brief));
        return;
    }
    runtime_.start_meeting(cloud && grants_ != nullptr, std::move(policy), qt::toUtf8(brief));
}

bool MeetingPresenter::askIntelligenceConsent() {
    try {
        const auto route =
            meeting_agent::resolve_meeting_agent_route(*context_.settings().snapshot());
        if (!askIntelligenceConsent(route)) {
            return false;
        }
        runtime_.set_start_intelligence_approval({route, askBackupConsent(route)});
        return true;
    } catch (const std::exception&) {
        status(QStringLiteral("Could not read the selected meeting insights destination."));
        return false;
    }
}

void MeetingPresenter::toggleLiveIntelligence(bool enabled) {
    if (!grants_ || !runtime_.is_active()) {
        return;
    }
    const auto actual = state_.value("cloud_enabled", false);
    if (actual == enabled) {
        return;
    }
    if (tab_) {
        tab_->setMeetingState(Json{{"cloud_enabled", actual}});
    }
    try {
        const auto meeting = state_.find("meeting_id");
        if (meeting == state_.end() || !meeting->is_string()) {
            return;
        }
        const auto id = meeting->get<std::string>();
        if (!enabled) {
            runtime_.set_live_intelligence(id, std::nullopt);
        } else if (const auto destination = runtime_.live_intelligence_destination();
                   destination && destination->first == id &&
                   askIntelligenceConsent(destination->second)) {
            runtime_.set_live_intelligence(
                destination->first,
                meeting::LiveIntelligenceApproval{destination->second,
                                                  askBackupConsent(destination->second)});
        }
    } catch (const std::exception&) {
        status(QStringLiteral("Could not change AI insights for this meeting."));
    }
}

bool MeetingPresenter::askIntelligenceConsent(const meeting_agent::MeetingAgentRoute& route) {
    try {
        auto& settings = context_.settings();
        MeetingConsentDialog dialog(meetingConsentCopy(route, *settings.snapshot()), &window_);
        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }
        // Settings might have changed while the modal event loop was running.
        if (route != meeting_agent::resolve_meeting_agent_route(*settings.snapshot())) {
            status(QStringLiteral(
                "The insights destination changed. Approve the new destination to continue."));
            return false;
        }
        settings.save_setting(keys::meeting_cloud_consent_given.key(), true);
        grants_->grant(route);
        return true;
    } catch (const std::exception&) {
        status(QStringLiteral("Could not authorize the selected meeting insights destination."));
        return false;
    }
}

std::optional<meeting_agent::MeetingAgentRoute>
MeetingPresenter::askBackupConsent(const meeting_agent::MeetingAgentRoute& primary) {
    const auto resolve = [this, &primary]() -> std::optional<meeting_agent::MeetingAgentRoute> {
        const auto settings = context_.settings().snapshot();
        const auto kind = meeting_agent::resolve_meeting_backup_agent(*settings);
        if (kind == primary.kind || !meeting_agent::is_installed_core(kind)) {
            return std::nullopt;
        }
        return meeting_agent::MeetingAgentRoute{
            kind, kind, meeting_agent::resolve_meeting_agent_model(kind, *settings), Json()};
    };
    try {
        const auto backup = resolve();
        if (!backup) {
            return std::nullopt;
        }
        MeetingConsentDialog dialog(meetingConsentCopy(*backup, *context_.settings().snapshot()),
                                    &window_, true);
        if (dialog.exec() == QDialog::Accepted && resolve() == backup) {
            grants_->grant(*backup);
            return backup;
        }
    } catch (const std::exception&) {
        status(QStringLiteral("Could not authorize the backup agent."));
    }
    return std::nullopt;
}

// refresh_ai_destination's speech half: the local engine's note, or the paired host that hears a
// remote meeting.
void MeetingPresenter::refreshSpeechDestination() {
    if (!tab_) {
        return;
    }
    const auto saved = context_.settings().snapshot();
    const auto engine = models::resolve_meeting_speech(*saved);
    if (engine.source == "remote") {
        std::optional<QString> host;
        try {
            if (const auto pairing = remote::load_client_pairing(*saved)) {
                host = qt::toQString(pairing->host_name);
            }
        } catch (const std::exception&) {
            // An unreadable pairing reads as none.
        }
        tab_->setRemoteSpeechHost(host);
    } else {
        tab_->setSpeechNote(qt::toQString(engine.note));
    }
}

void MeetingPresenter::refreshIntelligenceDestination() {
    if (tab_ && grants_) {
        const auto [where, privacy] = ai_insights_destination(*context_.settings().snapshot());
        tab_->setAiDestination(qt::toQString(where + " \xE2\x80\x94 " + privacy + "."),
                               qt::toQString(where));
    }
}

void MeetingPresenter::identifySavedSpeakers(const QString& id) {
    runtime_.request_speaker_upload(qt::toUtf8(id), [presenter = QPointer<MeetingPresenter>(this)](
                                                        const meeting::SpeakerUploadOffer& offer) {
        if (!presenter) {
            return false;
        }
        auto title = qt::toQString(offer.title);
        if (title.isEmpty()) {
            title = QStringLiteral("Untitled meeting");
        }
        QMessageBox dialog(&presenter->window_);
        dialog.setObjectName(QStringLiteral("meetingSpeakerUploadConsent"));
        dialog.setWindowTitle(QStringLiteral("Identify speakers with OpenAI"));
        dialog.setTextFormat(Qt::PlainText);
        dialog.setText(QStringLiteral("Send system audio from “%1” to OpenAI?").arg(title));
        dialog.setInformativeText(
            QStringLiteral("Meeting started %1. OpenAI will receive the saved system-audio "
                           "track in FLAC windows and may receive short reference clips from "
                           "that same track with anonymous speaker labels. The separate "
                           "microphone track, transcript, and participant names stay on this "
                           "computer. The result updates speaker labels in this meeting.")
                .arg(qt::toQString(offer.started_at)));
        const auto* send =
            dialog.addButton(QStringLiteral("Send system audio"), QMessageBox::AcceptRole);
        auto* cancel = dialog.addButton(QMessageBox::Cancel);
        dialog.setDefaultButton(cancel);
        dialog.setEscapeButton(cancel);
        dialog.exec();
        return dialog.clickedButton() == send;
    });
}

// Confirms the irreversible capture stop before ending a meeting.
void MeetingPresenter::end() {
    if (runtime_.is_active() &&
        QMessageBox::question(
            &window_, QStringLiteral("End meeting?"),
            QStringLiteral("End this meeting now? Audio capture will stop immediately. "
                           "OpenWhisper_CPP will keep working in the background to finish "
                           "the transcript and report."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
        runtime_.end();
    }
}

void MeetingPresenter::applyState(Json patch) {
    if (!patch.is_object()) {
        return;
    }
    state_.update(patch);
    // The tab always learns whether Open Dashboard can work, including during post-meeting
    // finalization when capture is already inactive.
    Json tabPatch = patch;
    if (!tabPatch.contains("dashboard_available")) {
        tabPatch["dashboard_available"] = flag(state_, "dashboard_available");
    }
    if (tab_) {
        tab_->setMeetingState(tabPatch);
    }
    const bool active = flag(state_, "active");
    if (panel_) {
        if (flag(patch, "active")) {
            panel_->setSelectedMeetingId(QString());
        } else if (patch.contains("meeting_id")) {
            panel_->setSelectedMeetingId(field(patch, "meeting_id"));
        }
    }
    static const QStringList kFinished = {QStringLiteral("ended"), QStringLiteral("failed"),
                                          QStringLiteral("needs_recovery"),
                                          QStringLiteral("canceled")};
    if (patch.contains("active") && !flag(patch, "active") &&
        kFinished.contains(field(patch, "status"))) {
        if (auto* history = context_.find<HistoryPanel>()) {
            history->refreshPastMeetings();
        }
    }
    if (patch.contains("active") && window_.tabbedContent() && (active || !window_.isRecording())) {
        if (active && window_.isHostMode()) {
            // A meeting starting here needs its tab, even on a host (switch_to_tab).
            window_.setHostMode(false);
        }
        // Recording on the Meeting Mode tab selects it; once capture ends every tab unlocks and
        // finalization stays on Meeting Mode without blocking Quick Record.
        window_.tabbedContent()->setRecordingState(active, TabbedContentWidget::TabMeetingMode);
    }
    if (tray_) {
        tray_->setMeetingActive(active, flag(state_, "dashboard_available"));
    }
    if (auto* action = context_.menus().action(menu_id::meetingDashboard)) {
        action->setEnabled(flag(state_, "dashboard_available"));
    }
}

void MeetingPresenter::updateDashboard(bool available) {
    applyState(Json{{"dashboard_available", available}});
}

void MeetingPresenter::status(const QString& message) {
    if (!message.isEmpty()) {
        status_ = message;
        window_.setStatus(message);
        if (tab_) {
            tab_->setStatusText(message);
        }
    }
}

void MeetingPresenter::showRecovery(const std::vector<Json>& rows) {
    QList<QJsonObject> meetings;
    for (const auto& row : rows) {
        meetings.push_back(QJsonDocument::fromJson(QByteArray::fromStdString(row.dump())).object());
    }
    auto* dialog = new MeetingRecoveryDialog(
        meetings, [this](const QString& id) { runtime_.finalize_recovered(qt::toUtf8(id)); },
        [this](const QString& id) { runtime_.discard_recovered(qt::toUtf8(id)); }, &window_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void MeetingPresenter::showScanError(const QString& error) {
    status(QStringLiteral("Meeting recovery scan failed"));
    if (scanError_ && scanError_->isVisible()) {
        scanError_->setText(error);
        scanError_->raise();
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Meeting Recovery"), error,
                                QMessageBox::NoButton, &window_);
    scanError_ = box;
    box->setModal(false);
    box->setInformativeText(QStringLiteral("Saved meeting audio remains on this computer. "
                                           "Retry the scan to find interrupted meetings."));
    box->setAttribute(Qt::WA_DeleteOnClose);
    auto* retry = box->addButton(QStringLiteral("Retry scan"), QMessageBox::ActionRole);
    box->addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    connect(box, &QDialog::finished, this, [this, box, retry] {
        if (scanError_ == box) {
            scanError_.clear();
        }
        if (box->clickedButton() == retry) {
            runtime_.retry_recovery_scan();
        }
    });
    box->show();
}

void MeetingPresenter::copy(const std::string& text, const QString& done) {
    auto* clipboard = context_.find<clipboard::TemporaryClipboard>();
    if (!text.empty() && clipboard != nullptr && clipboard->write_text(text)) {
        status(done);
        // The copied animation (show_copied_animation).
        if (auto* history = context_.find<HistoryPanel>()) {
            Q_EMIT history->copiedFromDialog();
        }
    }
}

void MeetingPresenter::copyPastMeeting(const QString& id) {
    // Repository reads must stay off the GUI thread.
    context_.io().post([runtime = &runtime_, ui = &context_.ui(),
                        presenter = QPointer<MeetingPresenter>(this), id = qt::toUtf8(id)] {
        auto text = runtime->copy_past_meeting_transcript(id);
        ui->post([presenter, text = std::move(text)] {
            if (presenter) {
                presenter->copy(text, QStringLiteral("Transcript copied"));
            }
        });
    });
}

void MeetingPresenter::exportPastMeetings() {
    // The panel has already fetched the visible records on io.
    if (panel_) {
        auto* dialog = new MeetingExportDialog(
            [rows = panel_->exportableMeetings()] { return rows; }, root_,
            meetingEntryLoader(repository_), context_.io(), &window_, context_.clock());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    }
}

} // namespace fv
