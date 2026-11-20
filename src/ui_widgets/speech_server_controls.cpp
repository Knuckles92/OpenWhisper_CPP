#include "ui_widgets/speech_server_controls.h"

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
#include <typeinfo>

#include <QComboBox>
#include <QHBoxLayout>
#include <QStandardItemModel>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/log.h"
#include "http/client.h"
#include "settings/store.h"
#include "speech/api_models.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/speech_server_dialog.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.speech_server_controls"};

const FieldHelpLink kApiKeysLink{QStringLiteral(u"Open Settings → App → API keys"),
                                 qt::toQString(destinations::api_keys)};

using CatalogKey = std::pair<std::string, std::optional<std::string>>;

CatalogKey catalogKey(const speech::SpeechEndpoint& endpoint) {
    return {endpoint.id, endpoint.base_url};
}

// The catalogs of the session, per (endpoint id, address), shared by every instance. A worker
// stores a result here itself, so a catalog that arrives after its widget (or its request) is
// gone is kept for the next field.
class Catalogs {
public:
    std::optional<std::vector<std::string>> find(const CatalogKey& key) const {
        std::lock_guard lock(mutex_);
        const auto found = models_.find(key);
        if (found == models_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    void put(const CatalogKey& key, std::vector<std::string> models) {
        std::lock_guard lock(mutex_);
        models_[key] = std::move(models);
    }
    void erase(const CatalogKey& key) {
        std::lock_guard lock(mutex_);
        models_.erase(key);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        models_.clear();
    }
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return models_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<CatalogKey, std::vector<std::string>> models_;
};

Catalogs& catalogs() {
    static Catalogs shared;
    return shared;
}

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

// Combo data of an item, or "" for a separator or an empty combo (Python's None).
QString dataOf(const QComboBox* combo) {
    const QVariant data = combo->currentData();
    return data.isValid() ? data.toString() : QString();
}

} // namespace

// What a worker may touch: never the widget itself, which it reaches only through deliver().
struct SpeechServerControls::Mailbox {
    std::mutex mutex;
    SpeechServerControls* widget = nullptr;
    qt::TokenGate gate;

