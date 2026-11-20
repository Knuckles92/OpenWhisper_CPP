#include "ui_controller/action_wiring.h"
#include "appcore/cancel_router.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/dictation_bridge.h"
#include "automation/routing.h"
#include "base/env.h"
#include "base/log.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/transcription_runtime.h"
#include "focus/capture_service.h"
#include "llm/judge.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/clipboard_service.h"
#include "ui_controller/process_judge.h"
#include "ui_controller/ui_controller.h"
#include "ui_record/scratchpad.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace fv {
namespace {
constexpr log::Logger kLog{"services.application_controller"};
class Host final : public automation::ActionHost {
public:
    Host(UiController& ui, ClipboardService& clipboard, dictation::TranscriptionRuntime& runtime,
         focus::CaptureService& capture, ScratchpadService* scratchpad)
        : ui_(&ui), clipboard_(clipboard), runtime_(runtime), capture_(capture),
          scratchpad_(scratchpad) {}
    void set_status(std::string text) override {
        if (ui_)
            ui_->setStatus(qt::toQString(text));
    }
    void show_notice(std::string text, bool done) override {
        if (ui_)
            ui_->showNotice(qt::toQString(text), done);
    }
    bool show_in_scratchpad(std::string text) override {
        if (!scratchpad_)
            return false;
        showScratchpadText(*scratchpad_, qt::toQString(text));
        return true;
    }
    bool copy_to_clipboard(std::string text, std::string html) override {
        return clipboard_.copyToClipboard(qt::toQString(text), qt::toQString(html));
    }
    bool paste_text_now(std::string text, const dictation::DictationJob* job) override {
        return runtime_.paste_text_now(std::move(text), job);
    }
    dictation::PasteTarget paste_target(const dictation::DictationJob& job) override {
        return dictation::paste_target(&job, &capture_);
    }
    void discard_clipboard_prefetch() override { clipboard_.discardPrefetch(); }

private:
    QPointer<UiController> ui_;
    ClipboardService& clipboard_;
    dictation::TranscriptionRuntime& runtime_;
    focus::CaptureService& capture_;
    ScratchpadService* scratchpad_;
};

bool askUser(QWidget* parent, const automation::ActionDisclosure& disclosure) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Confirm action"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(
        QStringLiteral("Send this material to:\n%1").arg(qt::toQString(disclosure.destination)),
        &dialog);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* material = new QPlainTextEdit(&dialog);
    material->setReadOnly(true);
    material->setPlainText(qt::toQString(disclosure.material));
    layout->addWidget(material);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Send"));
    buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(640, 420);
    return dialog.exec() == QDialog::Accepted;
}
struct ConsentGate {
    std::mutex mutex;
    bool open = true;
};
automation::ConfirmedAutomation::Confirm consent(UiController& controller, Executor& executor,
                                                 std::shared_ptr<ConsentGate> gate) {
    QPointer<UiController> guarded(&controller);
    return [guarded, &executor, gate](const automation::ActionDisclosure& disclosure,
                                      const CancelToken& cancel) {
        if (cancel.cancelled())
            return false;
        auto answer = std::make_shared<std::promise<bool>>();
        auto future = answer->get_future();
        auto present = [guarded, disclosure, cancel, answer] {
            const bool accepted = guarded && !cancel.cancelled() && askUser(nullptr, disclosure);
            answer->set_value(accepted && !cancel.cancelled());
        };
        if (QThread::currentThread() == qApp->thread())
            present();
        else {
            std::lock_guard lock(gate->mutex);
            if (!gate->open)
                return false;
            try {
                executor.post(std::move(present));
            } catch (...) {
                return false;
            }
        }
        while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
            if (cancel.cancelled())
                return false;
        return future.get() && !cancel.cancelled();
    };
}
} // namespace

// Own every dependency used by an outstanding route, including after cancellation: the
// provider owns its environment, credential store and client.
struct RouteJudgeBundle {
    std::shared_ptr<llm::JudgeProvider> provider;
};

// After the recording or transcription (0) and an AI write-up (the walkthrough's, 20).
constexpr int kCancelActionsOrder = 30;

