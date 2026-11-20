#include "ui_meeting/meeting_feature.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <QDesktopServices>
#include <QMessageBox>
#include <QPointer>
#include <QScopedValueRollback>
#include <QUrl>

#include "appcore/activity.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/event_bus.h"
#include "base/env.h"
#include "base/log.h"
#include "base/subscription.h"
#include "history/calendar_source.h"
#include "http/client.h"
#include "meeting/meeting_intelligence.h"
#include "meeting/openai_diarize.h"
#include "meeting/runtime.h"
#include "meeting/setup_check.h"
#include "meeting_agent/factory.h"
#include "meeting_intel/custom_report.h"
#include "meeting_intel/history_source.h"
#include "meeting_intel/insight_review.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/pdf_text.h"
#include "meeting_intel/recall.h"
#include "meeting_state/calendar_meetings.h"
#include "meeting_state/sql_repository.h"
#include "models/component_store.h"
#include "models/engine_resolution.h"
#include "models/meeting_speech.h"
#include "models/onnx_runtime.h"
#include "models/pdfium_runtime.h"
#include "net/reactor.h"
#include "process/launcher.h"
#include "remote_host/service.h"
#include "settings/keys.h"
#include "ui_controller/application_controller.h"
#include "ui_meeting/after_end_page.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_meeting/intelligence_page.h"
#include "ui_meeting/meeting_consent_dialog.h"
#include "ui_meeting/meeting_home_rows.h"
#include "ui_meeting/meeting_presenter.h"
#include "ui_settings/home_rows.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_meeting.feature"};

struct MeetingServices {
    std::weak_ptr<remote_host::RemoteEngineService> remote;
    QPointer<MainWindow> parent;
    bool savedConsentOpen = false;
    std::unique_ptr<net::Reactor> ownReactor;
    meeting::SetupDeps setup;
    std::shared_ptr<meeting_state::SqlMeetingRepository> repository;
    std::filesystem::path root;
    std::shared_ptr<MeetingConsentGrants> grants;
    std::unique_ptr<meeting_intel::RepositoryHistorySource> recallHistory;
    meeting_intel::RecallDeps recall;
    // Waits for the startup database before the recovery scan starts (install()).
    Subscription databaseReady;
    bool setupStarted = false;
};

bool usesOwnSpeech(const Json& settings) {
    const auto choice = models::resolve_meeting_speech(settings);
    return choice.source == "local" && (choice.separate || !choice.note.empty());
}

speech::BackendConfig resolveMeetingBackend(Json settings, const Paths& paths) {
    const std::string model = models::resolve_meeting_whisper_model(settings);
    Json localModels = keys::local_asr_models.read(settings);
    localModels["parakeet"] = model;
    settings[std::string(keys::local_asr_models.key())] = std::move(localModels);
    const auto resolved = models::resolve_parakeet(settings, paths);
    speech::BackendConfig config;
    config.backend = "parakeet";
    config.runtime_dir = resolved.runtime_dir.value_or(std::filesystem::path{});
    config.model_path = resolved.model_path.value_or(std::filesystem::path{});
    config.model_key = resolved.model_key;
    config.model_label = resolved.model_label;
    config.requested_device = resolved.requested_device;
    config.device = resolved.device;
    config.gpu_index = std::max(0, resolved.gpu_index);
    config.missing_message = resolved.missing_message;
    if (resolved.missing_state == models::EngineMissing::RuntimeMissing) {
        config.missing_state = speech::ReadyState::RuntimeMissing;
    } else if (resolved.missing_state == models::EngineMissing::ModelMissing) {
        config.missing_state = speech::ReadyState::ModelMissing;
    }
    return config;
}

} // namespace

