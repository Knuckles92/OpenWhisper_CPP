#include "ui_record/record_feature.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QStackedWidget>

#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "base/log.h"
#include "base/paths.h"
#include "clipboard/temporary_clipboard.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/transcription_runtime.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/rewrite.h"
#include "settings/store.h"
#include "speech/endpoints.h"
#include "ui_record/command_readiness.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/scratchpad.h"
#include "ui_record/scratchpad_transforms.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/tabbed_content.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.main_window"};

// The Scratchpad service over the notes file and the clipboard. A context without the paths,
// the persistence strand or the settings (a partial startup) has no Scratchpad.
std::shared_ptr<ScratchpadService> makeScratchpad(AppContext& context) {
    ScratchpadEnvironment environment;
    try {
        environment.settings = &context.settings();
        environment.persistence = &context.persistence();
        environment.file = [file = context.paths().user_file("scratchpad.txt")] {
            return file;
        };
    } catch (const ServiceUnavailable& error) {
        kLog.info("There is no Scratchpad: {}", error.what());
        return nullptr;
    }
    // UIController.copy_to_clipboard: the clipboard every delivery stages through.
    focus::ClipboardPort* clipboard = context.find<clipboard::TemporaryClipboard>();
    return std::make_shared<ScratchpadService>(
        std::move(environment), [clipboard](const QString& text) {
            return clipboard != nullptr && clipboard->write_text(qt::toUtf8(text));
        });
}

// The key sources text-model lookups read: the credential store and the .env file (with the
// process environment). The store is null when the context has no credentials or paths (a
// partial startup).
struct KeyAccess {
    CredentialStore* store = nullptr;
    std::shared_ptr<DotEnv> dotenv;
};

KeyAccess keyAccess(AppContext& context) {
    try {
        return {&context.credentials(), std::make_shared<DotEnv>(context.paths().env_file())};
    } catch (const ServiceUnavailable& error) {
        kLog.info("Text-model keys are not available: {}", error.what());
        return {};
    }
}

// The Scratchpad's Transform menu over the context's jobs strand and the AI cleanup model; null
// when the context lacks what it needs (the menu then says "No transforms yet").
std::shared_ptr<ScratchpadTransforms> makeTransforms(AppContext& context) {
    const KeyAccess keys = keyAccess(context);
    if (keys.store == nullptr) {
        return nullptr;
    }
    TransformEnvironment environment;
    try {
        environment.settings = &context.settings();
        environment.jobs = &context.jobs();
        environment.ui = &context.ui();
        // The client outlives every service of the context: the call gets a handle that owns
        // nothing.
        std::shared_ptr<http::Client> client(std::shared_ptr<void>(), &context.http());
        environment.rewrite = [store = keys.store, dotenv = keys.dotenv,
                               client](std::string_view text, std::string_view instruction,
                                       const Json& settings) {
            llm::KeySources sources{*store, system_env(), *dotenv};
            return llm::rewrite_standalone(text, instruction, settings, client, sources);
        };
    } catch (const ServiceUnavailable& error) {
        kLog.info("There are no Scratchpad transforms: {}", error.what());
        return nullptr;
    }
    return std::make_shared<ScratchpadTransforms>(std::move(environment));
}

} // namespace

RecordWidgets::RecordWidgets(SettingsStore& settings, Executor& ui)
    : settings_(settings), ui_(ui), engineSelection_(settings, ui) {}

RecordWidgets::~RecordWidgets() {
    readiness_.reset();
    // A widget the window took is the window's (it is destroyed after the features); one nothing
    // took would otherwise leak.
    if (quickRecord_ != nullptr && quickRecord_->parent() == nullptr) {
        delete quickRecord_.data();
    }
    if (compact_ != nullptr && compact_->parent() == nullptr) {
        delete compact_.data();
    }
    if (upload_ != nullptr && upload_->parent() == nullptr) {
        delete upload_.data();
    }
}

void RecordWidgets::setApiServerServices(http::Client* client, Executor* io,
                                         SpeechServerControls::KeyProvider keys) {
    client_ = client;
    io_ = io;
    keys_ = std::move(keys);
}

void RecordWidgets::addApiServer(TranscriptionTabBase& tab) {
    if (client_ == nullptr || io_ == nullptr) {
        return;
    }
    auto* controls = new SpeechServerControls(settings_, *client_, *io_);
    // Right after construction: the first catalog request went out before a key could be given.
    if (keys_) {
        controls->setKeyProvider(keys_);
    }
    tab.setApiServer(controls);
}

void RecordWidgets::setCommandReadiness(
    std::function<std::optional<bool>(const Json& settings)> probe, Executor& worker) {
    readiness_ = std::make_shared<CommandHintReadiness>(std::move(probe), worker, ui_);
    if (quickRecord_ != nullptr) {
        quickRecord_->setCommandModeReadiness(readiness_->supplier());
    }
    // The tab may be built after this: it is looked up when an answer lands.
    readiness_->setChanged([this] {
        if (quickRecord_ != nullptr) {
            quickRecord_->refreshCommandHint();
        }
    });
}