    // From the io executor: runs fn(widget) on the GUI thread if the widget still lives and token
    // is still the newest request when it runs.
    template <class Fn> void deliver(std::uint64_t token, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (widget == nullptr) {
            return;
        }
        // The widget cannot be destroyed while the lock is held, so post_task's QPointer is made
        // on a live object; from then on it drops the task if the widget goes.
        gate.deliver(widget, token,
                     [target = widget, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }
};

SpeechServerControls::SpeechServerControls(SettingsStore& settings, http::Client& client,
                                           Executor& io, QWidget* parent)
    : QWidget(parent), settings_(settings), client_(client), io_(io), clock_(&system_clock()),
      endpoint_(speech::selected_endpoint(*settings.snapshot())),
      mailbox_(std::make_shared<Mailbox>()) {
    mailbox_->widget = this;
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    serverCombo_ = engineCombo({});
    modelCombo_ = engineCombo({});
    const auto onHelp = [this](const QString& destination) {
        Q_EMIT helpRequested(destination);
    };
    serverField_ = engineField(
        QStringLiteral("Server"), serverCombo_,
        QStringLiteral("Choose where the API backend sends your audio: OpenAI, Groq, or a "
                       "speech server you run, such as Speaches, LocalAI, vLLM or whisper.cpp. "
                       "Audio sent to a server on this computer stays on it. Hosted services "
                       "need an API key."),
        {kApiKeysLink}, onHelp);
    modelField_ = engineField(
        QStringLiteral("Model"), modelCombo_,
        QStringLiteral("The speech model the server turns your audio into text with. A server "
                       "you run lists the models it has; choose Edit server… to enter another "
                       "model id."),
        {kApiKeysLink}, onHelp);
    row->addWidget(serverField_, 1);
    row->addWidget(modelField_, 1);
    connect(serverCombo_, &QComboBox::currentIndexChanged, this, [this] { onServerChanged(); });
    connect(modelCombo_, &QComboBox::currentIndexChanged, this, [this] { onModelChanged(); });
    refresh();
}

SpeechServerControls::~SpeechServerControls() {
    // After this no worker reaches the widget, and a request still on the wire stops.
    {
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->widget = nullptr;
    }
    cancel_.cancel();
}

std::size_t SpeechServerControls::catalogCount() {
    return catalogs().size();
}

void SpeechServerControls::resetCatalogs() {
    catalogs().clear();
}

std::chrono::year_month_day SpeechServerControls::today() const {
    return speech::local_today(*clock_);
}

void SpeechServerControls::setKeyProvider(KeyProvider provider) {
    keyProvider_ = std::move(provider);
    // A catalog the session holds, or a server without a catalog route, needs nothing; a missing
    // catalog is asked for again, and the newer request's answer is the one shown.
    if (endpoint_.is_openai() || !endpoint_.models_path || catalogs().find(catalogKey(endpoint_))) {
        return;
    }
    fetchModels();
}

void SpeechServerControls::setBusy(bool busy) {
    setEnabled(!busy);
}

void SpeechServerControls::refresh() {
    const Json settings = *settings_.snapshot();
    endpoint_ = speech::selected_endpoint(settings);
    const speech::SpeechEndpoint& endpoint = endpoint_;
    QComboBox* combo = serverCombo_;
    const bool blocked = combo->blockSignals(true);
    combo->clear();
    for (const speech::SpeechEndpoint& preset : speech::speech_endpoints()) {
        const speech::SpeechEndpoint configured = speech::configured_endpoint(preset.id, settings);
        // The Custom slot reads "Custom…" until it has an address.
        const QString name =
            configured.is_configured() ? toQString(preset.name) : QStringLiteral(u"Custom…");
        combo->addItem(name, toQString(preset.id));
        combo->setItemData(combo->count() - 1, toQString(preset.description), Qt::ToolTipRole);
    }
    combo->insertSeparator(combo->count());
    combo->addItem(QStringLiteral(u"Edit server…"), QString::fromLatin1(kEditOption));
    if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
        model->item(combo->count() - 1)->setEnabled(endpoint.configurable);
    }
    combo->setCurrentIndex(combo->findData(toQString(endpoint.id)));
    combo->blockSignals(blocked);
    combo->setToolTip(QStringLiteral("%1 %2").arg(
        toQString(endpoint.description), toQString(speech::audio_destination_sentence(endpoint))));
    showModels(toQString(speech::endpoint_model(endpoint, settings, today())));
}

void SpeechServerControls::showModels(const QString& current) {
    const speech::SpeechEndpoint endpoint = endpoint_;
    std::vector<std::string> models;
    if (endpoint.is_openai() || !endpoint.models_path) {
        // Curated or known lists: no request is made.
        models = speech::list_models(client_, endpoint, std::nullopt, CancelToken(), today());
    } else if (auto cached = catalogs().find(catalogKey(endpoint))) {
        models = std::move(*cached);
    } else {
        fetchModels();
    }
    const std::string currentId = current.toStdString();
    if (!endpoint.is_openai() && std::ranges::find(models, currentId) == models.end()) {
        // The chosen id stays listed: the server may not report it (yet).
        models.insert(models.begin(), currentId);
    }
    QComboBox* combo = modelCombo_;
    const bool blocked = combo->blockSignals(true);
    combo->clear();
    for (const std::string& model : models) {
        // Only OpenAI's ids carry its retirement dates.
        combo->addItem(endpoint.is_openai() ? toQString(speech::api_model_label(model, today()))
                                            : toQString(model),
                       toQString(model));
    }
    combo->setCurrentIndex(std::max(0, combo->findData(current)));
    combo->blockSignals(blocked);
    combo->setToolTip(QStringLiteral("%1 model. %2")
                          .arg(toQString(endpoint.name),
                               toQString(speech::audio_destination_sentence(endpoint))));
}

void SpeechServerControls::fetchModels() {
    const speech::SpeechEndpoint endpoint = endpoint_;
    const std::uint64_t token = mailbox_->gate.next();
    io_.post([mailbox = mailbox_, client = &client_, endpoint, provider = keyProvider_,
              cancel = cancel_.token(), today = today(), token] {
        std::optional<std::vector<std::string>> models;
        try {
            const std::optional<std::string> key = provider ? provider(endpoint) : std::nullopt;
            models = speech::list_models(*client, endpoint, key, cancel, today);
            catalogs().put(catalogKey(endpoint), *models);
        } catch (const std::exception& error) {
            kLog.info("Couldn't list {} models: {}", endpoint.name, typeid(error).name());
            models.reset();
        }
        mailbox->deliver(token, [endpoint, models](SpeechServerControls& controls) {
            controls.onModelsDone(endpoint, models);
        });
    });
}

void SpeechServerControls::onModelsDone(const speech::SpeechEndpoint& endpoint,
                                        const std::optional<std::vector<std::string>>& models) {
    // The catalog is already kept; a late one for another server is not shown.
    if (catalogKey(endpoint) != catalogKey(endpoint_)) {
        return;
    }
    if (!models) {
        modelCombo_->setToolTip(
            QStringLiteral("Couldn't reach %1 at %2 to list its models. Check that the server is "
                           "running, or choose Edit server…")
                .arg(toQString(endpoint.name), toQString(endpoint.base_url.value_or("None"))));
        return;
    }
    showModels(dataOf(modelCombo_));
}

void SpeechServerControls::restoreServer() {
    const bool blocked = serverCombo_->blockSignals(true);
    serverCombo_->setCurrentIndex(serverCombo_->findData(toQString(endpoint_.id)));
    serverCombo_->blockSignals(blocked);
}

void SpeechServerControls::onServerChanged() {
    const QVariant selected = serverCombo_->currentData();
    if (!selected.isValid()) {
        return;
    }
    const QString choice = selected.toString();
    if (choice == QLatin1String(kEditOption)) {
        restoreServer();
        edit(endpoint_, false);
        return;
    }
    const std::string id = choice.toStdString();
    const speech::SpeechEndpoint endpoint = speech::configured_endpoint(id, *settings_.snapshot());
    if (id == speech::kCustomEndpointId && !endpoint.is_configured()) {
        restoreServer();
        edit(endpoint, true);
        return;
    }
    try {
        settings_.mutate([&id](Json& all) { speech::save_selection(all, id); });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't save the speech server");
        restoreServer();
        return;
    }
    // Picking a server is the moment to ask it what it has now.
    catalogs().erase(catalogKey(endpoint));
    refresh();
    Q_EMIT changed();
}

void SpeechServerControls::edit(const speech::SpeechEndpoint& endpoint, bool select) {
    if (!endpoint.configurable) {
        return;
    }
    SpeechServerDialog dialog(settings_, endpoint, window());
    const bool accepted =
        editorRunner_ ? editorRunner_(dialog) : dialog.exec() == QDialog::Accepted;
    if (!accepted) {
        return;
    }
    if (select) {
        try {
            const std::string id = endpoint.id;
            settings_.mutate([&id](Json& all) { speech::save_selection(all, id); });
        } catch (const std::exception& error) {
            kLog.error_with(error, "Couldn't save the speech server");
            return;
        }
    }
    refresh();
    Q_EMIT changed();
}

void SpeechServerControls::onModelChanged() {
    const QString model = dataOf(modelCombo_);
    if (model.isEmpty()) {
        return;
    }
    try {
        const std::string id = endpoint_.id;
        const std::string chosen = model.toStdString();
        settings_.mutate([&](Json& all) { speech::save_model(all, id, chosen); });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't save the speech model");
        return;
    }
    Q_EMIT changed();
}

} // namespace fv
