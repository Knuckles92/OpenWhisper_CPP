#include "ui_widgets/local_engine_controls.h"

#include <algorithm>

#include <QComboBox>
#include <QHBoxLayout>

#include "models/model_details.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_widgets/engine_field.h"

namespace fv {
namespace {

const QString kModelHelp = QStringLiteral(
    "The speech model turns audio into text. Larger models generally improve accuracy "
    "but need more memory and processing time. Use Downloads to install models for this backend.");
const QString kDeviceHelp =
    QStringLiteral("Choose the hardware used for transcription. Auto picks an available device. "
                   "GPU uses an NVIDIA graphics card. CPU uses your main processor.");
const QString kLanguageHelp =
    QStringLiteral("Choose a language supported by this model, or Auto for language detection. "
                   "Coverage and accuracy vary by model; this transcribes rather than translates.");

const FieldHelpLink kVoiceModelLink{QStringLiteral(u"Open Settings → Voice model"),
                                    QStringLiteral("ondemand")};
const FieldHelpLink kDownloadsLink{QStringLiteral(u"Open Settings → Downloads"),
                                   QStringLiteral("downloads")};

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

// The dict setting's saved mapping as the copy settings change.
Json savedMapping(const Json& snapshot, const DictSetting& setting) {
    Json mapping = setting.read(snapshot);
    return mapping.is_object() ? mapping : Json::object();
}

} // namespace

LocalEngineControls::LocalEngineControls(SettingsStore& settings, LocalEngineCatalog catalog,
                                         const QString& backend, bool dictation, QWidget* parent)
    : QWidget(parent), settings_(settings), catalog_(std::move(catalog)), dictation_(dictation) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    modelCombo_ = engineCombo({});
    deviceCombo_ = engineCombo({});
    languageCombo_ = engineCombo({});
    // QComboBox keeps the first minimum size hint it computes. The Language combo is measured
    // while it has no items, as OpenWhisper's is (its items come with the backend), so it can
    // shrink to the width of an empty combo and the row shares its room as OpenWhisper's does.
    languageCombo_->minimumSizeHint();

    auto onHelp = [this](const QString& destination) {
        Q_EMIT helpRequested(destination);
    };
    // Model carries twice Device's share, matching the Backend field's, so Model reads as its
    // peer and the runtime knobs stay visibly secondary.
    layout->addWidget(engineField(QStringLiteral("Model"), modelCombo_, kModelHelp,
                                  {kVoiceModelLink, kDownloadsLink}, onHelp),
                      2);
    layout->addWidget(
        engineField(QStringLiteral("Device"), deviceCombo_, kDeviceHelp, {kVoiceModelLink}, onHelp),
        1);
    languageField_ = engineField(QStringLiteral("Language"), languageCombo_, kLanguageHelp,
                                 {kVoiceModelLink}, onHelp);
    layout->addWidget(languageField_, 1);
    languageField_->hide();

    setBackend(backend);

    const auto changed = [this] {
        onChanged();
    };
    connect(modelCombo_, &QComboBox::currentTextChanged, this, changed);
    connect(deviceCombo_, &QComboBox::currentTextChanged, this, changed);
    connect(languageCombo_, &QComboBox::currentTextChanged, this, changed);
}

void showDownloadSizes(QComboBox* combo) {
    for (int index = 0; index < combo->count(); ++index) {
        // itemData(index) or itemText(index): the key is the item data, and the text stands in
        // for an item without one or with an empty one.
        QString key = combo->itemData(index).toString();
        if (key.isEmpty()) {
            key = combo->itemText(index);
        }
        const auto sizeMb = models::model_download_size_mb(key.toStdString());
        QVariant tip;
        if (sizeMb && *sizeMb != 0) {
            // format_download_mb(...).lstrip("~")
            QString size = toQString(models::format_download_mb(static_cast<double>(*sizeMb)));
            if (size.startsWith(QLatin1Char('~'))) {
                size.remove(0, 1);
            }
            tip = QStringLiteral("About %1 download").arg(size);
        }
        combo->setItemData(index, tip, Qt::ToolTipRole);
    }
}

void LocalEngineControls::setDictation(bool dictation) {
    dictation_ = dictation;
    syncLanguageField();
}

void LocalEngineControls::setBackend(const QString& backend) {
    backend_ = backend;
    const bool known = catalog_.knows(backend);
    const Json settings = *settings_.snapshot();

    deviceCombo_->blockSignals(true);
#ifdef Q_OS_MACOS
    // No CUDA on a Mac.
    setDeviceChoices(deviceCombo_, {QStringLiteral("auto"), QStringLiteral("cpu")});
#else
    setDeviceChoices(deviceCombo_,
                     {QStringLiteral("auto"), QStringLiteral("cuda"), QStringLiteral("cpu")});
#endif
    deviceCombo_->blockSignals(false);

    modelCombo_->blockSignals(true);
    modelCombo_->clear();
    if (known) {
        for (const auto& model : catalog_.models(backend)) {
            modelCombo_->addItem(model.label, model.key);
        }
        modelCombo_->setCurrentIndex(
            modelCombo_->findData(catalog_.selectedModel(settings, backend)));
        deviceCombo_->blockSignals(true);
        // A saved device this computer doesn't offer shows Auto.
        deviceCombo_->setCurrentIndex(
            std::max(0, deviceCombo_->findData(catalog_.selectedDevice(settings, backend))));
        deviceCombo_->blockSignals(false);
    }
    showDownloadSizes(modelCombo_);
    modelCombo_->blockSignals(false);

    syncLanguageField();
    languageCombo_->blockSignals(true);
    languageCombo_->clear();
    if (known) {
        const QString stored = toQString(keys::local_asr_language.read(settings));
        for (const auto& [code, label] : catalog_.languages(backend)) {
            languageCombo_->addItem(label, code);
        }
        languageCombo_->setCurrentIndex(
            languageCombo_->findData(catalog_.selectedLanguage(backend, stored)));
    }
    languageCombo_->blockSignals(false);
    languageCombo_->setEnabled(languageCombo_->count() > 1);
}

void LocalEngineControls::syncLanguageField() {
    bool shown = catalog_.knows(backend_);
    if (shown && dictation_ && catalog_.dictationDecidesLanguage) {
        // A one-language engine hides the dictation languages and keeps its own field.
        shown = !catalog_.dictationDecidesLanguage(*settings_.snapshot(), backend_);
    }
    languageField_->setVisible(shown);
}

void LocalEngineControls::loadFromSettings() {
    setBackend(backend_);
}

void LocalEngineControls::onChanged() {
    if (!catalog_.knows(backend_)) {
        return;
    }
    const Json snapshot = *settings_.snapshot();
    Json models = savedMapping(snapshot, keys::local_asr_models);
    Json devices = savedMapping(snapshot, keys::local_asr_devices);
    const std::string backend = backend_.toStdString();
    models[backend] = modelCombo_->currentData().toString().toStdString();
    devices[backend] = deviceCombo_->currentData().toString().toStdString();
    const QString language =
        catalog_.selectedLanguage(backend_, languageCombo_->currentData().toString());

    Json patch = Json::object();
    patch["local_asr_models"] = models;
    patch["local_asr_devices"] = devices;
    patch["local_asr_language"] = language.toStdString();
    settings_.update(patch);
    Q_EMIT engineSettingsChanged();
}

void LocalEngineControls::setBusy(bool busy) {
    for (QComboBox* combo : {modelCombo_, deviceCombo_, languageCombo_}) {
        combo->setEnabled(!busy);
    }
}

} // namespace fv