void RecordWidgets::refreshLivePreviewControls() {
    for (TranscriptionTabBase* view : engineSelection_.views()) {
        view->loadLivePreviewSetting();
    }
}

QuickRecordTab* RecordWidgets::quickRecord(QWidget* parent) {
    if (quickRecord_ == nullptr) {
        quickRecord_ = new QuickRecordTab(settings_, speechEngineCatalog(), parent);
        if (readiness_ != nullptr) {
            quickRecord_->setCommandModeReadiness(readiness_->supplier());
        }
        addApiServer(*quickRecord_);
        engineSelection_.add(quickRecord_);
        quickRecord_->syncEngineSelection();
    }
    return quickRecord_;
}

QuickRecordTab* RecordWidgets::existingQuickRecord() const {
    return quickRecord_;
}

UploadFileTab* RecordWidgets::existingUpload() const {
    return upload_;
}

UploadFileTab* RecordWidgets::upload(QWidget* parent) {
    if (upload_ == nullptr) {
        upload_ = new UploadFileTab(settings_, speechEngineCatalog(), parent);
        addApiServer(*upload_);
        engineSelection_.add(upload_);
        upload_->syncEngineSelection();
    }
    return upload_;
}

CompactRecordController* RecordWidgets::compactController(QWidget* parent) {
    if (compact_ == nullptr) {
        compact_ = new CompactRecordController(parent);
    }
    return compact_;
}

void RecordFeature::provide(AppContext& context) {
    auto widgets = std::make_shared<RecordWidgets>(context.settings(), context.ui());
    // The API backend's fields need the network and a key; a context without them (tests, a
    // partial startup) lists the backend with no fields.
    try {
        http::Client& client = context.http();
        Executor& io = context.io();
        SpeechServerControls::KeyProvider keys;
        try {
            CredentialStore& credentials = context.credentials();
            auto dotenv = std::make_shared<DotEnv>(context.paths().env_file());
            keys = [&credentials, dotenv](const speech::SpeechEndpoint& endpoint) {
                return speech::api_key(endpoint, credentials, system_env(), *dotenv);
            };
            // Command Mode's hint asks whether the AI model is set up: key lookups block, so the
            // io strand answers them.
            widgets->setCommandReadiness(
                [&credentials, dotenv](const Json& settings) {
                    llm::KeySources sources{credentials, system_env(), *dotenv};
                    return llm::provider_readiness(settings, sources);
                },
                io);
        } catch (const ServiceUnavailable& error) {
            kLog.info("API server catalogs are asked without a key: {}", error.what());
        }
        widgets->setApiServerServices(&client, &io, std::move(keys));
    } catch (const ServiceUnavailable& error) {
        kLog.info("The API backend shows no server fields: {}", error.what());
    }
    context.provide(std::move(widgets));
}

void RecordFeature::install(AppContext& context) {
    RecordWidgets* widgets = &context.service<RecordWidgets>();
    context.screens().define(
        kQuickRecordScreen, order::tab::quick_record, QStringLiteral("Quick Record"),
        [widgets](QWidget* parent) -> QWidget* { return widgets->quickRecord(parent); },
        kTranscriptionGroup);
    context.screens().define(
        kUploadScreen, order::tab::upload_file, QStringLiteral("Upload File"),
        [widgets](QWidget* parent) -> QWidget* { return widgets->upload(parent); },
        kTranscriptionGroup);

    UploadPresenter::Ports ports;
    if (auto* runtime = context.find<dictation::TranscriptionRuntime>()) {
        ports = UploadPresenter::runtimePorts(*runtime);
    }
    ports.clipboard = context.find<clipboard::TemporaryClipboard>();
    if (MainWindow* window = context.find<MainWindow>()) {
        ports.windowStatus = [window](const QString& status) {
            window->setStatus(status);
        };
        ports.tabParent = [window]() -> QWidget* {
            return window->tabbedContent()->stack();
        };
    }
    ports.quickRecordLoading = [widgets] {
        const QuickRecordTab* tab = widgets->existingQuickRecord();
        return tab != nullptr && tab->engineLoading();
    };
    context.provide(std::make_shared<UploadPresenter>(context.screens(), std::move(ports)));

    if (std::shared_ptr<ScratchpadService> scratchpad = makeScratchpad(context)) {
        // Dictation goes into a focused Scratchpad instead of the paste. The runtime was
        // provided before the service, so the context releases it after the service has let go.
        if (auto* runtime = context.find<dictation::TranscriptionRuntime>()) {
            runtime->set_scratchpad(scratchpad.get());
            scratchpad->setReleaseHook([runtime] { runtime->set_scratchpad(nullptr); });
        }
        if (std::shared_ptr<ScratchpadTransforms> transforms = makeTransforms(context)) {
            scratchpad->setTransformMenuFiller(transforms->filler());
        }
        // The Scratchpad hotkey (services/runtime/hotkeys.py connect_signals). The View menu and
        // the tray are the composition's: the controller wiring test holds them open until it
        // connects them through connectWindow and connectTray.
        if (auto* hotkeys = context.find<dictation::HotkeyRuntime>()) {
            scratchpad->connectHotkey(*hotkeys, context.ui());
        }
        context.provide(scratchpad);
    }
}

} // namespace fv
