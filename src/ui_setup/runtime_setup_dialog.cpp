#include "ui_setup/runtime_setup_dialog.h"

#include <cstdint>
#include <exception>
#include <format>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "engine/runtime.h"
#include "history/format_utils.h"
#include "models/catalog.h"
#include "models/speech_catalog.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_setup.runtime_setup"};

// services/component_catalog.py's display names of the NeMo-Speech.cpp runtimes.
QString runtimeName(std::string_view componentId) {
    if (componentId == models::component_id::kAsrNvidiaCpu) {
        return QStringLiteral("NVIDIA Speech CPU");
    }
    if (componentId == models::component_id::kAsrNvidiaCuda) {
        return QStringLiteral("NVIDIA Speech GPU");
    }
    if (componentId == models::component_id::kAsrNvidiaVulkan) {
        return QStringLiteral("NVIDIA Speech GPU (Vulkan)");
    }
    if (componentId == models::component_id::kAsrNvidiaMetal) {
        return QStringLiteral("NVIDIA Speech GPU (Metal)");
    }
    return qt::toQString(componentId);
}

QString product() {
    return qt::toQString(identity().product);
}

QString legacyProduct() {
    return qt::toQString(legacy_identity().product);
}

QString pathText(const std::filesystem::path& path) {
    return qt::toQString(base_utf8::path_to_utf8(path));
}

WrappedLabel* infoLabel(const QString& text, QWidget* parent = nullptr) {
    auto* label = new WrappedLabel(text, parent);
    label->setObjectName(QStringLiteral("infoLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel* sectionLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionLabel"));
    return label;
}

} // namespace

struct RuntimeSetupDialog::Mailbox {
    std::mutex mutex;
    RuntimeSetupDialog* dialog = nullptr;
    qt::TokenGate gate;

