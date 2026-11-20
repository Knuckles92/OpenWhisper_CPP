#include "ui_setup/openwhisper_import.h"

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include <QFileDialog>
#include <QWidget>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "backup/coordinator.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "models/component_details.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.data_import"};

QString legacyProduct() {
    return qt::toQString(legacy_identity().product);
}

// The findings of one discovery, on the worker.
ImportFindings discover(const OpenWhisperImport::Ports& ports, models::Discovery& discovery,
                        std::optional<backup::ImportSource> source) {
    ImportFindings findings;
    if (!source && ports.findSources) {
        try {
            const auto sources = ports.findSources();
            if (!sources.empty()) {
                source = sources.front();
            }
        } catch (const std::exception&) {
            kLog.warning("Could not look for the data folder to import");
        }
    }
    findings.source = std::move(source);
    if (findings.source && ports.running) {
        try {
            findings.running = ports.running(findings.source->root);
        } catch (const std::exception&) {
            findings.running = false;
        }
    }
    if (ports.discoverModels) {
        try {
            discovery = ports.discoverModels();
        } catch (const std::exception&) {
            kLog.warning("Could not look for the speech runtime and model to adopt");
            discovery = {};
        }
    }
    for (const auto& component : discovery.components) {
        const auto* details = models::find_component_details(component.component_id);
        findings.runtimes.push_back(
            qt::toQString(details != nullptr ? details->display_name : component.component_id));
    }
    for (const auto& model : discovery.models) {
        findings.models.push_back(qt::toQString(model.label.empty() ? model.key : model.label));
    }
    return findings;
}

} // namespace

// What a posted task needs to reach the service: cleared on the GUI thread when it goes, and
// read only there.
struct OpenWhisperImport::Shared {
    OpenWhisperImport* owner = nullptr;
    // The newest discovery; an older one that finishes later is dropped.
    std::uint64_t generation = 0;
};

OpenWhisperImport::OpenWhisperImport(SettingsStore& store, Ports ports, Executor& worker,
                                     Executor& ui, QObject* parent)
    : QObject(parent), store_(store), ports_(std::move(ports)), worker_(worker), ui_(ui),
      shared_(std::make_shared<Shared>()) {
    shared_->owner = this;
}

OpenWhisperImport::OpenWhisperImport(AppContext& context)
    : OpenWhisperImport(context.settings(), portsFor(context), context.jobs(), context.ui()) {}

OpenWhisperImport::~OpenWhisperImport() {
    shared_->owner = nullptr;
    staged_.disconnect();
    // The window belongs to its parent window, which may outlive this service.
    delete dialog_.data();
}

bool OpenWhisperImport::available() const {
    return findings_.source.has_value();
}

bool OpenWhisperImport::imported() const {
    const Json record = store_.get(keys::fv_import);
    if (!record.is_object()) {
        return false;
    }
    const auto at = record.find("at");
    return at != record.end() && !(at->is_null() || (at->is_string() && at->empty()));
}

bool OpenWhisperImport::offered() const {
    return available() && !imported();
}

void OpenWhisperImport::refresh() {
    const std::uint64_t token = ++shared_->generation;
    std::weak_ptr<Shared> weak = shared_;
    worker_.post([weak, token, ports = ports_, ui = &ui_, picked = pickedRoot_] {
        std::optional<backup::ImportSource> chosen;
        if (picked && ports.inspectSource) {
            try {
                chosen = ports.inspectSource(*picked);
            } catch (const std::exception&) {
                chosen.reset();
            }
        }
        const bool pickedGone = picked.has_value() && !chosen.has_value();
        models::Discovery discovery;
        ImportFindings findings = discover(ports, discovery, std::move(chosen));
        ui->post([weak, token, pickedGone, findings = std::move(findings),
                  discovery = std::move(discovery)] {
            const auto shared = weak.lock();
            if (!shared || shared->owner == nullptr || shared->generation != token) {
                return;
            }
            if (pickedGone) {
                shared->owner->pickedRoot_.reset();
            }
            shared->owner->discovery_ = discovery;
            shared->owner->applyFindings(findings);
        });
    });
}

void OpenWhisperImport::applyFindings(const ImportFindings& findings) {
    const bool was = available();
    findings_ = findings;
    discovered_ = true;
    if (dialog_ != nullptr && !importing_) {
        dialog_->setFindings(findings_);
    }
    if (was != available()) {
        Q_EMIT availabilityChanged(available());
    }
}

void OpenWhisperImport::open(QWidget* parent) {
    if (dialog_ == nullptr) {
        dialog_ = new OpenWhisperImportDialog(parent);
        connect(dialog_, &OpenWhisperImportDialog::importRequested, this,
                [this](bool copyKeys) { startImport(copyKeys); });
        connect(dialog_, &OpenWhisperImportDialog::chooseFolderRequested, this,
                [this] { chooseFolder(); });
    }
    if (!importing_) {
        if (discovered_) {
            dialog_->setFindings(findings_);
        } else {
            dialog_->setLooking();
        }
        refresh();
    }
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
}

