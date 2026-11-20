#include "ui_meeting/meeting_setup_dialog.h"

#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "meeting/platform_policy.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.meeting_setup"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

bool onLinux() {
    return meeting::current_platform().platform.starts_with("linux");
}

} // namespace

// What a step's thread may touch: never the dialog itself, which it reaches only through
// deliver(). Steps run one at a time, so each delivery belongs to the step under way.
struct MeetingSetupDialog::Mailbox {
    std::mutex mutex;
    MeetingSetupDialog* dialog = nullptr;

    // From the step's thread: runs fn(dialog) on the GUI thread if the dialog still lives then.
    template <class Fn> void deliver(Fn&& fn) {
        std::lock_guard lock(mutex);
        if (dialog == nullptr) {
            return;
        }
        // The dialog cannot be destroyed while the lock is held, so post_task's QPointer is made
        // on a live object; from then on it drops the task if the dialog goes.
        qt::post(dialog, [target = dialog, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }
};

MeetingSetupDialog::MeetingSetupDialog(meeting::SetupDeps deps, audio::Backend& backend,
                                       bool starting, QWidget* parent)
    : MeetingSetupDialog(std::move(deps), backend, starting, parent, Hooks{}) {}

MeetingSetupDialog::MeetingSetupDialog(meeting::SetupDeps deps, audio::Backend& backend,
                                       bool starting, QWidget* parent, Hooks hooks)
    : QDialog(parent), deps_(std::move(deps)), backend_(backend), hooks_(std::move(hooks)),
      mailbox_(std::make_shared<Mailbox>()) {
    mailbox_->dialog = this;
    if (!hooks_.inspect) {
        hooks_.inspect = [](const meeting::SetupDeps& setup) {
            return meeting::inspect_models(setup);
        };
    }
    if (!hooks_.prepare) {
        hooks_.prepare = [](const meeting::SetupDeps& setup, const CancelToken& cancel,
                            const std::function<void(std::string)>& status) {
            return meeting::prepare_models(setup, cancel, status);
        };
    }
    if (!hooks_.checkAudio) {
        hooks_.checkAudio = [](audio::Backend& devices, const meeting::SetupDeps& setup,
                               const CancelToken& cancel, bool allowed) {
            return meeting::check_audio(devices, setup, cancel, allowed);
        };
    }
    setWindowTitle(QStringLiteral("Set up Meeting Mode"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    WrappedLabel* title = label(QStringLiteral("Set up Meeting Mode"));
    title->setObjectName(QStringLiteral("headerLabel"));
    layout->addWidget(title);
    layout->addWidget(label(QStringLiteral(
        "Prepare your meeting speech and speaker models. Then speak and play sound through the "
        "output your calls use while the audio check runs. The test audio is discarded.")));
    modelStatus_ = label(QStringLiteral("Checking meeting models…"));
    layout->addWidget(modelStatus_);
    prepareButton_ = new Button(QStringLiteral("Download missing files"));
    connect(prepareButton_, &QPushButton::clicked, this, [this] { prepare(); });
    layout->addWidget(prepareButton_);

    audioStatus_ = label(QStringLiteral("Microphone and system audio have not been checked."));
    layout->addWidget(audioStatus_);
    audioButton_ = new Button(QStringLiteral("Check audio"));
    connect(audioButton_, &QPushButton::clicked, this, [this] { checkAudio(); });
    layout->addWidget(audioButton_);
    // Shown only where system audio needs a permission the user can grant (macOS).
    audioSettings_ = new Button(QStringLiteral("Open System Settings"));
    connect(audioSettings_, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl(q(meeting::kScreenRecordingSettingsUrl))); });
    audioSettings_->hide();
    layout->addWidget(audioSettings_);

    // Stack actions so enlarged fonts and the microphone-only action fit without squeezing
    // button labels in a narrow dialog.
    auto* footer = new QVBoxLayout();
    back_ = new Button(starting ? QStringLiteral("Back") : QStringLiteral("Close"));
    connect(back_, &QPushButton::clicked, this, &QDialog::reject);
    footer->addWidget(back_);
    skip_ = new Button(QStringLiteral("Start anyway"));
    skip_->setVisible(starting);
    connect(skip_, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(skip_);
    finish_ =
        new SuccessButton(starting ? QStringLiteral("Start Meeting") : QStringLiteral("Done"));
    connect(finish_, &QPushButton::clicked, this, [this] { finish(); });
    footer->addWidget(finish_);
    layout->addLayout(footer);

    run(
        [inspect = hooks_.inspect, deps = deps_](const CancelToken&,
                                                 const std::function<void(std::string)>&) -> Done {
            meeting::ModelSetup models = inspect(deps);
            return [models = std::move(models)](MeetingSetupDialog& dialog) {
                dialog.showModels(models);
            };
        },
        modelStatus_);
}

MeetingSetupDialog::~MeetingSetupDialog() {
    cancel_.cancel();
    // A step's thread holds the mailbox, never the dialog: from here nothing is delivered.
    const std::lock_guard lock(mailbox_->mutex);
    mailbox_->dialog = nullptr;
}

QString MeetingSetupDialog::systemAudioPolicy() const {
    if (permissionMissing_) {
        return QStringLiteral("disabled");
    }
    if (onLinux() && systemAudioVerified_) {
        return QStringLiteral("required");
    }
    return QStringLiteral("auto");
}

WrappedLabel* MeetingSetupDialog::label(const QString& text) {
    auto* made = new WrappedLabel(text, nullptr, true);
    made->setTextFormat(Qt::PlainText);
    return made;
}

void MeetingSetupDialog::run(
    std::function<Done(const CancelToken&, const std::function<void(std::string)>& status)> work,
    WrappedLabel* statusLabel) {
    busy_ = true;
    updateButtons();
    // The step holds plain values and the mailbox, so it may finish after the dialog has closed.
    Task task = [mailbox = mailbox_, work = std::move(work), cancel = cancel_.token(),
                 statusLabel] {
        const std::function<void(std::string)> status = [&mailbox, statusLabel](std::string text) {
            mailbox->deliver([statusLabel, text = std::move(text)](MeetingSetupDialog&) {
                statusLabel->setText(q(text));
            });
        };
        Done apply;
        std::optional<std::string> error;
        try {
            apply = work(cancel, status);
        } catch (const std::exception& failure) {
            error = failure.what();
        } catch (...) {
            error = std::string();
        }
        mailbox->deliver([apply = std::move(apply), error = std::move(error),
                          statusLabel](MeetingSetupDialog& dialog) {
            dialog.onStepFinished(apply, error, statusLabel);
        });
    };
    if (hooks_.worker != nullptr) {
        hooks_.worker->post(std::move(task));
    } else {
        std::thread(std::move(task)).detach();
    }
}

void MeetingSetupDialog::onStepFinished(const Done& apply, const std::optional<std::string>& error,
                                        WrappedLabel* statusLabel) {
    busy_ = false;
    checkingAudio_ = false;
    if (closed_) {
        // Python stops its poll on done(): a step that ends after the dialog closed is not shown.
        return;
    }
    if (pendingResult_.has_value()) {
        closed_ = true;
        QDialog::done(*pendingResult_);
        return;
    }
    if (error.has_value()) {
        kLog.warning("Meeting setup step failed: {}", *error);
        statusLabel->setText(QStringLiteral("Couldn't complete this step. %1").arg(q(*error)));
    } else {
        apply(*this);
    }
    updateButtons();
}

void MeetingSetupDialog::prepare() {
    modelStatus_->setText(QStringLiteral("Preparing meeting models…"));
    run(
        [prepare = hooks_.prepare, deps = deps_](
            const CancelToken& cancel, const std::function<void(std::string)>& status) -> Done {
            meeting::ModelSetup models = prepare(deps, cancel, status);
            return [models = std::move(models)](MeetingSetupDialog& dialog) {
                dialog.showModels(models);
            };
        },
        modelStatus_);
}

void MeetingSetupDialog::showModels(const meeting::ModelSetup& models) {
    const std::vector<std::string> missing = models.missing();
    modelsReady_ = missing.empty();
    QString text = QStringLiteral("Speech engine: %1\n").arg(q(models.label));
    if (!missing.empty()) {
        QStringList names;
        for (const std::string& name : missing) {
            names.append(q(name));
        }
        text += QStringLiteral("Still needed: ") + names.join(QStringLiteral(", "));
    } else {
        text += QStringLiteral("Required local files are ready.");
    }
    modelStatus_->setText(text);
}

void MeetingSetupDialog::checkAudio() {
    checkedAudio_ = false;
    audioReady_ = false;
    systemAudioVerified_ = false;
    bool allowed = true;
    if (meeting::system_audio_permission_required() &&
        !meeting::system_audio_permission_granted()) {
        allowed = meeting::request_system_audio_permission();
    }
    permissionMissing_ = !allowed;
    audioSettings_->setVisible(permissionMissing_);
    skip_->setText(permissionMissing_ ? QStringLiteral("Start with microphone only")
                                      : QStringLiteral("Start anyway"));
    audioStatus_->setText(
        QStringLiteral("Listening… Speak and play audio through your call output."));
    checkingAudio_ = true;
    run(
        [check = hooks_.checkAudio, deps = deps_, backend = &backend_,
         allowed](const CancelToken& cancel, const std::function<void(std::string)>&) -> Done {
            meeting::AudioCheck audio = check(*backend, deps, cancel, allowed);
            return [audio = std::move(audio)](MeetingSetupDialog& dialog) {
                dialog.showAudio(audio);
            };
        },
        audioStatus_);
}

void MeetingSetupDialog::showAudio(const meeting::AudioCheck& audio) {
    checkedAudio_ = true;
    audioReady_ = audio.ready();
    systemAudioVerified_ = audio.system_audio_verified();
    QString message = QStringLiteral("Microphone: %1\nSystem audio: %2")
                          .arg(q(audio.microphone), q(audio.system_audio));
    if (permissionMissing_) {
        message += QStringLiteral("\nGrant access in System Settings. You may need to relaunch "
                                  "%1 before retrying.")
                       .arg(q(identity().product));
    } else if (onLinux() && !systemAudioVerified_) {
        message += QStringLiteral("\nCheck the default output in Sound settings and make sure "
                                  "PulseAudio or PipeWire-Pulse is running.");
    }
    if (audio.microphone == "No signal detected") {
        message += QStringLiteral(
            "\nSpeak close to the microphone or turn up its input level, then retry.");
    }
    audioStatus_->setText(message);
}

void MeetingSetupDialog::updateButtons() {
    const bool idle = !busy_;
    prepareButton_->setEnabled(idle && !modelsReady_);
    audioButton_->setEnabled(idle);
    skip_->setEnabled(idle && checkedAudio_ && !(modelsReady_ && audioReady_));
    const bool ready = idle && modelsReady_ && audioReady_;
    finish_->setActive(ready);
    finish_->setEnabled(ready);
}

void MeetingSetupDialog::finish() {
    try {
        if (hooks_.markComplete) {
            hooks_.markComplete();
        } else if (hooks_.settings != nullptr) {
            hooks_.settings->save_setting(keys::meeting_setup_complete.key(), true);
        } else {
            throw std::logic_error("no settings store to save to");
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not save Meeting Mode setup completion: {}", error.what());
    }
    Q_EMIT setupCompleted();
    accept();
}

void MeetingSetupDialog::done(int result) {
    cancel_.cancel();
    // Keep the modal guard until the step has released the capture devices; otherwise Quick
    // Record or a meeting could race the closing check.
    if (checkingAudio_ && busy_) {
        pendingResult_ = result;
        audioStatus_->setText(QStringLiteral("Closing the audio check…"));
        setEnabled(false);
        return;
    }
    closed_ = true;
    QDialog::done(result);
}

} // namespace fv
