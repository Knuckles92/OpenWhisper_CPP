#include "ui_controller/personalize_wiring.h"

#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QStackedWidget>

#include "agents/agent_actions.h"
#include "agents/installed_agents.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "automation/action_model.h"
#include "base/env.h"
#include "base/log.h"
#include "base/paths.h"
#include "clipboard/temporary_clipboard.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "dictation/cleanup_stage.h"
#include "dictation/command_runtime.h"
#include "dictation/dictionary_learning.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/text_preparer.h"
#include "dictation/transcription_runtime.h"
#include "http/client.h"
#include "llm/cleanup_settings.h"
#include "llm/credentials.h"
#include "llm/profiles.h"
#include "llm/thinking_levels.h"
#include "llm/transcript_cleanup.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "speech/service.h"
#include "text/cleanup_prompts.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/rule_transcriber.h"
#include "ui_controller/ui_controller.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/personal_binding_sources.h"
#include "ui_personalize/cleanup_agent_offer.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_personalize/cleanup_rules_section.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_personalize/rules_page.h"
#include "ui_personalize/transforms_panel.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_setup/cleanup_provider_row.h"
#include "ui_setup/guided_setup_dialog.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/agent_lookup.h"
#include "ui_widgets/dictation_languages_field.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.application_controller"};

template <class T> T* findService(AppContext& context, T& (AppContext::*accessor)()) {
    try {
        return &(context.*accessor)();
    } catch (const ServiceUnavailable&) {
        return nullptr;
    }
}

void required(const void* part, const char* name) {
    if (part == nullptr) {
        throw std::invalid_argument(std::string("The personalization wiring needs ") + name + ".");
    }
}

// The variable the saved cleanup provider's key is read from, "" when it needs none.
std::string cleanupKeyVariable(const Json& settings) {
    const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
    if (llm::is_installed_cleanup_agent(provider)) {
        return {};
    }
    const std::optional<llm::TextLLMProfile> profile = llm::get_profile(provider, settings);
    return profile ? profile->api_key_env : std::string();
}

bool cleanupAgentAvailable(std::string_view provider) {
    const auto registry = agents::installed_agents_if_set();
    const auto agent = registry ? registry->resolve_agent(provider) : std::nullopt;
    return agent && agent->usable();
}

std::optional<std::string> runCleanupAgent(const llm::CleanupAgentRequest& request,
                                           const CancelToken& cancel) {
    if (cancel.cancelled()) {
        return std::nullopt;
    }
    auto outcome =
        agents::ask_for_cleanup(request.provider, request.text, request.system_prompt,
                                request.model, request.reasoning, request.timeout_s, cancel);
    if (outcome.ok && !outcome.stopped && !cancel.cancelled()) {
        return std::move(outcome.answer);
    }
    return std::nullopt;
}

} // namespace

PersonalizeWiring::PersonalizeWiring() = default;

PersonalizeWiring::~PersonalizeWiring() {
    links_.clear();
    // The page reads the provider through a global; it must not outlive the speech service.
    DictionaryPage::setBackendProvider({});
    llm::set_codex_efforts({}, {});
}