void OpenWhisperImport::chooseFolder() {
    if (dialog_ == nullptr || importing_ || !ports_.pickFolder) {
        return;
    }
    const QString folder = ports_.pickFolder(dialog_);
    if (folder.isEmpty()) {
        return;
    }
    const std::filesystem::path root = base_utf8::path_from_utf8(qt::toUtf8(folder));
    const std::uint64_t token = ++shared_->generation;
    std::weak_ptr<Shared> weak = shared_;
    worker_.post([weak, token, root, ports = ports_, ui = &ui_] {
        std::optional<backup::ImportSource> source;
        if (ports.inspectSource) {
            try {
                source = ports.inspectSource(root);
            } catch (const std::exception&) {
                source.reset();
            }
        }
        models::Discovery discovery;
        ImportFindings findings;
        if (source) {
            findings = discover(ports, discovery, std::move(source));
        }
        ui->post([weak, token, root, found = findings.source.has_value(), findings,
                  discovery = std::move(discovery)] {
            const auto shared = weak.lock();
            if (!shared || shared->owner == nullptr || shared->generation != token) {
                return;
            }
            OpenWhisperImport& self = *shared->owner;
            if (!found) {
                if (self.dialog_ != nullptr) {
                    self.dialog_->setError(QStringLiteral("No %1 data was found in that folder.")
                                               .arg(legacyProduct()));
                }
                return;
            }
            self.pickedRoot_ = root;
            self.discovery_ = discovery;
            self.applyFindings(findings);
        });
    });
}

void OpenWhisperImport::startImport(bool copyKeys) {
    if (importing_ || !findings_.source || dialog_ == nullptr) {
        return;
    }
    if (ports_.coordinator == nullptr || ports_.coordinator() == nullptr) {
        dialog_->setError(QStringLiteral("Importing is not available right now."));
        return;
    }
    importing_ = true;
    // A discovery still running would only overwrite what the window shows.
    ++shared_->generation;
    dialog_->setError(QString());
    dialog_->setBusy(true);
    const std::filesystem::path source = findings_.source->root;
    std::weak_ptr<Shared> weak = shared_;
    worker_.post([weak, adopt = ports_.adopt, discovery = discovery_, source, copyKeys, ui = &ui_] {
        // The runtime and model are this app's own once linked: adopting them does not wait for
        // the restart, and a failure leaves them to be downloaded later.
        if (adopt) {
            try {
                adopt(discovery);
            } catch (const std::exception&) {
                kLog.warning("Could not adopt the speech runtime or model");
            }
        }
        ui->post([weak, source, copyKeys] {
            const auto shared = weak.lock();
            if (!shared || shared->owner == nullptr) {
                return;
            }
            OpenWhisperImport& self = *shared->owner;
            backup::BackupCoordinator* coordinator =
                self.ports_.coordinator ? self.ports_.coordinator() : nullptr;
            if (coordinator == nullptr) {
                self.finishImport("", "Importing is not available right now.");
                return;
            }
            const bool wasBusy = coordinator->busy();
            self.staged_ = coordinator->restore_staged.connect(
                self.ui_, [&self](const std::string& stage, const std::string& error) {
                    if (self.ownsJob_) {
                        backup::BackupCoordinator* current =
                            self.ports_.coordinator ? self.ports_.coordinator() : nullptr;
                        // The coordinator is idle again before it reports its own job; while it
                        // is busy, what arrives is another caller's refusal.
                        if (current != nullptr && current->busy()) {
                            return;
                        }
                    }
                    self.finishImport(stage, error);
                });
            coordinator->prepare_import(source, copyKeys);
            // A refusal of this request has already ended the import (importing_ is false).
            self.ownsJob_ = !wasBusy && self.importing_ && coordinator->busy();
        });
    });
}

void OpenWhisperImport::finishImport(const std::string& stage, const std::string& error) {
    if (!importing_) {
        return;
    }
    importing_ = false;
    ownsJob_ = false;
    staged_.disconnect();
    if (dialog_ == nullptr) {
        return;
    }
    dialog_->setBusy(false);
    if (!stage.empty()) {
        kLog.info("The import is staged; the app restarts to apply it");
        dialog_->accept();
        return;
    }
    dialog_->setError(qt::toQString(error.empty() ? std::string("The import failed.") : error));
}

OpenWhisperImport::Ports OpenWhisperImport::portsFor(AppContext& context) {
    Ports ports;
    const Paths* paths = &context.paths();
    ports.findSources = [paths] {
        return backup::find_openwhisper_data(*paths);
    };
    ports.inspectSource = [](const std::filesystem::path& root) {
        return backup::inspect_import_source(root);
    };
    ports.running = [](const std::filesystem::path& root) {
        return backup::openwhisper_running(root);
    };
    ports.discoverModels = [paths] {
        return models::discover_openwhisper(*paths);
    };
    ports.adopt = [paths](const models::Discovery& discovery) {
        for (const auto& component : discovery.components) {
            try {
                models::adopt_component(*paths, component.component_id);
            } catch (const std::exception&) {
                kLog.warning("Could not adopt the runtime {}", component.component_id);
            }
        }
        for (const auto& model : discovery.models) {
            try {
                models::adopt_model(*paths, model.key);
            } catch (const std::exception&) {
                kLog.warning("Could not adopt the speech model {}", model.key);
            }
        }
    };
    ports.coordinator = [&context] {
        return context.find<backup::BackupCoordinator>();
    };
    ports.pickFolder = [](QWidget* parent) {
        return QFileDialog::getExistingDirectory(
            parent, QStringLiteral("Choose the %1 data folder").arg(legacyProduct()));
    };
    return ports;
}

} // namespace fv