void MeetingFeature::provide(AppContext& context) {
    try {
        auto& settings = context.settings();
        models::keep_separate_meeting_speech(settings);
        auto& clock = context.clock();
        auto& audio = context.audio();
        auto& speechService = context.speech();
        auto services = std::make_shared<MeetingServices>();
        services->root = context.paths().user_file("meetings");
        services->repository =
            std::make_shared<meeting_state::SqlMeetingRepository>(context.db(), clock);
        services->setup =
            meeting::setup_deps({context.paths(), system_env(), settings, context.http(),
                                 context.service<models::SpeechModelCache>(),
                                 context.service<models::ComponentCoordinator>(),
                                 context.service<models::HuggingFaceAccessCoordinator>(), audio});

        meeting::MeetingRuntimeDeps deps;
        deps.ui = &context.ui();
        deps.settings = &settings;
        deps.repository = services->repository;
        deps.past_meetings = meeting_web::sql_past_meeting_pages(services->repository);
        deps.meetings_root = services->root;
        deps.clock = &clock;
        deps.activity = &context.activity();
        try {
            deps.events = &context.events();
        } catch (const ServiceUnavailable&) {
            // A partial startup may have no event sink.
        }
        deps.speech = &speechService;
        deps.confirm_live_audio = [services](const Json& route) {
            if (!services->parent || services->remote.expired())
                return false;
            QMessageBox box(
                QMessageBox::Question, QStringLiteral("Send audio for this meeting?"),
                QStringLiteral("Send microphone and enabled system audio to %1 for transcription "
                               "during this meeting? Saved meetings stay on this computer.")
                    .arg(qt::toQString(route.at("host_name").get<std::string>())),
                QMessageBox::Yes | QMessageBox::Cancel, services->parent);
            box.setTextFormat(Qt::PlainText);
            box.setDefaultButton(QMessageBox::Cancel);
            return box.exec() == QMessageBox::Yes;
        };
        deps.live_remote_backend = [services](std::shared_ptr<Json> pinned) {
            const auto host = services->remote.lock();
            if (!host)
                throw EngineError("Paired meeting speech is unavailable.");
            return host->make_meeting_backend(std::move(pinned));
        };
        // Bootstrap owns the audio backend and releases it after the context.
        deps.audio = std::shared_ptr<audio::Backend>(&audio, [](audio::Backend*) {});
        try {
            deps.reactor = &context.net();
        } catch (const ServiceUnavailable&) {
            services->ownReactor = std::make_unique<net::Reactor>();
            deps.reactor = services->ownReactor.get();
        }
        try {
            auto grants = std::make_shared<MeetingConsentGrants>();
            auto dotenv = std::make_shared<DotEnv>(context.paths().env_file());
            auto client = std::shared_ptr<http::Client>(&context.http(), [](http::Client*) {});
            meeting_agent::CoreDeps agentDeps{std::move(client),
                                              {context.credentials(), system_env(), *dotenv},
                                              [&settings] { return *settings.snapshot(); },
                                              deps.reactor,
                                              make_process_launcher(),
                                              &clock};
            services->recallHistory =
                std::make_unique<meeting_intel::RepositoryHistorySource>(*services->repository);
            services->recall = {services->recallHistory.get(), agentDeps.settings, nullptr, &clock};
            deps.intelligence = std::make_shared<meeting::MeetingIntelligence>(
                [agentDeps, dotenv](std::string_view kind) mutable {
                    return meeting_agent::create_agent_core(kind, agentDeps);
                },
                agentDeps.settings, [grants](const auto& route) { return grants->allows(route); },
                &services->recall);
            // Local fast judgments must connect directly even with a system proxy.
            const http::ProxyResolver direct = [](std::string_view) {
                return std::optional<std::string>{};
            };
            llm::JudgeHooks localHooks;
            localHooks.proxy_for = direct;
            auto judgeEnvironment = std::make_shared<llm::JudgeEnvironment>(
                http::make_curl_client({direct}), agentDeps.keys.store, agentDeps.keys.env, *dotenv,
                std::move(localHooks));
            auto judgeProvider =
                std::shared_ptr<llm::JudgeProvider>(llm::make_judge_provider(*judgeEnvironment));
            deps.local_fast_judge = [read = agentDeps.settings, dotenv, judgeEnvironment,
                                     judgeProvider] {
                (void)dotenv;
                (void)judgeEnvironment;
                const auto settings = read();
                return meeting_intel::resolve_typesafe_enabled(settings) &&
                               judgeProvider->route(settings).is_local()
                           ? judgeProvider->judge(settings)
                           : std::shared_ptr<llm::Judge>{};
            };
            services->recall.local_judge = deps.local_fast_judge;
            // insight_review._default_reviewer: the review's judge waits at least
            // REVIEW_TIMEOUT_S per question, longer when the route's own budget is longer.
            deps.local_review_judge = [read = agentDeps.settings, judgeEnvironment,
                                       judgeProvider](double atLeast) {
                const auto settings = read();
                return meeting_intel::build_review_judge(
                    settings, judgeProvider->route(settings), atLeast,
                    [&settings, &judgeEnvironment](const llm::JudgeOptions& options) {
                        return llm::judge_from_settings(settings, *judgeEnvironment, options);
                    });
            };
            deps.start_saved_report =
                [services, dotenv, keys = agentDeps.keys, read = agentDeps.settings,
                 client = http::make_curl_client({direct}), &clock](
                    std::shared_ptr<meeting_state::MeetingStateStore> store,
                    const meeting_agent::MeetingAgentRoute& destination, std::string_view request,
                    Executor& worker, const CancelToken& cancel, std::function<bool()> allowed) {
                    const meeting_intel::ReportRoute route{destination.provider, destination.model,
                                                           destination.endpoint};
                    meeting_intel::ReportDeps report{*services->repository,
                                                     client,
                                                     keys,
                                                     [services, dotenv, read] { return read(); },
                                                     &services->recall,
                                                     clock,
                                                     &worker,
                                                     {},
                                                     {},
                                                     cancel,
                                                     std::move(allowed)};
                    return meeting_intel::start_custom_report(std::move(store), report, request,
                                                              route, "");
                };
            deps.confirm_saved_insights = [services](const Json& meeting,
                                                     const meeting_agent::MeetingAgentRoute& route,
                                                     bool report) {
                if (!services->parent || services->savedConsentOpen) {
                    return false;
                }
                const QScopedValueRollback pending(services->savedConsentOpen, true);
                const auto copy = meetingConsentCopy(route);
                const auto text = [&meeting](const char* field) {
                    const auto value = meeting.find(field);
                    return value != meeting.end() && value->is_string()
                               ? qt::toQString(value->get<std::string>())
                               : QString();
                };
                const auto title = text("title");
                const auto name = title.isEmpty() ? text("id") : title;
                const auto date = text("started_at");
                const auto model = route.model.empty()
                                       ? QStringLiteral("the destination's default model")
                                       : qt::toQString(route.model);
                QMessageBox dialog(
                    QMessageBox::Question,
                    report ? QStringLiteral("Write a Custom Meeting Report")
                           : QStringLiteral("Retry Saved Meeting Insights"),
                    QStringLiteral(
                        "Allow %1 to use the transcript and dashboard state for %2 (%3) "
                        "with %4 for this request? A custom report also sends your request. "
                        "If enabled, relevant excerpts from past meetings or your knowledge "
                        "folder can be included. Audio is kept on its selected speech route. "
                        "The configured credential or installed agent sign-in is used to "
                        "connect to this destination.")
                        .arg(qt::toQString(copy.destination), name, date, model),
                    QMessageBox::Yes | QMessageBox::Cancel, services->parent);
                dialog.setTextFormat(Qt::PlainText);
                dialog.setInformativeText(qt::toQString(copy.detail));
                dialog.setDefaultButton(QMessageBox::Cancel);
                return dialog.exec() == QMessageBox::Yes;
            };
            deps.approved_speaker_decoder = [client = agentDeps.http, keys = agentDeps.keys,
                                             dotenv]() mutable -> meeting::DiarizeFn {
                (void)dotenv;
                auto key = llm::lookup_env_value("OPENAI_API_KEY", keys);
                return key ? meeting::make_openai_diarize(client, std::move(*key))
                           : meeting::DiarizeFn{};
            };
            deps.confirm_browser_insights = [services, grants, read = agentDeps.settings](
                                                const std::string& title,
                                                const meeting_agent::MeetingAgentRoute& route) {
                if (!services->parent) {
                    return false;
                }
                MeetingConsentDialog dialog(meetingConsentCopy(route, read()), services->parent);
                dialog.setWindowTitle(title.empty()
                                          ? QStringLiteral("Dashboard Requested AI Insights")
                                          : QStringLiteral("Dashboard Requested AI Insights for %1")
                                                .arg(qt::toQString(title)));
                if (dialog.exec() != QDialog::Accepted ||
                    route != meeting_agent::resolve_meeting_agent_route(read())) {
                    return false;
                }
                grants->grant(route);
                return true;
            };
            services->grants = std::move(grants);
        } catch (const ServiceUnavailable&) {
            // Partial contexts still offer transcript-only meetings.
        }
        auto lease = std::make_shared<meeting::SpeechModelLease>(
            speechService, [&settings] { return usesOwnSpeech(*settings.snapshot()); },
            "meeting-finalization",
            [&settings, paths = context.paths()] {
                return resolveMeetingBackend(*settings.snapshot(), paths);
            });
        // install() routes its dictation half through the application controller.
        context.provide<meeting::SpeechModelLease>(lease);
        deps.model_lease = std::move(lease);
        deps.forget_replayable_events = [] {
            automation::clear_bus();
        };
        deps.open_url = [](std::string_view url) {
            return QDesktopServices::openUrl(QUrl(qt::toQString(url)));
        };
        deps.engine_loading_message = [&speechService]() -> std::optional<std::string> {
            if (speechService.readiness(speech::Slot::Dictation).state ==
                speech::ReadyState::Loading) {
                return "The speech engine is still loading. Try again when it is ready.";
            }
            return std::nullopt;
        };
        deps.sources.mic_priority = services->setup.mic_priority;
        deps.sources.speaker_model_path = services->setup.speaker_model_path;
        deps.sources.ensure_speaker_model = [setup = services->setup](const CancelToken& cancel) {
            setup.ensure_speaker_model(cancel);
            return setup.speaker_model_path();
        };
        auto store = std::make_shared<models::ComponentStore>(context.paths().components_root(),
                                                              context.http());
        meeting_intel::set_pdfium_path(models::locate_pdfium(system_env(), *store));
        deps.sources.onnxruntime_path = [store] {
            return models::locate_onnxruntime(system_env(), *store);
        };
        auto runtime = std::make_shared<meeting::MeetingRuntime>(std::move(deps));
        if (!context.find<history::MeetingCalendarSource>()) {
            context.provide<history::MeetingCalendarSource>(
                std::make_shared<meeting_state::SqlMeetingCalendarSource>(services->repository,
                                                                          clock));
        }
        context.provide(std::move(services));
        context.provide(runtime);
        context.shutdown().add(ShutdownStep::MeetingRuntime, "Error during meeting runtime cleanup",
                               [runtime = std::weak_ptr(runtime)] {
                                   if (auto live = runtime.lock()) {
                                       live->cleanup();
                                   }
                               });
    } catch (const ServiceUnavailable& error) {
        kLog.info("Meeting Mode is unavailable: {}", error.what());
    }
}