// application_controller.py:165-275's M4 share, in its construction order: the transcription
// runtime's AI cleanup client (deferred), dictionary learning, then CommandRuntime.
void PersonalizeWiring::provide(AppContext& context, const PersonalizeParts& parts) {
    required(parts.transcription, "the transcription runtime");
    required(parts.hotkeys, "the hotkey runtime");
    required(parts.app, "the application controller");
    required(parts.clipboard, "the clipboard");
    required(parts.keys, "the synthetic keys");
    required(parts.capture, "the focus capture");
    const std::weak_ptr<agents::InstalledAgents> installedAgents =
        agents::installed_agents_if_set();
    llm::set_codex_efforts(
        [installedAgents] {
            std::map<std::string, std::vector<std::string>> byModel;
            if (const auto registry = installedAgents.lock()) {
                for (auto& [model, efforts] : registry->config().codex_model_efforts()) {
                    byModel.emplace(std::move(model), std::move(efforts));
                }
            }
            return byModel;
        },
        [installedAgents](std::string_view provider) {
            const auto registry = installedAgents.lock();
            return registry ? registry->config().configured_default_model(provider) : std::string();
        });
    parts_ = parts;
    SettingsStore& settings = context.settings();
    Executor& ui = context.ui();
    dictation::TranscriptionRuntime* transcription = parts.transcription;
    dictation::HotkeyRuntime* hotkeys = parts.hotkeys;
    ApplicationController* app = parts.app;

    // Learning saves on the persistence strand and says so on the UI thread (install()); the
    // preparer that hands it pasted dictations is let go by the runtime before it goes.
    auto learner = std::make_shared<dictation::DictionaryLearner>(
        settings, ui, context.persistence(), parts.capture);
    learner_ = learner.get();
    context.provide<dictation::DictionaryLearner>(learner);
    std::shared_ptr<dictation::DictionaryTextPreparer> preparer(
        new dictation::DictionaryTextPreparer(learner.get()),
        [transcription](dictation::DictionaryTextPreparer* made) {
            transcription->set_text_preparer(nullptr);
            delete made;
        });
    transcription->set_text_preparer(preparer.get());
    context.provide<dictation::DictionaryTextPreparer>(preparer);

    // transcribe_clip, for the Learned rules page's dictated rules.
    RuleTranscriberPorts transcriberPorts;
    transcriberPorts.activity = &context.activity();
    transcriberPorts.speech = &context.speech();
    transcriberPorts.busy = [app] {
        return app->is_transcribing();
    };
    context.provide<RuleTranscriber>(
        std::make_shared<ControllerRuleTranscriber>(std::move(transcriberPorts)));

    // A profile's shortcut toggles its recording (hotkeys.py:255); the press arrives on its own
    // thread.
    links_.push_back(hotkeys->profile_record_requested.connect(
        ui, [app](std::string profileId) { app->toggle_profile_recording(profileId); }));

    http::Client* http = findService(context, &AppContext::http);
    CredentialStore* credentials = findService(context, &AppContext::credentials);
    if (http == nullptr || credentials == nullptr) {
        kLog.info("AI cleanup and Command Mode are off: no {} in this context",
                  http == nullptr ? "network client" : "credential store");
        return;
    }
    // Every holder of the key sources keeps the .env reader alive in its deleter.
    auto dotenv = std::make_shared<DotEnv>(context.paths().env_file());
    const EnvReader& environment =
        parts.keyEnvironment != nullptr ? *parts.keyEnvironment : system_env();
    const llm::KeySources keys{*credentials, environment, *dotenv};
    // The app's client outlives every request: it is borrowed, never owned.
    std::shared_ptr<http::Client> client(http, [](http::Client*) {});
    const auto makeCleanup = [&](llm::CleanupOptions options) {
        options.default_prompt = text::legacy_default_prompt();
        options.clock = &context.clock();
        return std::shared_ptr<llm::TranscriptCleanup>(
            new llm::TranscriptCleanup(
                client, keys, [&settings] { return *settings.snapshot(); }, {}, {}, {},
                /*defer_client=*/true, std::move(options)),
            [dotenv](llm::TranscriptCleanup* made) { delete made; });
    };
    // Commands can contain a selection or clipboard text. Their endpoint client stays separate
    // from the runner the user selected for spoken dictation.
    auto commandCleanup = makeCleanup({});
    llm::CleanupOptions options;
    const auto* injected = context.find<llm::CleanupOptions>();
    options.agent_available = injected ? injected->agent_available : cleanupAgentAvailable;
    auto run = injected ? injected->run_agent : decltype(options.run_agent)(runCleanupAgent);
    if (run) {
        options.run_agent = [&settings, &ui, run = std::move(run)](
                                const llm::CleanupAgentRequest& request,
                                const CancelToken& cancel) -> std::optional<std::string> {
            CancelSource pass(cancel);
            // The runner can spend time starting the CLI before it sends stdin. Turning cleanup
            // off or switching recipients also cancels a request already starting or running.
            const auto changed = settings.watch({keys::transcript_cleanup_enabled.key(),
                                                 keys::transcript_cleanup_provider.key(),
                                                 keys::fv_cleanup_agent_consent.key()},
                                                ui, [pass](const Json&) mutable { pass.cancel(); });
            if (pass.cancelled() ||
                !llm::cleanup_agent_authorized(*settings.snapshot(), request.provider)) {
                return std::nullopt;
            }
            auto answer = run(request, pass.token());
            return pass.cancelled() ? std::nullopt : std::move(answer);
        };
    }
    auto cleanup = makeCleanup(std::move(options));
    cleanup_ = cleanup.get();
    context.provide<llm::TranscriptCleanup>(cleanup);

    std::shared_ptr<dictation::LlmTranscriptCleaner> cleaner(
        new dictation::LlmTranscriptCleaner(cleanup),
        [transcription](dictation::LlmTranscriptCleaner* made) {
            transcription->set_cleaner(nullptr);
            delete made;
        });
    transcription->set_cleaner(cleaner.get());
    context.provide<dictation::LlmTranscriptCleaner>(cleaner);

    // Its selection reads and the transform's provider check must not run on the jobs strand,
    // where complete_recording waits for them.
    std::shared_ptr<dictation::CommandRuntime> commands(
        new dictation::CommandRuntime(*transcription, settings, *parts.clipboard, *parts.keys,
                                      *parts.capture, commandCleanup, ui, context.io(),
                                      context.clock(), keys),
        [transcription, hotkeys, dotenv](dictation::CommandRuntime* made) {
            hotkeys->set_command_keys(nullptr);
            transcription->set_command_runtime(nullptr);
            delete made;
        });
    commands_ = commands.get();
    hotkeys->set_command_keys(commands.get());
    transcription->set_command_runtime(commands.get());
    context.provide<dictation::CommandRuntime>(commands);

    // hotkeys.py:257: a transform's shortcut rewrites the selection.
    std::weak_ptr<dictation::CommandRuntime> weakCommands = commands;
    links_.push_back(
        hotkeys->transform_requested.connect(ui, [weakCommands](std::string transformId) {
            if (const auto runtime = weakCommands.lock()) {
                runtime->run_transform(std::move(transformId));
            }
        }));

    // cleanup()'s "Error during command runtime cleanup", before the hotkey runtime stops.
    if (ShutdownPlan* plan = findService(context, &AppContext::shutdown)) {
        dictation::CommandRuntime* runtime = commands.get();
        plan->add(ShutdownStep::CommandRuntime, "Error during command runtime cleanup",
                  [runtime] { runtime->cleanup(); });
    }
}

