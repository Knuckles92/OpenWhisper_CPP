#include "ui_personalize/personalize_feature.h"

#include <memory>

#include "appkit/app_context.h"
#include "base/env.h"
#include "base/log.h"
#include "base/paths.h"
#include "credentials/dotenv.h"
#include "llm/credentials.h"
#include "llm/local_servers.h"
#include "llm/rewrite.h"
#include "llm/thinking_levels.h"
#include "llm/transcript_cleanup.h"
#include "recorder/recorder.h"
#include "settings/store.h"
#include "text/cleanup_prompts.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/cleanup_page.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/commands_page.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_personalize/profiles_page.h"
#include "ui_personalize/rules_page.h"
#include "ui_personalize/snippets_page.h"
#include "ui_personalize/styles_page.h"
#include "ui_settings/home_rows.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

// The services the AI cleanup and Learned rules pages use, from what the context has; a
// missing one leaves its members empty.
void fillCleanupServices(AppContext& context, CleanupServices& services) {
    try {
        services.ui = &context.ui();
    } catch (const ServiceUnavailable& error) {
        kLog.info("AI cleanup pages run no background work: {}", error.what());
        return;
    }
    try {
        services.io = &context.io();
        services.jobs = &context.jobs();
    } catch (const ServiceUnavailable& error) {
        kLog.info("AI cleanup pages run no background work: {}", error.what());
    }
    services.codexAgentLevels = [](const std::string& model) {
        return llm::agent_levels("codex", model);
    };
    try {
        CredentialStore& credentials = context.credentials();
        http::Client& http = context.http();
        SettingsStore& store = context.settings();
        Executor* io = services.io;
        auto dotenv = std::make_shared<DotEnv>(context.paths().env_file());
        // The app's client outlives every request: it is borrowed, never owned.
        std::shared_ptr<http::Client> shared(&http, [](http::Client*) {});
        services.listModels = [&credentials, &http, dotenv](const std::string& provider,
                                                            const std::string& sort,
                                                            const Json& settings) {
            llm::KeySources keys{credentials, system_env(), *dotenv};
            return llm::list_cleanup_models(http, provider, sort, settings, keys);
        };
        services.credentialStatus = [&credentials, dotenv](const llm::TextLLMProfile& profile) {
            llm::KeySources keys{credentials, system_env(), *dotenv};
            return llm::credential_status(profile, keys);
        };
        if (io != nullptr) {
            services.detectServers = [&http, io](const std::vector<std::string>& urls) {
                return llm::detect_running_servers(http, urls, *io);
            };
        }
        services.polishRule = [&credentials, &store, shared,
                               dotenv](const std::string& raw, const std::string& provider,
                                       const std::string& model, const std::string& reasoning) {
            llm::KeySources keys{credentials, system_env(), *dotenv};
            llm::CleanupOptions options;
            options.default_prompt = text::legacy_default_prompt();
            return llm::polish_cleanup_rule(
                raw, text::polish_prompt(), shared, keys, [&store] { return *store.snapshot(); },
                provider, model, reasoning, {}, std::move(options));
        };
    } catch (const ServiceUnavailable& error) {
        kLog.info("AI cleanup reaches no text model: {}", error.what());
    }
    try {
        audio::Backend& audio = context.audio();
        const Paths& paths = context.paths();
        services.makeRecorder = [&audio, &paths](const std::vector<audio::DeviceKey>& priority,
                                                 const std::filesystem::path& output) {
            RecorderOptions options;
            options.output_file = output;
            options.device_priority = priority;
            return std::make_shared<Recorder>(audio, paths, std::move(options));
        };
    } catch (const ServiceUnavailable& error) {
        kLog.info("Rules cannot be dictated: {}", error.what());
    }
}

} // namespace

void PersonalizeFeature::provide(AppContext& context) {
    auto gates = std::make_shared<CleanupGates>(context.settings());
    context.provide<CleanupGates>(gates);
    auto cleanup = std::make_shared<CleanupSettings>(context, *gates);
    fillCleanupServices(context, cleanup->services());
    context.provide<CleanupSettings>(std::move(cleanup));
    // Whether Command Mode has a model reads the credential store, so it runs on the io
    // executor; a context without credentials (tests, a partial startup) answers "unknown".
    auto readiness = std::make_shared<CommandReadiness>();
    try {
        CredentialStore& credentials = context.credentials();
        Executor& io = context.io();
        auto dotenv = std::make_shared<DotEnv>(context.paths().env_file());
        readiness->setCheck(
            [&credentials, dotenv](const Json& settings) {
                llm::KeySources keys{credentials, system_env(), *dotenv};
                return llm::provider_readiness(settings, keys);
            },
            &io);
    } catch (const ServiceUnavailable& error) {
        kLog.info("Command Mode readiness is not checked: {}", error.what());
    }
    context.provide<CommandReadiness>(std::move(readiness));
}

void PersonalizeFeature::install(AppContext& context) {
    registerCleanupPage(context);
    registerRulesPage(context);
    registerDictionaryPage(context);
    registerSnippetsPage(context);
    registerStylesPage(context);
    registerProfilesPage(context);
    registerCommandsPage(context);
    // Home asks the page modules for their rows in the order Settings registers them.
    if (context.find<HomeRows>() == nullptr) {
        context.provide<HomeRows>(std::make_shared<HomeRows>());
    }
    HomeRows& rows = context.service<HomeRows>();
    rows.addRows(HomeCardId::Dictation, addCleanupHomeRows);
    rows.addRows(HomeCardId::Personalize, addDictionaryHomeRows);
    rows.addRows(HomeCardId::Personalize, addSnippetsHomeRows);
    rows.addRows(HomeCardId::Personalize, addStylesHomeRows);
    rows.addRows(HomeCardId::Personalize, addCommandsHomeRows);
}

} // namespace fv