struct ActionWiring::Impl {
    dictation::TranscriptionRuntime& transcription;
    dictation::HotkeyRuntime& hotkeys;
    std::unique_ptr<Host> host;
    std::unique_ptr<automation::ActionRuntime> runtime;
    std::shared_ptr<automation::FilterLeases> leases;
    std::shared_ptr<RouteJudgeBundle> routeJudge;
    std::shared_ptr<ConsentGate> consentGate = std::make_shared<ConsentGate>();
    std::unique_ptr<automation::DictationAutomation> delegate;
    std::shared_ptr<automation::ConfirmedAutomation> port;
    std::vector<Subscription> links;
    // What the user's Cancel step reaches; cleared at close, since the router outlives this.
    std::shared_ptr<std::atomic<automation::ActionRuntime*>> cancelTarget =
        std::make_shared<std::atomic<automation::ActionRuntime*>>(nullptr);
    bool closed = false;
    Impl(AppContext& c, ApplicationController& app, UiController& ui, ClipboardService& clipboard)
        : transcription(c.service<dictation::TranscriptionRuntime>()),
          hotkeys(c.service<dictation::HotkeyRuntime>()) {
        auto http = std::shared_ptr<http::Client>(&c.http(), [](auto*) {});
        host =
            std::make_unique<Host>(ui, clipboard, transcription, c.service<focus::CaptureService>(),
                                   c.find<ScratchpadService>());
        auto* supplied = c.find<automation::ActionServices>();
        automation::ActionServices options = supplied ? *supplied : automation::ActionServices{};
        if (!options.http)
            options.http = http;
        EventSink* events = nullptr;
        try {
            events = &c.events();
        } catch (const ServiceUnavailable&) {
            events = &automation::bus();
        }
        runtime = std::make_unique<automation::ActionRuntime>(*host, c.settings(), c.credentials(),
                                                              *events, c.ui(), std::move(options));
        leases = std::make_shared<automation::FilterLeases>();
        // The Hooks page lists the leases the dictation filters read (the live API leases them).
        if (!c.find<automation::FilterLeases>())
            c.provide<automation::FilterLeases>(leases);
        routeJudge = std::make_shared<RouteJudgeBundle>();
        // The process's one provider; it owns what its judges read.
        routeJudge->provider = provideProcessJudge(c);
        delegate = std::make_unique<automation::DictationAutomation>(
            *runtime, *leases, *routeJudge->provider, *events, http);
        // A judge still asked after its dictation stopped waiting keeps its provider.
        delegate->keep_judges_alive(routeJudge);
        auto* custom = c.find<ActionConsent>();
        auto confirm = custom ? custom->confirm : consent(ui, c.ui(), consentGate);
        port = std::make_shared<automation::ConfirmedAutomation>(
            *delegate, *runtime, c.settings(), std::move(confirm),
            [bundle = routeJudge](const Json& settings) {
                const auto route = bundle->provider->route(settings);
                return route.name + "\n" + route.endpoint;
            },
            [weak = QPointer<UiController>(&ui)](std::string text) {
                if (weak)
                    weak->setStatus(qt::toQString(text));
            });
        links.push_back(hotkeys.action_record_requested.connect(
            c.ui(), [&app](std::string id) { app.toggle_action_recording(id); }));
        // toggle_route_recording: a press while recording stops; otherwise Route by voice must
        // be ready (routing.status, which reads the judge's key: never on the GUI thread), or the
        // shortcut says what it still needs instead of recording words that would be pasted.
        links.push_back(hotkeys.route_record_requested.connect(c.ui(), [this, &app, &c] {
            if (app.is_recording()) {
                app.toggle_route_recording();
                return;
            }
            auto gate = consentGate;
            auto bundle = routeJudge;
            SettingsStore* settings = &c.settings();
            Executor* ui = &c.ui();
            ApplicationController* controller = &app;
            try {
                c.io().post([gate, bundle, settings, ui, controller] {
                    const auto status =
                        automation::status(*settings->snapshot(), *bundle->provider);
                    ui->post([gate, status, controller] {
                        {
                            std::lock_guard lock(gate->mutex);
                            if (!gate->open)
                                return;
                        }
                        if (controller->is_recording())
                            return;
                        if (!status.ready) {
                            controller->status_update.emit(status.message);
                            return;
                        }
                        controller->toggle_route_recording();
                    });
                });
            } catch (const std::exception&) {
                kLog.warning("Couldn't check Route by voice before recording");
            }
        }));
        links.push_back(c.settings().watch(
            {"actions"}, c.ui(), [this](const Json&) { hotkeys.refresh_profile_hotkeys(); }));
        hotkeys.set_action_hotkeys(
            [this](const Json& settings) { return port->action_hotkeys(settings); });
        transcription.set_automation(port);
        // The user's Cancel (application_controller.py cancel and _cancel_actions): with no
        // recording, transcription or write-up to cancel, it stops the actions still running.
        // They say what they canceled; with none running the router says there was nothing.
        cancelTarget->store(runtime.get());
        try {
            c.cancel().add(kCancelActionsOrder, [target = cancelTarget] {
                automation::ActionRuntime* actions = target->load();
                return actions != nullptr && actions->cancel() > 0;
            });
        } catch (const ServiceUnavailable&) {
            // A context without the user's Cancel (a test's) reaches the actions only directly.
        }
    }
    void close() {
        if (closed)
            return;
        closed = true;
        {
            std::lock_guard lock(consentGate->mutex);
            consentGate->open = false;
        }
        cancelTarget->store(nullptr);
        links.clear();
        hotkeys.set_action_hotkeys({});
        transcription.set_automation(dictation::null_automation_port());
        runtime->close();
    }
    ~Impl() { close(); }
};
ActionWiring::ActionWiring(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ActionWiring::~ActionWiring() = default;
void ActionWiring::close() {
    impl_->close();
}
std::shared_ptr<ActionWiring> ActionWiring::make(AppContext& c, ApplicationController& app,
                                                 UiController& ui, ClipboardService& clipboard) {
    return std::shared_ptr<ActionWiring>(
        new ActionWiring(std::make_unique<Impl>(c, app, ui, clipboard)));
}
} // namespace fv
