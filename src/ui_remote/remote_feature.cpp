#include "ui_remote/remote_feature.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>
#include <QString>
#include <QSysInfo>

#include "agentapi/mcp_runtime.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "db/database.h"
#include "dictation/transcription_runtime.h"
#include "history/calendar_source.h"
#include "models/coordinator.h"
#include "models/engine_control.h"
#include "models/hf_access.h"
#include "models/speech_catalog.h"
#include "remote/connection.h"
#include "remote/control.h"
#include "remote/pairing_settings.h"
#include "remote/record_sync_control.h"
#include "remote_host/mcp_session.h"
#include "remote_host/service.h"
#include "remote_records/dictation_records.h"
#include "remote_records/meeting_records.h"
#include "remote_records/sync.h"
#include "settings/model_selection.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/backup_lifecycle.h"
#include "ui_history/history_feature.h"
#include "ui_remote/client_history_dialog.h"
#include "ui_remote/history_publication.h"
#include "ui_remote/host_dashboard.h"
#include "ui_remote/host_model_switch.h"
#include "ui_remote/pair_request_prompter.h"
#include "ui_remote/remote_engine_page.h"
#include "ui_remote/remote_models_dialog.h"
#include "ui_remote/share_meeting_dialog.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_window.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/nav_rail.h"