void MeetingFeature::install(AppContext& context) {
    // MeetingRuntime._model_lease is (controller.release_local_engine,
    // controller.restore_local_engine): the reload machine learns of the lease, and a Remote
    // dictation engine is left connected.
    if (auto* lease = context.find<meeting::SpeechModelLease>()) {
        if (auto* app = context.find<ApplicationController>()) {
            lease->route_dictation(app->lease_release_hook(), app->lease_restore_hook());
        }
    }
    registerMeetingSettingsPages(context);
    registerIntelligencePage(context);
    if (auto* rows = context.find<HomeRows>()) {
        registerMeetingHomeRows(*rows);
    }

    auto* services = context.find<MeetingServices>();
    auto* runtime = context.find<meeting::MeetingRuntime>();
    auto* window = context.find<MainWindow>();
    if (services == nullptr || runtime == nullptr || window == nullptr) {
        return;
    }
    services->parent = window;
    if (auto* host = context.find<remote_host::RemoteEngineService>()) {
        services->remote = host->weak_from_this();
    }
    auto* presenter = new MeetingPresenter(context, *window, *runtime, services->setup,
                                           services->repository, services->root, services->grants);
    presenter->install();
    if (auto* lifecycle = context.find<ShellLifecycle>()) {
        auto previous = std::move(lifecycle->trayCreated);
        lifecycle->trayCreated = [previous = std::move(previous),
                                  presenter =
                                      QPointer<MeetingPresenter>(presenter)](SystemTray& tray) {
            if (previous) {
                previous(tray);
            }
            if (presenter) {
                presenter->attachTray(tray);
            }
        };
    }
    // _on_startup_database_ready runs meeting_runtime.setup(): the controller opens the startup
    // database once the main window is shown (notify_main_ui_ready), so the recovery scan, and
    // the dialog it may raise, never start behind the splash.
    const auto setupWhenReady = [services, runtime](const ActivitySnapshot& activity) {
        if (!activity.startup_ready || activity.shutting_down || services->setupStarted) {
            return;
        }
        services->setupStarted = true;
        runtime->setup();
    };
    services->databaseReady = context.activity().changed.connect(context.ui(), setupWhenReady);
    setupWhenReady(context.activity().snapshot());
}

} // namespace fv