void PersonalizeWiring::install(AppContext& context, UiController& ui) {
    ApplicationController* app = parts_.app;
    dictation::HotkeyRuntime* hotkeys = parts_.hotkeys;
    required(app, "provide() first");

    // _setup_ui_callbacks (330-333).
    ui.onProfileRecordStart = [app](const std::string& profileId, const std::string& source) {
        return app->start_profile_recording(profileId, source);
    };
    ui.onCleanupProfilesChangedHook = [hotkeys] {
        hotkeys->refresh_profile_hotkeys();
    };
    ui.onProfileHotkeyCaptureHook = [hotkeys](bool suspended) {
        hotkeys->set_capture_suspended(suspended);
    };

    // dictionary_term_learned: learning saves on a worker; the notice is the UI thread's.
    QPointer<UiController> guardedUi = &ui;
    if (learner_ != nullptr) {
        links_.push_back(learner_->term_learned.connect(
            context.ui(), [guardedUi](std::string term, bool protectedTerm) {
                if (guardedUi != nullptr) {
                    guardedUi->onDictionaryTermLearned(qt::toQString(term), protectedTerm);
                }
            }));
    }

    // The Settings window's AI cleanup callbacks (_prepare_settings_dialog, 1627-1630).
    if (auto* cleanup = context.find<CleanupSettings>()) {
        connect(cleanup, &CleanupSettings::cleanupChanged, &ui,
                &UiController::refreshCleanupControls);
        connect(cleanup, &CleanupSettings::cleanupProfilesChanged, &ui,
                &UiController::onCleanupProfilesChanged);
        connect(cleanup, &CleanupSettings::profileHotkeyCapture, &ui,
                &UiController::onProfileHotkeyCapture);
        cleanup->services().meetingActive = [app] {
            return app->is_meeting_active();
        };
    }

    // The one-time offer of a signed-in Claude Code or Codex for AI cleanup (ui_controller.py:
    // _cleanup_agent_offer), made when cleanup is turned on in Quick Record or in Settings.
    if (offer_ == nullptr) {
        MainWindow* window = context.find<MainWindow>();
        offer_ =
            new CleanupAgentOffer(context.settings(), context.io(), context.ui(), window, {}, this);
        QPointer<UiController> guarded = &ui;
        offer_->onAccepted = [guarded, &context] {
            // _on_cleanup_agent_accepted: show the new runner wherever it is named.
            if (auto* settingsWindow = context.find<SettingsWindow>()) {
                if (SettingsDialog* dialog = settingsWindow->dialog()) {
                    dialog->refresh();
                }
            }
            if (auto* guided = context.find<GuidedSetup>()) {
                if (GuidedSetupDialog* dialog = guided->dialog()) {
                    dialog->refresh();
                }
            }
            if (guarded != nullptr) {
                guarded->refreshCleanupControls();
            }
        };
        CleanupAgentOffer* offer = offer_;
        if (window != nullptr) {
            connect(window, &MainWindow::cleanupToggled, offer, &CleanupAgentOffer::maybeOffer);
        }
        if (auto* cleanup = context.find<CleanupSettings>()) {
            connect(cleanup, &CleanupSettings::cleanupChanged, offer,
                    &CleanupAgentOffer::maybeOffer);
        }
    }

    // settings_dictionary.set_backend_provider(get_current_backend): the engine loaded here,
    // which takes no words into its model. A paired host's engine (M9) answers for itself.
    speech::SpeechService* speech = &context.speech();
    DictionaryPage::setBackendProvider([speech, settings = &context.settings()] {
        BackendInfo info;
        if (const auto remote = speech::remote_voice_capabilities(*settings->snapshot())) {
            info.remote = true;
            info.hostName = remote->host;
            info.name = remote->engine_label;
            info.support = remote->recognition_hints ? "model" : "after";
        }
        info.available =
            speech->readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready;
        return info;
    });

    if (auto* window = context.find<SettingsWindow>()) {
        links_.push_back(speech->readiness_changed.connect(
            context.ui(), [window](speech::Slot slot, const speech::Readiness&) {
                if (slot != speech::Slot::Dictation) {
                    return;
                }
                if (auto* dialog = window->dialog(); dialog && dialog->isVisible()) {
                    dialog->refreshPage(destinations::dictionary);
                    if (auto* voice = dialog->pageContent(destinations::voice_model)) {
                        if (auto* languages = voice->findChild<DictationLanguagesField*>()) {
                            languages->refresh();
                        }
                    }
                }
            }));
        connect(window, &SettingsWindow::dialogCreated, this,
                [this, &context](SettingsDialog* dialog) { wireDialog(context, dialog); });
        if (SettingsDialog* dialog = window->dialog()) {
            wireDialog(context, dialog);
        }
    }

    // Guided setup's provider row (provider_summary), whose key status is read off the GUI
    // thread.
    if (auto* guided = context.find<GuidedSetup>()) {
        CredentialStore* credentials = findService(context, &AppContext::credentials);
        KeyPresence::Lookup lookup = [](const std::string&) {
            return false;
        };
        if (credentials != nullptr) {
            auto dotenv = std::make_shared<DotEnv>(context.paths().env_file());
            const EnvReader* environment =
                parts_.keyEnvironment != nullptr ? parts_.keyEnvironment : &system_env();
            lookup = [credentials, dotenv, environment](const std::string& env) {
                llm::KeySources keys{*credentials, *environment, *dotenv};
                const std::optional<std::string> value = llm::lookup_env_value(env, keys);
                return value.has_value() && !value->empty();
            };
        }
        keyPresence_ = new KeyPresence(std::move(lookup), context.io(), context.ui(), this);
        QPointer<KeyPresence> presence = keyPresence_;
        auto summary = [presence](const Json& settings) {
            return providerSummary(settings, [presence](const std::string& env) {
                return presence != nullptr && presence->saved(env);
            });
        };
        guided->setProviderSummary(summary);
        // A lookup that came back draws the row again.
        QPointer<GuidedSetup> guardedGuided = guided;
        connect(keyPresence_, &KeyPresence::changed, this, [guardedGuided, summary] {
            if (guardedGuided != nullptr) {
                guardedGuided->setProviderSummary(summary);
            }
        });
        // The saved provider's key is looked up now, so the row knows it before setup asks.
        if (const std::string env = cleanupKeyVariable(*context.settings().snapshot());
            !env.empty()) {
            keyPresence_->check(env);
        }
        if (auto* signals = context.find<SettingsPagesSignals>()) {
            connect(signals, &SettingsPagesSignals::apiKeysChanged, keyPresence_,
                    &KeyPresence::recheck);
        }
    }
}