namespace fv {

static_assert(RemoteFeature::kId == "remote");

namespace {

void showHostModels(AppContext& context, QWidget* parent, const QString& host,
                    std::string fingerprint,
                    const std::shared_ptr<remote::RemoteEngineControl>& service) {
    openRemoteModelsDialog(parent, host, std::move(fingerprint), service, context.io(),
                           context.ui(), [&context] {
                               if (load_model_selection(context.settings()) == "remote") {
                                   if (auto* engine = context.find<models::EngineControl>()) {
                                       engine->reload_engine();
                                   }
                               }
                           });
}

// RemoteRuntime.records_changed (services/runtime/remote.py): History and Past Meetings read
// again whenever a record moves to or from the paired host, the pairing changes or the host is
// renamed: the host's "records" event, the record sync's "records", and the client's "client"
// (paired, forgotten, its addresses learned) and "host_renamed".
void installRecordRefresh(AppContext& context) {
    auto* panel = context.find<HistoryPanel>();
    if (panel == nullptr) {
        return;
    }
    struct Links : QObject {
        using QObject::QObject;
        std::vector<Subscription> links;
    };
    auto* links = new Links(panel); // ends the connections with the panel
    const QPointer<HistoryPanel> target(panel);
    const auto refresh = [target](std::string event) {
        if (target && (event == "records" || event == "client" || event == "host_renamed")) {
            target->refresh();
            target->refreshPastMeetings();
        }
    };
    // The interface the service is provided as, so that a fake engine drives it in tests.
    if (auto* service = context.find<remote::RemoteEngineControl>()) {
        links->links.push_back(service->changed.connect(context.ui(), refresh));
    }
    if (auto* sync = context.find<remote::RecordSyncControl>()) {
        links->links.push_back(sync->changed.connect(context.ui(), refresh));
    }
}

} // namespace

void registerRemoteEnginePage(AppContext& context,
                              std::function<void(RemoteEnginePageServices&)> customize) {
    // The rail shows what the page drew last; nothing until a page has been built, as in
    // OpenWhisper, whose page sets the value when it refreshes.
    auto rail = std::make_shared<QString>();
    SettingsPageSpec spec;
    spec.destination = destinations::remote_engine;
    spec.title = QStringLiteral("Remote engine");
    spec.subtitle = QStringLiteral(
        "Transcribe dictation and meetings with another computer's engine over your network or "
        "Tailscale, or share this computer's engine with computers you pair.");
    // OpenWhisper's history tile on API & MCP is this page's history session in OpenWhisper_CPP
    // (a17-direct-history): search finds it here.
    spec.searchFields = {{"remote_publish_history",
                          QStringLiteral("Share history for this session"),
                          QStringLiteral("Let the paired host read this computer's latest saved "
                                         "dictations and meeting titles for up to 15 minutes.")}};
    spec.controls = {"remote_publish_history"};
    spec.create = [&context, rail, customize = std::move(customize)](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        QPointer<SettingsDialog> window(dialog);
        RemoteEnginePageServices services;
        services.service = context.find<remote::RemoteEngineControl>();
        services.records = context.find<remote::RecordSyncControl>();
        auto* sync = context.find<remote_records::RecordSync>();
        services.canBrowseHeldMeetings = sync != nullptr;
        auto* publication = context.find<remote_history::Publication>();
        services.canPublishHistory = publication != nullptr;
        services.canGrantMcpSession = context.find<remote_host::PeerMcpSession>() != nullptr;
        if (services.service != nullptr) {
            services.io = &context.io();
            services.clock = &context.clock();
        }
        services.setRailValue = [rail, window](const QString& text) {
            *rail = text;
            if (window != nullptr) {
                window->rail()->setValue(QString::fromUtf8(destinations::remote_engine.data(),
                                                           destinations::remote_engine.size()),
                                         text);
            }
        };
        if (auto* engine = context.find<models::EngineControl>()) {
            try {
                services.speech = &context.speech();
                services.usesRemoteDictation = [engine] {
                    return engine->selected_backend() == "remote";
                };
                services.retryRemoteDictation = [engine] {
                    engine->reload_engine();
                };
            } catch (const ServiceUnavailable&) {
                // Minimal settings contexts can still build the pairing controls.
            }
        }
        if (customize) {
            customize(services);
        }
        SettingsPage fallback;
        auto* page = new RemoteEnginePage(container != nullptr ? *container : fallback,
                                          std::move(services), parent);
        if (dialog != nullptr) {
            if (publication) {
                QObject::connect(page, &RemoteEnginePage::publishHistoryRequested, dialog,
                                 [&context, publication, window](const QString& host,
                                                                 const QString& fingerprint) {
                                     if (window) {
                                         remote_history::openSession(
                                             *window, host, qt::toUtf8(fingerprint),
                                             publication->shared_from_this(), context.ui());
                                     }
                                 });
            }
            if (sync) {
                QObject::connect(
                    page, &RemoteEnginePage::shareMeetingRequested, dialog,
                    [&context, sync, window](const QString& host, const QString& fingerprint) {
                        if (window) {
                            openShareMeetingDialog(*window, *sync, host, qt::toUtf8(fingerprint),
                                                   context.io(), context.ui());
                        }
                    });
                QObject::connect(
                    page, &RemoteEnginePage::heldMeetingsRequested, dialog,
                    [&context, sync, window](const QString& host, const QString& fingerprint) {
                        if (window) {
                            openHeldMeetingHistory(*window, *sync, qt::toUtf8(host),
                                                   qt::toUtf8(fingerprint), context.io(),
                                                   context.ui());
                        }
                    });
            }
            // The app provides its RemoteEngineService as the RemoteEngineControl too.
            if (auto* engine = context.find<models::EngineControl>();
                engine && context.find<remote::RemoteEngineControl>()) {
                QObject::connect(page, &RemoteEnginePage::useForDictationRequested, dialog,
                                 [&context, engine, window] {
                                     try {
                                         save_model_selection(context.settings(), "remote");
                                         engine->reload_engine();
                                     } catch (const std::exception& error) {
                                         if (window) {
                                             window->showMessage(QString::fromUtf8(error.what()));
                                         }
                                     }
                                 });
            }
            if (auto* service = context.find<remote_host::RemoteEngineService>()) {
                const auto shared = service->shared_from_this();
                QObject::connect(
                    page, &RemoteEnginePage::manageHostModelsRequested, dialog,
                    [&context, shared, window](const QString& host, const QString& fingerprint) {
                        if (window) {
                            showHostModels(context, window, host, qt::toUtf8(fingerprint), shared);
                        }
                    });
            }
            QObject::connect(page, &RemoteEnginePage::settingsDestinationRequested, dialog,
                             [window](const QString& destination) {
                                 if (window != nullptr) {
                                     window->selectDestination(destination);
                                 }
                             });
        }
        return page;
    };
    spec.railValue = [rail](const Json&) {
        return *rail;
    };
    context.settingsPages().addPage(std::move(spec));
}

void RemoteFeature::provide(AppContext& context) {
    if (context.find<remote::RemoteEngineControl>()) {
        return;
    }
    try {
        auto& settings = context.settings();
        auto& speech_service = context.speech();
        auto& io = context.io();
        auto& shutdown = context.shutdown();
        auto* engine = context.find<models::EngineControl>();
        const auto model = [&settings, engine] {
            if (engine) {
                auto loaded = engine->loaded_model_key();
                if (!loaded.empty()) {
                    return loaded;
                }
            }
            return models::selected_model(*settings.snapshot(), "parakeet");
        };
        const auto readiness = [&speech_service] {
            return speech_service.readiness(speech::Slot::Dictation);
        };
        remote_host::SpeechServiceEngineDeps backend{
            .speech = speech_service,
            .settings = settings,
            .family = [] { return std::string("parakeet"); },
            .model = model,
            .label =
                [model] {
                    const auto* entry = models::model(model());
                    return entry ? std::string(entry->label) : std::string();
                },
            .device =
                [readiness] {
                    const auto read = readiness();
                    const auto end = read.device_info.find(" | ");
                    return read.state == speech::ReadyState::Ready && end != std::string::npos
                               ? read.device_info.substr(end + 3)
                               : std::string();
                },
            .device_info = [readiness] { return readiness().device_info; },
            .available = [readiness] { return readiness().state == speech::ReadyState::Ready; },
        };
        const auto local = remote_host::speech_service_engine(backend);
        std::shared_ptr<remote_records::HostRecordStore> records;
        try {
            auto& db = context.db();
            remote_records::HostRecordStore::Kinds kinds;
            kinds.emplace("dictation", remote_records::dictation_records(
                                           context.paths().user_file("recordings"), db));
            kinds.emplace("meeting",
                          remote_records::meeting_records(context.paths().user_file("meetings"), db,
                                                          context.clock()));
            records = std::make_shared<remote_records::HostRecordStore>(
                context.paths().user_file("remote_records"), std::move(kinds), context.clock());
        } catch (const ServiceUnavailable&) {
            // An app without a local database can still pair and transcribe.
        }
        std::shared_ptr<remote_host::PeerMcpSession> mcpSession;
        if (records) {
            agentapi::McpRuntimeDeps scoped;
            scoped.database = context.db().path();
            scoped.reactor = &context.net();
            scoped.io = &io;
            scoped.clock = &context.clock();
            scoped.ephemeral_token = true;
            const auto listener = std::make_shared<agentapi::McpRuntime>(std::move(scoped));
            using HostSession = remote_host::PeerMcpSession;
            mcpSession = std::make_shared<HostSession>(
                io, HostSession::Listener{
                        .start = [listener] { listener->start(HostSession::kPort, false); },
                        .stop = [listener] { listener->stop(false); },
                        .status =
                            [listener] {
                                const auto state = listener->status();
                                return Json{{"state", state.state},
                                            {"message", state.message},
                                            {"port", HostSession::kPort},
                                            {"tailscale", false}};
                            },
                        .local_token = [listener] { return listener->token(); },
                    });
            context.provide<HostSession>(mcpSession);
        }
        auto service =
            std::make_shared<remote_host::RemoteEngineService>(remote_host::RemoteEngineServiceDeps{
                .paths = context.paths(),
                .identity = context.identity(),
                .settings = settings,
                .credentials = context.credentials(),
                .env = system_env(),
                .clock = context.clock(),
                .io = io,
                .client_env = {context.net(), remote::udp_discovery()},
                .computer_name = QSysInfo::machineHostName().toStdString(),
                .engine =
                    [&settings, backend, local] {
                        const auto selected = load_model_selection(settings);
                        return selected == "parakeet"
                                   ? local
                                   : remote_host::host_engine_for(selected, backend);
                    },
                .components = context.find<models::ComponentCoordinator>(),
                .model_cache = context.find<models::SpeechModelCache>(),
                .model_access = context.find<models::HuggingFaceAccessCoordinator>(),
                .engine_control = engine,
                .speech_service = &speech_service,
                .ui = &context.ui(),
                .inventory = context.find<models::CachedModelsSnapshot>(),
                .records = std::move(records),
                .mcp_session = std::move(mcpSession),
            });
        service->watch_engine(speech_service);
        context.provide<remote::RemoteEngineControl>(service);
        context.provide<remote_host::RemoteEngineService>(service);
        if (!context.find<remote::ClientHistorySource>()) {
            context.provide<remote::ClientHistorySource>(
                std::shared_ptr<remote::ClientHistorySource>(service, &service->history()));
        }
        const std::weak_ptr weak = service;
        try {
            auto publication = std::make_shared<remote_history::Publication>(
                io, context.db().path(),
                [weak]() -> std::optional<remote::ClientPairing> {
                    if (const auto host = weak.lock()) {
                        return host->client_pairing();
                    }
                    return std::nullopt;
                },
                [weak](const remote::ClientPairing& pairing)
                    -> std::shared_ptr<remote::RemoteConnection> {
                    if (const auto host = weak.lock()) {
                        return host->client_connection(pairing, true);
                    }
                    return {};
                });
            context.provide<remote_history::Publication>(publication);
            shutdown.add(ShutdownStep::RecordSync, "Error stopping session history sharing",
                         [publication = std::weak_ptr(publication)] {
                             if (const auto session = publication.lock()) {
                                 session->stop();
                             }
                         });
        } catch (const ServiceUnavailable&) {
            // The manual sharing control needs a local history database.
        }
        if (!context.find<remote::RecordSyncControl>()) {
            try {
                auto client = std::make_shared<remote_records::RecordSync>(remote_records::SyncDeps{
                    .db = context.db(),
                    .settings = settings,
                    .clock = context.clock(),
                    .io = io,
                    .transfer_root = context.paths().user_file("remote_records"),
                    .recordings = context.paths().user_file("recordings"),
                    .meetings = context.paths().user_file("meetings"),
                    .pairing = [service] { return service->client_pairing(); },
                    .connect =
                        [service](const remote::ClientPairing& pairing) {
                            return service->client_connection(pairing);
                        },
                });
                context.provide<remote_records::RecordSync>(client);
                context.provide<remote::RecordSyncControl>(client);
                if (!context.find<history::RemoteRecordSource>()) {
                    context.provide<history::RemoteRecordSource>(client);
                }
                const std::weak_ptr transfers = client;
                shutdown.add(ShutdownStep::RecordSync, "Error stopping record transfers",
                             [transfers] {
                                 if (const auto held = transfers.lock()) {
                                     held->shutdown();
                                 }
                             });
            } catch (const ServiceUnavailable&) {
                // A client without a local database can still use remote speech.
            }
        }
        shutdown.add(ShutdownStep::RemoteEngine, "Error stopping the remote engine host", [weak] {
            if (const auto remote = weak.lock()) {
                remote->shutdown();
            }
        });
        speech_service.set_backend_factory(
            "remote", [weak](const speech::BackendConfig&) -> std::unique_ptr<speech::Backend> {
                if (const auto remote = weak.lock()) {
                    return remote->make_backend();
                }
                return nullptr;
            });
    } catch (const ServiceUnavailable&) {
        // Minimal contexts can build the unbound page without network services.
    }
}

HostDashboard* buildHostDashboard(AppContext& context, QWidget* parent) {
    auto* control = context.find<remote::RemoteEngineControl>();
    auto* dashboard = new HostDashboard(control, context.io(), context.ui(), parent);
    dashboard->setSettings(&context.settings());
    dashboard->setMcpLookup([&context] { return context.find<agentapi::McpServerControl>(); });
    // UIController._on_host_model_selected: the picker switches this computer's engine, and a
    // refusal is said under it.
    QObject::connect(dashboard, &HostDashboard::modelRequested, dashboard,
                     [&context, dashboard](const QString& family, const QString& model) {
                         auto* engine = context.find<models::EngineControl>();
                         const auto error =
                             engine == nullptr
                                 ? std::optional<std::string>(
                                       "Switching models isn't available in this window.")
                                 : select_host_model(*engine, context.settings(),
                                                     family.toStdString(), model.toStdString());
                         if (error) {
                             dashboard->showEngineError(QString::fromStdString(*error));
                         }
                     });
    if (auto* app = context.find<ApplicationController>()) {
        dashboard->followEngineBusy(app->engine_busy_changed);
    }
    if (auto* clientHistory = context.find<remote::ClientHistorySource>()) {
        dashboard->setClientHistorySource(*clientHistory);
    }
    if (auto* settings = context.find<SettingsWindow>()) {
        QObject::connect(dashboard, &HostDashboard::settingsRequested, settings,
                         &SettingsWindow::open);
    }
    return dashboard;
}

void RemoteFeature::install(AppContext& context) {
    if (auto* settings = context.find<SettingsWindow>()) {
        if (const auto service = context.find_shared<remote::RemoteEngineControl>()) {
            const std::weak_ptr<remote::RemoteEngineControl> weak = service;
            settings->setRemoteModelsOpener([&context, weak](QWidget* parent) {
                if (const auto current = weak.lock()) {
                    // Navigation only reads the local snapshot. The dialog verifies the
                    // fingerprint again on its worker before contacting the host.
                    if (const auto pairing = remote::load_client_pairing(context.settings())) {
                        showHostModels(context, parent, qt::toQString(pairing->host_name),
                                       pairing->fingerprint, current);
                        return true;
                    }
                }
                return false;
            });
        }
    }
    // RemoteRuntime's client half for the controller: the readiness check reconnects the Remote
    // engine at once and names the host (application_controller.py
    // _remote_engine_readiness_message).
    if (auto* app = context.find<ApplicationController>()) {
        if (auto* service = context.find<remote_host::RemoteEngineService>()) {
            const std::weak_ptr<remote_host::RemoteEngineService> weak = service->weak_from_this();
            SettingsStore* settings = &context.settings();
            if (auto* transcription = context.find<dictation::TranscriptionRuntime>()) {
                // _retry_remote_for_recording: a recording that stopped while the host was away
                // joins its reconnects.
                transcription->set_remote_link(
                    [weak] {
                        if (const auto remote = weak.lock()) {
                            remote->dictation_link().retry_now();
                        }
                    },
                    [weak] {
                        const auto remote = weak.lock();
                        return remote && remote->dictation_link().restoring();
                    });
                // _timed_decode: the stats line's "on <host> · N ms network".
                transcription->set_remote_timing(
                    [weak]() -> std::function<std::optional<remote::RemoteTiming>()> {
                        const auto remote = weak.lock();
                        if (!remote) {
                            return {};
                        }
                        auto link = remote->dictation_link();
                        const auto mark = link.timing_mark();
                        if (!mark) {
                            return {};
                        }
                        return [link, mark = *mark] {
                            return link.timing_since(mark);
                        };
                    });
            }
            app->set_remote_engine(RemoteEngineHooks{
                .retry_now =
                    [weak] {
                        if (const auto remote = weak.lock()) {
                            remote->dictation_link().retry_now();
                        }
                    },
                .restoring =
                    [weak] {
                        const auto remote = weak.lock();
                        return remote && remote->dictation_link().restoring();
                    },
                .host_name =
                    [settings] {
                        const auto pairing = remote::load_client_pairing(*settings);
                        return pairing ? pairing->host_name : std::string();
                    },
                .link = [weak]() -> std::optional<remote::RemoteLink> {
                    const auto remote = weak.lock();
                    return remote ? remote->dictation_link().link() : std::nullopt;
                },
                .model_choices = [weak]() -> std::optional<remote::RemoteModels> {
                    const auto remote = weak.lock();
                    return remote ? remote->dictation_link().model_choices() : std::nullopt;
                },
                .request_model =
                    [weak](const std::string& family, const std::string& model,
                           const std::optional<Json>& changes) {
                        if (const auto remote = weak.lock()) {
                            remote->request_model(family, model, changes);
                        }
                    },
                .connected_clients = [weak]() -> std::vector<remote::ConnectedClient> {
                    const auto remote = weak.lock();
                    return remote ? remote->connected_clients()
                                  : std::vector<remote::ConnectedClient>{};
                },
            });
            // remote_link_poke and remote_clients_changed: the controller reads afresh on the
            // UI thread. The connections end with the main window.
            if (auto* window = context.find<MainWindow>()) {
                struct Links : QObject {
                    using QObject::QObject;
                    std::vector<Subscription> links;
                };
                auto* links = new Links(window);
                const std::weak_ptr<ApplicationController> controller =
                    context.find_shared<ApplicationController>();
                links->links.push_back(
                    service->dictation_link_changed.connect(context.io(), [controller] {
                        if (const auto held = controller.lock()) {
                            held->remote_link_poked();
                        }
                    }));
                links->links.push_back(
                    service->changed.connect(context.io(), [controller](std::string event) {
                        const auto held = controller.lock();
                        if (held &&
                            (event == "clients" || event == "activity" || event == "state")) {
                            held->remote_clients_poked();
                        }
                    }));
            }
        }
    }
    installRecordRefresh(context);
    if (auto* backup = context.find<BackupLifecycle>()) {
        if (auto* sync = context.find<remote_records::RecordSync>()) {
            backup->addWorkerContext([weak = sync->weak_from_this()](const auto& work) {
                if (const auto client = weak.lock()) {
                    const auto pause = client->paused_for_backup();
                    work();
                } else {
                    work();
                }
            });
        }
        if (auto* service = context.find<remote_host::RemoteEngineService>()) {
            backup->addWorkerContext([weak = service->weak_from_this()](const auto& work) {
                const auto host = weak.lock();
                if (auto* gate = host ? host->record_gate() : nullptr) {
                    const auto pause = gate->paused();
                    work();
                } else {
                    work();
                }
            });
        }
    }

    if (auto* window = context.find<MainWindow>()) {
        auto* control = context.find<remote::RemoteEngineControl>();
        if (control) {
            installPairRequestPrompter(*control, context.io(), context.ui(), *window);
        }
        window->setHostDashboardFactory([&context](QWidget* parent) -> QWidget* {
            return buildHostDashboard(context, parent);
        });
    }
    registerRemoteEnginePage(context);
}

} // namespace fv