    // From the io executor: runs fn(dialog) on the GUI thread if the dialog still lives and
    // token is still the newest request when it runs.
    template <class Fn> void deliver(std::uint64_t token, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (dialog == nullptr) {
            return;
        }
        // The dialog cannot be destroyed while the lock is held, so post_task's QPointer is made
        // on a live object; from then on the QPointer drops the task if the dialog goes.
        gate.deliver(dialog, token,
                     [target = dialog, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }
};

RuntimeSetupDialog::RuntimeSetupDialog(std::shared_ptr<const RuntimeSetupPorts> ports,
                                       QWidget* parent)
    : QDialog(parent), ports_(std::move(ports)), mailbox_(std::make_shared<Mailbox>()) {
    mailbox_->dialog = this;
    setObjectName(QStringLiteral("runtimeSetupDialog"));
    setWindowTitle(QStringLiteral("Set up speech recognition"));
    setModal(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    auto* header = new QLabel(QStringLiteral("Set up speech recognition"));
    header->setObjectName(QStringLiteral("headerLabel"));
    root->addWidget(header);
    root->addWidget(infoLabel(
        QStringLiteral("%1 needs a speech runtime and a speech model on this computer. Use the "
                       "ones %2 already downloaded, or point %1 at your own.")
            .arg(product(), legacyProduct())));

    readinessLabel_ = new QLabel(QStringLiteral("Looking for the speech runtime and model…"));
    readinessLabel_->setObjectName(QStringLiteral("statusLabel"));
    root->addWidget(readinessLabel_);

    auto* offer = new Card;
    offer->cardLayout()->addWidget(
        sectionLabel(QStringLiteral("Use what %1 already downloaded").arg(legacyProduct())));
    offerInfoLabel_ = infoLabel(QStringLiteral("Looking for %1's downloads…").arg(legacyProduct()));
    offer->cardLayout()->addWidget(offerInfoLabel_);
    rowsLayout_ = new QVBoxLayout;
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(4);
    offer->cardLayout()->addLayout(rowsLayout_);
    auto* useRow = new QHBoxLayout;
    useRow->addStretch();
    useButton_ = new PrimaryButton(QStringLiteral("Use these"));
    useRow->addWidget(useButton_);
    offer->cardLayout()->addLayout(useRow);
    root->addWidget(offer);

    auto* ownRuntime = new Card;
    ownRuntime->cardLayout()->addWidget(sectionLabel(QStringLiteral("Use your own runtime")));
    ownRuntime->cardLayout()->addWidget(
        infoLabel(QStringLiteral("An unpacked NeMo-Speech.cpp release: the folder that holds %1.")
                      .arg(QString::fromLatin1(runtime_library_name()))));
    auto* folderRow = new QHBoxLayout;
    folderRow->addStretch();
    folderButton_ = new Button(QStringLiteral("Choose a NeMo-Speech.cpp folder…"));
    folderRow->addWidget(folderButton_);
    ownRuntime->cardLayout()->addLayout(folderRow);
    root->addWidget(ownRuntime);

    auto* ownModel = new Card;
    ownModel->cardLayout()->addWidget(sectionLabel(QStringLiteral("Use your own model")));
    ownModel->cardLayout()->addWidget(
        infoLabel(QStringLiteral("A .gguf file of Parakeet TDT 0.6B v3 or Orukeet TDT 0.6B.")));
    auto* modelRow = new QHBoxLayout;
    modelRow->addStretch();
    modelButton_ = new Button(QStringLiteral("Choose a model file…"));
    modelRow->addWidget(modelButton_);
    ownModel->cardLayout()->addLayout(modelRow);
    root->addWidget(ownModel);

    // Shown only while a step runs: an empty bar would read as work stuck at 0 %.
    progress_ = new AnimatedProgressBar;
    progress_->reset();
    progress_->setVisible(false);
    root->addWidget(progress_);

    resultLabel_ = new WrappedLabel(QString(), nullptr, true);
    resultLabel_->setObjectName(QStringLiteral("statusLabel"));
    resultLabel_->setWordWrap(true);
    resultLabel_->setVisible(false);
    root->addWidget(resultLabel_);

    auto* footer = new QHBoxLayout;
    footer->addStretch();
    laterButton_ = new Button(QStringLiteral("Later"));
    footer->addWidget(laterButton_);
    root->addLayout(footer);

    connect(useButton_, &QPushButton::clicked, this, [this] { useOffered(); });
    connect(folderButton_, &QPushButton::clicked, this, [this] { chooseFolder(); });
    connect(modelButton_, &QPushButton::clicked, this, [this] { chooseModel(); });
    connect(laterButton_, &QPushButton::clicked, this, [this] { reject(); });

    updateEnabled();
    look();
}

RuntimeSetupDialog::~RuntimeSetupDialog() {
    {
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->dialog = nullptr;
    }
    mailbox_->gate.next();
    cancel_.cancel();
}

void RuntimeSetupDialog::done(int result) {
    // Closed is final: what is still running stops, and a look or step that finishes later
    // neither reopens the question (a usable look would turn Later into Accepted) nor shows
    // anything.
    cancel_.cancel();
    mailbox_->gate.next();
    if (busy_) {
        setBusy(false);
    }
    QDialog::done(result);
}

QStringList RuntimeSetupDialog::offered() const {
    QStringList texts;
    for (const QLabel* row : rows_) {
        texts.push_back(row->text());
    }
    return texts;
}

QString RuntimeSetupDialog::readinessText() const {
    return readinessLabel_->text();
}

QString RuntimeSetupDialog::resultText() const {
    return resultLabel_->text();
}

RuntimeSetupDialog::Snapshot RuntimeSetupDialog::take(const RuntimeSetupPorts& ports) {
    Snapshot snapshot;
    try {
        snapshot.discovery = ports.discover();
    } catch (const std::exception& error) {
        kLog.error_with(
            error, std::format("Looking for {}'s downloads failed", legacy_identity().product));
    } catch (...) {
        kLog.error("Looking for {}'s downloads failed", legacy_identity().product);
    }
    const auto unresolved = [&snapshot](std::string message) {
        snapshot.resolved = {};
        snapshot.resolved.missing_message = std::move(message);
        snapshot.resolved.missing_state = models::EngineMissing::RuntimeMissing;
    };
    try {
        snapshot.resolved = ports.resolve();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Resolving the speech runtime and model failed");
        unresolved(error.what());
    } catch (...) {
        kLog.error("Resolving the speech runtime and model failed");
        unresolved("The speech runtime and model could not be checked.");
    }
    return snapshot;
}

void RuntimeSetupDialog::look() {
    const auto token = mailbox_->gate.next();
    ports_->io->post([ports = ports_, mailbox = mailbox_, token] {
        Snapshot snapshot = take(*ports);
        mailbox->deliver(token, [snapshot = std::move(snapshot)](RuntimeSetupDialog& dialog) {
            dialog.apply(snapshot);
        });
    });
}

void RuntimeSetupDialog::apply(const Snapshot& snapshot) {
    looked_ = true;
    discovery_ = snapshot.discovery;
    usable_ = snapshot.resolved.usable();

    const auto& resolved = snapshot.resolved;
    QStringList lines;
    lines.push_back(
        QStringLiteral("Speech runtime: %1")
            .arg(resolved.runtime_dir ? QStringLiteral("Ready") : QStringLiteral("Missing")));
    if (!resolved.model_key.empty()) {
        const QString label = qt::toQString(resolved.model_label);
        lines.push_back(
            (label.isEmpty() ? QStringLiteral("Speech model: %1")
                             : QStringLiteral("Speech model (") + label + QStringLiteral("): %1"))
                .arg(resolved.model_path ? QStringLiteral("Ready") : QStringLiteral("Missing")));
    }
    readinessLabel_->setText(lines.join(QLatin1Char('\n')));

    rebuildRows();
    updateEnabled();
    if (usable_) {
        accept();
    }
}

void RuntimeSetupDialog::rebuildRows() {
    for (QLabel* row : rows_) {
        rowsLayout_->removeWidget(row);
        delete row;
    }
    rows_.clear();

    for (const auto& component : discovery_.components) {
        rows_.push_back(
            infoLabel(runtimeName(component.component_id) + QStringLiteral(" runtime")));
    }
    for (const auto& model : discovery_.models) {
        rows_.push_back(infoLabel(QStringLiteral("%1 model (%2)")
                                      .arg(qt::toQString(model.label),
                                           qt::toQString(format_size_bytes(model.size_bytes)))));
    }
    for (QLabel* row : rows_) {
        rowsLayout_->addWidget(row);
    }
    offerInfoLabel_->setText(
        discovery_.empty()
            ? QStringLiteral("Nothing to reuse: %1 has no complete speech runtime or model on this "
                             "computer.")
                  .arg(legacyProduct())
            : QStringLiteral("%1 links or copies these into its own folders. %2's files are not "
                             "changed.")
                  .arg(product(), legacyProduct()));
}

void RuntimeSetupDialog::updateEnabled() {
    useButton_->setEnabled(!busy_ && !discovery_.empty());
    folderButton_->setEnabled(!busy_);
    modelButton_->setEnabled(!busy_);
}

void RuntimeSetupDialog::setBusy(bool busy) {
    busy_ = busy;
    if (busy) {
        progress_->setIndeterminate();
    } else {
        progress_->reset();
    }
    progress_->setVisible(busy);
    updateEnabled();
}

void RuntimeSetupDialog::runAction(const QString& busyText,
                                   std::function<std::string(const CancelToken&)> work) {
    if (busy_) {
        return;
    }
    setBusy(true);
    resultLabel_->setText(busyText);
    resultLabel_->setVisible(true);

    const auto token = mailbox_->gate.next();
    ports_->io->post([ports = ports_, mailbox = mailbox_, token, cancel = cancel_.token(),
                      work = std::move(work)] {
        Outcome outcome;
        try {
            outcome.message = work(cancel);
        } catch (const models::ComponentCanceled&) {
            outcome.canceled = true;
        } catch (const std::exception& error) {
            outcome.failed = true;
            outcome.message = error.what();
            kLog.warning("Speech setup step failed: {}", error.what());
        } catch (...) {
            outcome.failed = true;
            outcome.message = "Something went wrong. Try again.";
            kLog.warning("Speech setup step failed with a non-standard exception");
        }
        if (cancel.cancelled()) {
            return; // the dialog was closed: nobody is waiting for the result
        }
        Snapshot snapshot = take(*ports);
        mailbox->deliver(token,
                         [outcome = std::move(outcome), snapshot = std::move(snapshot)](
                             RuntimeSetupDialog& dialog) { dialog.finish(outcome, snapshot); });
    });
}

void RuntimeSetupDialog::finish(const Outcome& outcome, const Snapshot& snapshot) {
    setBusy(false);
    if (outcome.canceled || cancel_.cancelled()) {
        return;
    }
    QString text = qt::toQString(outcome.message);
    // What is still missing, unless the step itself failed (its sentence says why).
    if (!outcome.failed && snapshot.resolved.missing_message) {
        text += QLatin1Char(' ') + qt::toQString(*snapshot.resolved.missing_message);
    }
    resultLabel_->setText(text);
    resultLabel_->setVisible(true);
    apply(snapshot);
}

void RuntimeSetupDialog::useOffered() {
    if (discovery_.empty()) {
        return;
    }
    // Copies: the rows change while the worker runs.
    const auto components = discovery_.components;
    const auto modelsToAdopt = discovery_.models;
    runAction(
        QStringLiteral("Adopting what %1 downloaded…").arg(legacyProduct()),
        [ports = ports_, components, modelsToAdopt](const CancelToken& cancel) {
            QStringList adopted;
            std::vector<std::string> failures;
            const auto attempt = [&](const QString& name, auto&& adopt) {
                try {
                    adopt();
                    adopted.push_back(name);
                } catch (const models::ComponentCanceled&) {
                    throw;
                } catch (const std::exception& error) {
                    kLog.warning("Adopting {} failed: {}", qt::toUtf8(name), error.what());
                    failures.push_back("Couldn't adopt " + qt::toUtf8(name) + ": " + error.what());
                }
            };
            for (const auto& component : components) {
                attempt(runtimeName(component.component_id),
                        [&] { ports->adopt_component(component.component_id, cancel); });
            }
            for (const auto& model : modelsToAdopt) {
                attempt(qt::toQString(model.label), [&] { ports->adopt_model(model.key, cancel); });
            }
            if (!failures.empty()) {
                std::string joined;
                for (const auto& failure : failures) {
                    joined += (joined.empty() ? "" : "\n") + failure;
                }
                throw std::runtime_error(joined);
            }
            return qt::toUtf8(
                QStringLiteral("Adopted %1.").arg(adopted.join(QStringLiteral(", "))));
        });
}

void RuntimeSetupDialog::chooseFolder() {
    if (busy_) {
        return;
    }
    const auto chosen = ports_->pick_runtime_folder(this);
    if (!chosen) {
        return;
    }
    runAction(QStringLiteral("Checking the folder…"), [ports = ports_,
                                                       folder = *chosen](const CancelToken&) {
        find_runtime_library(folder); // throws EngineError naming the folder
        ports->save_runtime_dir(folder);
        std::string message = "Using the speech runtime in " + qt::toUtf8(pathText(folder)) + ".";
        if (ports->runtime_loaded && ports->runtime_loaded()) {
            message += " Restart " + std::string(identity().product) + " to switch to it.";
        }
        return message;
    });
}

void RuntimeSetupDialog::chooseModel() {
    if (busy_) {
        return;
    }
    const auto chosen = ports_->pick_model_file(this);
    if (!chosen) {
        return;
    }
    runAction(QStringLiteral("Checking the model file…"),
              [ports = ports_, file = *chosen](const CancelToken& cancel) {
                  const std::string key = ports->import_model_file(file, cancel);
                  const auto* known = models::model(key);
                  return "Imported " + std::string(known != nullptr ? known->label : key) + ".";
              });
}

} // namespace fv