// The Settings window as the M4 pages reach it: its profile changes and capture pauses come
// through CleanupSettings, and its Hotkeys page lists the profile and transform shortcuts.
void PersonalizeWiring::wireDialog(AppContext& context, SettingsDialog* dialog) {
    if (dialog == nullptr) {
        return;
    }
    for (const QPointer<SettingsDialog>& known : wired_) {
        if (known.data() == dialog) {
            return;
        }
    }
    wired_.emplace_back(dialog);
    if (auto* cleanup = context.find<CleanupSettings>()) {
        cleanup->attach(*dialog);
    }
    // _on_agent_looked_up (settings_dialog.py:657, 3259-3264): Commands and the rail read an
    // agent's readiness without waiting for it, so a lookup that lands refreshes them.
    if (AgentLookupRelay* relay = agentLookupRelay()) {
        connect(relay, &AgentLookupRelay::lookedUp, dialog,
                [dialog] { dialog->refreshPage(destinations::commands); });
    }
    // The Hotkeys page is built on first use; its rows are registered before anyone reads it.
    QPointer<SettingsDialog> guarded = dialog;
    auto registered = std::make_shared<QPointer<HotkeysPage>>();
    const auto wire = [guarded, registered] {
        if (guarded == nullptr) {
            return;
        }
        const std::string key(destinations::hotkeys);
        if (!guarded->isRegistered(key) || !guarded->isBuilt(key)) {
            return;
        }
        auto* page = qobject_cast<HotkeysPage*>(guarded->pageContent(key));
        if (page == nullptr || page == registered->data()) {
            return;
        }
        *registered = page;
        // _on_binding_shortcut_saved (settings_dialog.py:2818-2830): the profile editor follows
        // the saved shortcut (a draft keeps its own edits), then on_cleanup_profiles_changed; a
        // transform's page re-reads, then notify_changed("transforms").
        PersonalBindingHooks hooks;
        hooks.profilesSaved = [guarded] {
            if (guarded == nullptr) {
                return;
            }
            if (guarded->isBuilt(std::string(destinations::cleanup_profiles))) {
                guarded->refreshPage(destinations::cleanup_profiles);
            }
            guarded->notifyChanged(QStringLiteral("cleanup_profiles"));
        };
        hooks.transformsSaved = [guarded] {
            if (guarded == nullptr) {
                return;
            }
            guarded->refreshPage(destinations::commands);
            guarded->notifyChanged(QStringLiteral("transforms"));
        };
        registerPersonalBindingSources(*page, std::move(hooks));
        if (!guarded->isLoading()) {
            page->loadSettings(*guarded->settingsSnapshot());
        }
    };
    // The Profiles and Commands editors check a shortcut against the saved actions too
    // (hotkey_conflicts.hotkey_conflict reads load_actions(settings)); fv_text cannot load them,
    // so the editors are given them as bindings, read from the latest saved settings.
    SettingsStore* store = &context.settings();
    const auto actionBindings = [store] {
        std::vector<hotkeys::Binding> bindings;
        for (const automation::Action& action : automation::load_actions(*store->snapshot())) {
            bindings.push_back({action.id, action.name, action.hotkey});
        }
        return bindings;
    };
    const auto bindEditors = [guarded, actionBindings] {
        if (guarded == nullptr) {
            return;
        }
        for (TransformsPanel* panel : guarded->findChildren<TransformsPanel*>()) {
            panel->setActionBindings(actionBindings);
        }
        for (CleanupProfilesPanel* panel : guarded->findChildren<CleanupProfilesPanel*>()) {
            panel->setActionBindings(actionBindings);
        }
    };
    for (const auto& step : {std::function<void()>(wire), std::function<void()>(bindEditors)}) {
        connect(dialog, &SettingsDialog::destinationChanged, this, step);
        connect(dialog, &SettingsDialog::windowShown, this, step);
        if (QStackedWidget* stack = dialog->stack()) {
            connect(stack, &QStackedWidget::widgetAdded, this, step);
        }
        step();
    }
}

bool ruleDictationBusy(AppContext& context) {
    const SettingsWindow* window = context.find<SettingsWindow>();
    SettingsDialog* dialog = window != nullptr ? window->dialog() : nullptr;
    if (dialog == nullptr) {
        return false;
    }
    for (const RulesPage* page : dialog->findChildren<RulesPage*>()) {
        if (page->section() != nullptr &&
            page->section()->dictationState() != CleanupRulesSection::DictationState::Idle) {
            return true;
        }
    }
    return false;
}

} // namespace fv
