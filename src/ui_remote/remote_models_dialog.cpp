#include "ui_remote/remote_models_dialog.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <map>

#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include "base/executor.h"
#include "base/identity.h"
#include "history/format_utils.h"
#include "models/model_details.h"
#include "remote/control.h"
#include "speech/dictation_language.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

const Json& member(const Json& object, std::string_view key) {
    static const Json empty;
    const auto found = object.find(std::string(key));
    return found == object.end() ? empty : *found;
}

std::string s(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

QString q(const Json& value) {
    return QString::fromStdString(s(value));
}

bool allowed(const Json& object, std::string_view key) {
    return member(object, key) == true;
}

// _model_state: the second line of a model's list entry.
QString modelState(const Json& model, bool active) {
    if (active) {
        return QStringLiteral("Active");
    }
    if (allowed(model, "cached")) {
        return QStringLiteral("Downloaded");
    }
    const QString size = q(member(model, "download_size"));
    return size.isEmpty() ? QStringLiteral("Not downloaded") : size;
}

// _engine_summary: one line naming the host's engine, its device, and any GPU.
QString engineSummary(const Json& engine, const Json& runtime) {
    const Json& gpu = member(runtime, "gpu");
    QString hardware;
    if (gpu.is_object() && !gpu.empty()) {
        const Json& total = member(gpu, "total_mib");
        const double mib = total.is_number() ? total.get<double>() : 0.0;
        hardware = QStringLiteral(u" · %1 (%2 GB VRAM)")
                       .arg(q(member(gpu, "name")), QString::number(mib / 1024.0, 'g', 6));
    }
    const QString raw = q(member(engine, "device"));
    const QString device = raw.isEmpty() ? QStringLiteral("device not reported") : deviceLabel(raw);
    const QString label = q(member(engine, "label"));
    return QStringLiteral(u"%1: %2 · %3%4")
        .arg(allowed(engine, "available") ? QStringLiteral("Running") : QStringLiteral("Not ready"),
             label.isEmpty() ? QStringLiteral("No engine") : label, device, hardware);
}

QString keyOf(const Json& model) {
    return q(member(model, "family")) + QLatin1Char(':') + q(member(model, "model"));
}

WrappedLabel* plainLabel(const QString& text) {
    auto* label = new WrappedLabel(text);
    label->setTextFormat(Qt::PlainText);
    return label;
}

// The paired host's models and speech runtimes, independent of this computer's storage
// (ui_qt/dialogs/remote_models.py RemoteModelsDialog): a searchable list, an inspector with
// Download, Run on host, Install runtime and Use on host, and the host's download and install
// progress. FioriVoce adds the host's speech language, which OpenWhisper sets from the engine
// card only. Requests run on `io`; their answers come back on `ui` to the dialog if it is still
// open, and to the host the dialog was opened for only (its fingerprint).
class RemoteModelsDialog final : public QDialog {
public:
    bool isFor(std::string_view fingerprint) const { return fingerprint_ == fingerprint; }

    RemoteModelsDialog(QWidget* parent, const QString& host, std::string fingerprint,
                       std::shared_ptr<remote::RemoteEngineControl> service, Executor& io,
                       Executor& ui, std::function<void()> selected)
        : QDialog(parent), service_(std::move(service)), io_(io), ui_(ui),
          selected_(std::move(selected)), host_(host), fingerprint_(std::move(fingerprint)) {
        setObjectName(QStringLiteral("remoteModelsDialog"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QStringLiteral("Models on %1").arg(host));
        setMinimumSize(760, 570);
        resize(960, 700);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(22, 20, 22, 18);
        layout->setSpacing(14);
        auto* title = plainLabel(QStringLiteral("Models on %1").arg(host));
        title->setObjectName(QStringLiteral("downloadsTitle"));
        layout->addWidget(title);
        layout->addWidget(plainLabel(QStringLiteral(
            "Manage the host's models and speech runtimes here. Downloads use its network and "
            "storage and continue when this window closes. Engine changes apply to every "
            "connected client.")));
        engineLabel_ = plainLabel(QString());
        engineLabel_->setObjectName(QStringLiteral("remoteHostSummary"));
        layout->addWidget(engineLabel_);

        auto* splitter = new QSplitter(Qt::Horizontal);
        splitter->setChildrenCollapsible(false);
        auto* browser = new QWidget;
        auto* browse = new QVBoxLayout(browser);
        browse->setContentsMargins(0, 0, 8, 0);
        search_ = new QLineEdit;
        search_->setObjectName(QStringLiteral("remoteModelSearch"));
        search_->setPlaceholderText(QStringLiteral(u"Search host models…"));
        search_->setClearButtonEnabled(true);
        search_->setAccessibleName(QStringLiteral("Search host models"));
        connect(search_, &QLineEdit::textChanged, this, [this] { filterModels(); });
        browse->addWidget(search_);
        list_ = new QListWidget;
        list_->setObjectName(QStringLiteral("remoteModelList"));
        list_->setAccessibleName(QStringLiteral("Models available on the host"));
        connect(list_, &QListWidget::currentItemChanged, this, [this] { updateActions(); });
        browse->addWidget(list_);
        emptyLabel_ = plainLabel(QStringLiteral("No matching models"));
        emptyLabel_->setObjectName(QStringLiteral("remoteModelEmpty"));
        emptyLabel_->hide();
        browse->addWidget(emptyLabel_);
        splitter->addWidget(browser);

        auto* inspector = new QFrame;
        inspector->setObjectName(QStringLiteral("remoteModelInspector"));
        inspector->setMinimumWidth(320);
        auto* details = new QVBoxLayout(inspector);
        details->setContentsMargins(18, 16, 18, 16);
        details->setSpacing(12);
        modelTitle_ = plainLabel(QStringLiteral("Choose a model"));
        modelTitle_->setObjectName(QStringLiteral("remoteModelTitle"));
        details->addWidget(modelTitle_);
        description_ = plainLabel(QString());
        details->addWidget(description_);
        detail_ = plainLabel(QString());
        details->addWidget(detail_);
        download_ = new Button(QStringLiteral("Download model on host"));
        download_->setObjectName(QStringLiteral("remoteModelDownloadButton"));
        connect(download_, &QPushButton::clicked, this, [this] { download(); });
        details->addWidget(download_);
        auto* deviceCaption = new QLabel(QStringLiteral("Run on host"));
        devices_ = new ElidingComboBox;
        devices_->setObjectName(QStringLiteral("remoteRuntimeChoice"));
        devices_->setAccessibleName(QStringLiteral("Device for selected host model"));
        deviceCaption->setBuddy(devices_);
        details->addWidget(deviceCaption);
        details->addWidget(devices_);
        connect(devices_, &QComboBox::currentIndexChanged, this, [this] { updateActions(); });
        runtimeDetail_ = plainLabel(QString());
        details->addWidget(runtimeDetail_);
        install_ = new Button(QStringLiteral("Install runtime on host"));
        install_->setObjectName(QStringLiteral("remoteRuntimeInstallButton"));
        connect(install_, &QPushButton::clicked, this, [this] { installRuntime(); });
        details->addWidget(install_);
        details->addStretch();
        select_ = new PrimaryButton(QStringLiteral("Use on host"));
        ui::setStyleProperty(select_, ui::prop::buttonId,
                             QStringLiteral("remoteModelSelectButton"));
        connect(select_, &QPushButton::clicked, this, [this] { selectModel(); });
        auto* inspectorScroll = new QScrollArea;
        inspectorScroll->setWidgetResizable(true);
        inspectorScroll->setFrameShape(QFrame::NoFrame);
        inspectorScroll->setWidget(inspector);
        inspectorScroll->setMinimumWidth(340);
        auto* inspectorColumn = new QWidget;
        auto* inspectorLayout = new QVBoxLayout(inspectorColumn);
        inspectorLayout->setContentsMargins(0, 0, 0, 0);
        inspectorLayout->addWidget(inspectorScroll, 1);
        inspectorLayout->addWidget(select_);
        splitter->addWidget(inspectorColumn);
        splitter->setSizes({360, 520});
        layout->addWidget(splitter, 1);

        // FioriVoce: the host's speech language, for the engine it runs now.
        auto* languageRow = new QHBoxLayout;
        auto* languageCaption = new QLabel(QStringLiteral("Host speech language"));
        languages_ = new QComboBox;
        languages_->setObjectName(QStringLiteral("remoteHostLanguage"));
        languageCaption->setBuddy(languages_);
        applyLanguage_ = new Button(QStringLiteral(u"Set host language…"));
        applyLanguage_->setObjectName(QStringLiteral("remoteSetHostLanguage"));
        languageRow->addWidget(languageCaption);
        languageRow->addWidget(languages_, 1);
        languageRow->addWidget(applyLanguage_);
        layout->addLayout(languageRow);
        connect(languages_, &QComboBox::currentIndexChanged, this, [this] { updateActions(); });
        connect(applyLanguage_, &QPushButton::clicked, this, [this] { setLanguage(); });

        status_ = plainLabel(QStringLiteral(u"Reading host models…"));
        status_->setObjectName(QStringLiteral("remoteModelStatus"));
        layout->addWidget(status_);
        downloadProgress_ = new QProgressBar;
        downloadProgress_->setAccessibleName(QStringLiteral("Host model download progress"));
        layout->addWidget(downloadProgress_);
        installStatus_ = plainLabel(QString());
        installStatus_->setObjectName(QStringLiteral("remoteInstallStatus"));
        installStatus_->hide();
        layout->addWidget(installStatus_);
        installProgress_ = new QProgressBar;
        installProgress_->setAccessibleName(QStringLiteral("Host runtime installation progress"));
        layout->addWidget(installProgress_);
        downloadProgress_->hide();
        installProgress_->hide();
        auto* footer = new QHBoxLayout;
        refresh_ = new Button(QStringLiteral("Refresh host"));
        connect(refresh_, &QPushButton::clicked, this, [this] { request("model_catalog"); });
        footer->addWidget(refresh_);
        footer->addStretch();
        auto* close = new Button(QStringLiteral("Close"));
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        footer->addWidget(close);
        layout->addLayout(footer);
        for (QPushButton* button :
             {static_cast<QPushButton*>(download_), static_cast<QPushButton*>(install_),
              static_cast<QPushButton*>(select_), static_cast<QPushButton*>(applyLanguage_),
              static_cast<QPushButton*>(refresh_), static_cast<QPushButton*>(close)}) {
            button->setAutoDefault(false);
        }

        poll_ = new QTimer(this);
        poll_->setSingleShot(true);
        poll_->setInterval(kPollMs);
        connect(poll_, &QTimer::timeout, this, [this] { request("model_catalog"); });
        updateActions();
        request("model_catalog");
    }

    // How often a download or an installation running on the host is read again.
    static constexpr int kPollMs = 2000;

private:
    bool confirm(const QString& title, const QString& text) {
        QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No,
                        this);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::No);
        return box.exec() == QMessageBox::Yes;
    }

    // _choice: the catalog entry of the selected list row; an empty object for none.
    const Json& choice() const {
        static const Json empty = Json::object();
        const QListWidgetItem* item = list_->currentItem();
        if (item == nullptr) {
            return empty;
        }
        const QString key = item->data(Qt::UserRole).toString();
        const Json& models = member(catalog_, "models");
        if (models.is_array()) {
            for (const Json& model : models) {
                if (keyOf(model) == key) {
                    return model;
                }
            }
        }
        return empty;
    }

    // _dependency: the selected model's runtime for the device chosen to run it on.
    const Json& dependency() const {
        static const Json empty = Json::object();
        const Json& options = member(choice(), "dependencies");
        const QString device = devices_->currentData().toString();
        if (options.is_array()) {
            for (const Json& option : options) {
                if (q(member(option, "device")) == device) {
                    return option;
                }
            }
        }
        return empty;
    }

    void filterModels() {
        const QString query = search_->text().trimmed().toCaseFolded();
        QListWidgetItem* firstVisible = nullptr;
        for (int row = 0; row < list_->count(); ++row) {
            QListWidgetItem* item = list_->item(row);
            item->setHidden(!item->text().toCaseFolded().contains(query));
            if (!item->isHidden() && firstVisible == nullptr) {
                firstVisible = item;
            }
        }
        emptyLabel_->setVisible(firstVisible == nullptr && valid_);
        QListWidgetItem* current = list_->currentItem();
        if (current == nullptr || current->isHidden()) {
            if (firstVisible != nullptr) {
                list_->setCurrentItem(firstVisible);
            } else {
                list_->setCurrentRow(-1);
            }
        }
    }

    void updateActions() {
        const Json& selected = choice();
        const bool chosen = !selected.empty();
        const QString key = chosen ? keyOf(selected) : QString();
        const Json& options = member(selected, "dependencies");
        const bool hasOptions = options.is_array() && !options.empty();
        QString device = deviceModel_ == key ? devices_->currentData().toString() : QString();
        if (device.isEmpty()) {
            device = q(member(selected, "selected_device"));
        }
        if (device != QLatin1String("cpu") && device != QLatin1String("cuda")) {
            device = QStringLiteral("cpu");
            if (hasOptions) {
                for (auto it = options.rbegin(); it != options.rend(); ++it) {
                    if (allowed(*it, "ready")) {
                        device = q(member(*it, "device"));
                        break;
                    }
                }
            }
        }
        deviceModel_ = key;
        {
            const QSignalBlocker blocker(devices_);
            devices_->clear();
            if (hasOptions) {
                for (const Json& option : options) {
                    const QString state = allowed(option, "ready") ? QStringLiteral("Ready")
                                          : allowed(option, "installable")
                                              ? QStringLiteral("Setup needed")
                                              : QStringLiteral("Unavailable");
                    devices_->addItem(QStringLiteral(u"%1 · %2")
                                          .arg(deviceLabel(q(member(option, "device"))), state),
                                      q(member(option, "device")));
                }
            }
            const int index = devices_->findData(device);
            if (index >= 0) {
                devices_->setCurrentIndex(index);
            }
        }
        devices_->setVisible(hasOptions);
        const Json& runtime = dependency();
        const bool enabled = valid_ && !busy_ && chosen;
        list_->setEnabled(!busy_);
        devices_->setEnabled(enabled);
        refresh_->setEnabled(!busy_);
        const Json& job = member(catalog_, "download");
        const Json& install = member(catalog_, "installation");
        const bool cached = allowed(selected, "cached");
        download_->setEnabled(enabled && !cached && s(member(job, "state")) != "downloading");
        download_->setText(cached ? QStringLiteral("Model downloaded")
                                  : QStringLiteral("Download model on host"));
        download_->setVisible(!cached);
        install_->setEnabled(enabled && allowed(catalog_, "can_install_runtime") &&
                             allowed(runtime, "installable") &&
                             s(member(install, "state")) != "installing");
        install_->setVisible(!runtime.empty() && !allowed(runtime, "ready"));
        const Json& engine = member(catalog_, "engine");
        const bool active =
            chosen && allowed(engine, "available") &&
            member(selected, "family") == member(engine, "family") &&
            member(selected, "model") == member(engine, "model") &&
            (runtime.empty() || member(runtime, "device") == member(engine, "device"));
        const bool ready =
            hasOptions ? allowed(runtime, "ready") : allowed(selected, "runtime_ready");
        select_->setEnabled(enabled && cached && ready && allowed(catalog_, "can_select") &&
                            !active);
        select_->setText(active ? QStringLiteral("Active on host") : QStringLiteral("Use on host"));
        const QString label = q(member(selected, "label"));
        modelTitle_->setText(label.isEmpty() ? QStringLiteral("Choose a model") : label);
        QString description;
        if (chosen) {
            // Older clients can still manage newly added host models.
            if (const auto* profile = models::find_model_details(s(member(selected, "model")))) {
                description =
                    QString::fromStdString(profile->description + "\n" + profile->language_support);
            }
        }
        description_->setText(description);
        if (!chosen) {
            detail_->setText(QString());
        } else if (cached) {
            detail_->setText(QStringLiteral("Model downloaded on host"));
        } else {
            const QString size = q(member(selected, "download_size"));
            detail_->setText(QStringLiteral("Model download: %1")
                                 .arg(size.isEmpty() ? QStringLiteral("size unknown") : size));
        }
        if (!runtime.empty()) {
            QString text = allowed(runtime, "ready")
                               ? QStringLiteral(u"%1 · Ready").arg(q(member(runtime, "label")))
                               : q(member(runtime, "reason"));
            if (text.isEmpty()) {
                const Json& bytes = member(runtime, "download_bytes");
                const QString size =
                    bytes.is_number() && bytes.get<double>() > 0
                        ? QString::fromStdString(format_size_bytes(bytes.get<std::int64_t>()))
                        : QStringLiteral("size unknown");
                text = QStringLiteral("%1 runtime is missing. Install it on the host (%2) to use "
                                      "this device.")
                           .arg(q(member(runtime, "label")), size);
            }
            runtimeDetail_->setText(text);
        } else {
            runtimeDetail_->setText(
                chosen ? QStringLiteral(u"Update %1 on the host to manage its CPU/GPU runtimes "
                                        u"here. Missing runtimes can be installed in Settings → "
                                        u"Downloads on the host.")
                             .arg(QString::fromUtf8(identity().product))
                       : QString());
        }
        const Json& active_runtime = member(catalog_, "active_runtime");
        const bool canSet = valid_ && !busy_ && allowed(catalog_, "can_configure_runtime") &&
                            member(active_runtime, "family").is_string() &&
                            !q(member(active_runtime, "model")).isEmpty() &&
                            languages_->count() > 1;
        languages_->setEnabled(canSet);
        applyLanguage_->setEnabled(canSet && languages_->currentIndex() >= 0 &&
                                   languages_->currentData().toString() !=
                                       q(member(member(active_runtime, "selected"), "language")));
    }

    void download() {
        const Json selected = choice();
        if (!download_->isEnabled() || selected.empty()) {
            return;
        }
        const QString size = q(member(selected, "download_size"));
        if (confirm(QStringLiteral("Download on host?"),
                    QStringLiteral("Download %1 (%2) on %3? This uses the host's network and disk "
                                   "space.")
                        .arg(q(member(selected, "label")),
                             size.isEmpty() ? QStringLiteral("size unknown") : size, host_))) {
            request("download_model", Json{{"family", member(selected, "family")},
                                           {"model", member(selected, "model")}});
        }
    }

    void installRuntime() {
        const Json selected = choice();
        const Json runtime = dependency();
        if (!install_->isEnabled() || runtime.empty()) {
            return;
        }
        if (confirm(QStringLiteral("Install runtime on host?"),
                    QStringLiteral("Install %1 on %2? This downloads verified runtime files using "
                                   "the host's network and disk space.")
                        .arg(q(member(runtime, "label")), host_))) {
            request("install_runtime", Json{{"family", member(selected, "family")},
                                            {"model", member(selected, "model")},
                                            {"device", member(runtime, "device")}});
        }
    }

    void selectModel() {
        const Json selected = choice();
        if (!select_->isEnabled() || selected.empty()) {
            return;
        }
        const QString device = devices_->currentData().toString();
        const QString where =
            device.isEmpty() ? QString() : QStringLiteral(" using %1").arg(deviceLabel(device));
        if (confirm(QStringLiteral("Change the host's engine?"),
                    QStringLiteral("Use %1 on %2%3? This changes the engine for the host and "
                                   "every connected client.")
                        .arg(q(member(selected, "label")), host_, where))) {
            Json fields{{"family", member(selected, "family")},
                        {"model", member(selected, "model")}};
            if (!device.isEmpty() && allowed(catalog_, "can_install_runtime")) {
                fields["device"] = device.toStdString();
            }
            request("select_model", fields);
        }
    }

    void setLanguage() {
        const Json& runtime = member(catalog_, "active_runtime");
        const auto language = languages_->currentData().toString().toStdString();
        const Json fields{{"family", member(runtime, "family")},
                          {"model", member(runtime, "model")},
                          {"settings", {{"language", language}}}};
        const auto modelName = q(member(member(catalog_, "engine"), "label"));
        if (confirm(QStringLiteral("Set host speech language"),
                    QStringLiteral("Set the speech language for %1 on %2 to %3? This changes the "
                                   "engine's default for the host and its paired computers.")
                        .arg(modelName, host_, languages_->currentText()))) {
            request("configure_runtime", fields);
        }
    }

    void request(std::string op, Json fields = Json::object()) {
        if (busy_) {
            return;
        }
        poll_->stop();
        busy_ = true;
        updateActions();
        if (op != "model_catalog" || !valid_) {
            static const std::map<std::string, QString> kStarting{
                {"model_catalog", QStringLiteral(u"Reading host models…")},
                {"download_model", QStringLiteral(u"Starting model download…")},
                {"install_runtime", QStringLiteral(u"Starting runtime installation…")},
                {"select_model", QStringLiteral(u"Loading model on host…")},
                {"configure_runtime", QStringLiteral(u"Setting the host's language…")},
            };
            const auto found = kStarting.find(op);
            if (found != kStarting.end()) {
                status_->setText(found->second);
            }
        }
        QPointer<RemoteModelsDialog> dialog(this);
        io_.post([dialog, ui = &ui_, service = service_, op = std::move(op),
                  fields = std::move(fields), expected = pairing_, fingerprint = fingerprint_] {
            Json result;
            QString error;
            auto pairing = expected;
            try {
                if (!pairing) {
                    pairing = service->client_pairing();
                }
                if (!pairing) {
                    throw remote::RemoteEngineError("Pair with a host before managing its models.");
                }
                if (fingerprint.empty() || pairing->fingerprint != fingerprint) {
                    throw remote::RemoteEngineError(
                        "The paired host changed. Close this window and open it again.");
                }
                result = service->remote_model_request(op, fields, pairing);
            } catch (const std::exception& failure) {
                error = QString::fromUtf8(failure.what());
            }
            ui->post([dialog, op, pairing = std::move(pairing), result = std::move(result),
                      error = std::move(error)]() mutable {
                if (dialog) {
                    dialog->finish(op, std::move(pairing), std::move(result), error);
                }
            });
        });
    }

    // _progress: shows `bar` while `job` is in `busyState`, determinate when it has a total.
    static bool progress(QProgressBar* bar, const Json& job, std::string_view busyState) {
        const bool busy = s(member(job, "state")) == busyState;
        bar->setVisible(busy);
        const Json& done = member(job, "done");
        const Json& total = member(job, "total");
        const double totalValue = total.is_number() ? total.get<double>() : 0.0;
        const double doneValue = done.is_number() ? done.get<double>() : 0.0;
        bar->setRange(0, totalValue > 0 ? 100 : 0);
        if (totalValue > 0) {
            bar->setValue(std::min(100, static_cast<int>(100 * doneValue / totalValue)));
        }
        return busy;
    }

    void finish(const std::string& op, std::optional<remote::ClientPairing> pairing, Json result,
                const QString& error) {
        busy_ = false;
        pairing_ = std::move(pairing);
        if (!error.isEmpty()) {
            valid_ = false;
            status_->setText(error + QStringLiteral(" Refresh to check the host's current state."));
        } else if (op == "model_catalog") {
            applyCatalog(std::move(result));
        } else {
            if ((op == "select_model" || op == "configure_runtime") && selected_) {
                selected_();
            }
            request("model_catalog");
        }
        updateActions();
    }

    void applyCatalog(Json result) {
        valid_ = true;
        const Json previous = choice();
        const QString key = previous.empty() ? QString() : keyOf(previous);
        catalog_ = result.is_object() ? std::move(result) : Json::object();
        const Json& engine = member(catalog_, "engine");
        populateModels(engine, previous.empty(), key);
        filterModels();
        engineLabel_->setText(engineSummary(engine, member(catalog_, "runtime")));
        populateLanguages();
        const Json& job = member(catalog_, "download");
        const Json& installation = member(catalog_, "installation");
        installStatus_->setVisible(installation.is_object() && !installation.empty());
        const bool downloading = progress(downloadProgress_, job, "downloading");
        const bool installing = progress(installProgress_, installation, "installing");
        showDownloadStatus(job, downloading);
        showInstallStatus(installation, installing);
        if (isVisible() && (downloading || installing)) {
            poll_->start();
        }
    }

    // _populate_models: refills the list, keeping the scroll position and the selection.
    void populateModels(const Json& engine, bool nothingChosen, const QString& key) {
        const QSignalBlocker blocker(list_);
        const int scroll = list_->verticalScrollBar()->value();
        list_->clear();
        QListWidgetItem* selected = nullptr;
        const Json& models = member(catalog_, "models");
        if (models.is_array()) {
            for (const Json& model : models) {
                const bool active = allowed(engine, "available") &&
                                    member(model, "family") == member(engine, "family") &&
                                    member(model, "model") == member(engine, "model");
                auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(
                    q(member(model, "label")), modelState(model, active)));
                item->setData(Qt::UserRole, keyOf(model));
                list_->addItem(item);
                if (keyOf(model) == key) {
                    selected = item;
                } else if (nothingChosen && active) {
                    selected = item;
                }
            }
        }
        if (selected != nullptr) {
            list_->setCurrentItem(selected);
        } else if (list_->count() > 0) {
            list_->setCurrentRow(0);
        }
        list_->verticalScrollBar()->setValue(scroll);
    }

    void populateLanguages() {
        const QSignalBlocker blocker(languages_);
        languages_->clear();
        const Json& runtime = member(catalog_, "active_runtime");
        const Json& choices = member(runtime, "languages");
        if (choices.is_array()) {
            for (const auto& entry : choices) {
                if (const auto code = s(entry); !code.empty()) {
                    languages_->addItem(QString::fromStdString(speech::label(code)), q(entry));
                }
            }
        }
        languages_->setCurrentIndex(
            languages_->findData(q(member(member(runtime, "selected"), "language"))));
    }

    void showDownloadStatus(const Json& job, bool downloading) {
        const QString label = q(member(job, "label"));
        if (downloading) {
            const Json& total = member(job, "total");
            const QString percent = total.is_number() && total.get<double>() > 0
                                        ? QStringLiteral(" (%1%)").arg(downloadProgress_->value())
                                        : QString();
            status_->setText(QStringLiteral(u"Downloading %1 on host%2…")
                                 .arg(label.isEmpty() ? QStringLiteral("model") : label, percent));
        } else if (s(member(job, "state")) == "failed") {
            const QString failure = q(member(job, "error"));
            status_->setText(failure.isEmpty()
                                 ? QStringLiteral("Model download failed. Retry the download.")
                                 : failure);
        } else if (s(member(job, "state")) == "complete") {
            status_->setText(QStringLiteral("%1 downloaded. Choose a device, then Use on host.")
                                 .arg(label.isEmpty() ? QStringLiteral("Model") : label));
        } else {
            status_->setText(
                QStringLiteral("Download a model, prepare its device, then choose Use on host."));
        }
    }

    void showInstallStatus(const Json& installation, bool installing) {
        const QString label = q(member(installation, "label"));
        const QString state = q(member(installation, "state"));
        if (installing) {
            const QString phase = q(member(installation, "phase"));
            installStatus_->setText(
                QStringLiteral(u"%1 · %2…")
                    .arg(label.isEmpty() ? QStringLiteral("Runtime") : label,
                         phase.isEmpty() ? QStringLiteral("Installing") : phase));
        } else if (state == QLatin1String("failed") || state == QLatin1String("restart_required")) {
            const QString failure = q(member(installation, "error"));
            installStatus_->setText(
                failure.isEmpty() ? QStringLiteral("Runtime setup needs attention on the host.")
                                  : failure);
        } else if (state == QLatin1String("complete")) {
            installStatus_->setText(QStringLiteral("%1 installed. Ready to use on the host.")
                                        .arg(label.isEmpty() ? QStringLiteral("Runtime") : label));
        } else {
            installStatus_->clear();
        }
    }

    std::shared_ptr<remote::RemoteEngineControl> service_;
    Executor& io_;
    Executor& ui_;
    std::function<void()> selected_;
    const QString host_;
    const std::string fingerprint_;
    std::optional<remote::ClientPairing> pairing_;
    Json catalog_ = Json::object();
    bool busy_ = false;
    // The last catalog read answered; an error until the next one.
    bool valid_ = false;
    // The model whose devices the Run on host field lists.
    QString deviceModel_;
    WrappedLabel* engineLabel_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    WrappedLabel* emptyLabel_ = nullptr;
    WrappedLabel* modelTitle_ = nullptr;
    WrappedLabel* description_ = nullptr;
    WrappedLabel* detail_ = nullptr;
    WrappedLabel* runtimeDetail_ = nullptr;
    Button* download_ = nullptr;
    ElidingComboBox* devices_ = nullptr;
    Button* install_ = nullptr;
    PrimaryButton* select_ = nullptr;
    QComboBox* languages_ = nullptr;
    Button* applyLanguage_ = nullptr;
    WrappedLabel* status_ = nullptr;
    QProgressBar* downloadProgress_ = nullptr;
    WrappedLabel* installStatus_ = nullptr;
    QProgressBar* installProgress_ = nullptr;
    Button* refresh_ = nullptr;
    QTimer* poll_ = nullptr;
};

} // namespace

void openRemoteModelsDialog(QWidget* parent, const QString& host, std::string fingerprint,
                            std::shared_ptr<remote::RemoteEngineControl> service, Executor& io,
                            Executor& ui, std::function<void()> selected) {
    if (parent) {
        const auto existing = parent->findChildren<QDialog*>(QStringLiteral("remoteModelsDialog"),
                                                             Qt::FindDirectChildrenOnly);
        for (auto* child : existing) {
            auto* dialog = dynamic_cast<RemoteModelsDialog*>(child);
            if (!dialog || !dialog->isVisible()) {
                continue;
            }
            if (dialog->isFor(fingerprint)) {
                dialog->raise();
                dialog->activateWindow();
                return;
            }
            dialog->close();
        }
    }
    auto* dialog = new RemoteModelsDialog(parent, host, std::move(fingerprint), std::move(service),
                                          io, ui, std::move(selected));
    // Non-modal, as OpenWhisper's show(): Settings stays usable while the host's models load.
    dialog->show();
}

} // namespace fv
