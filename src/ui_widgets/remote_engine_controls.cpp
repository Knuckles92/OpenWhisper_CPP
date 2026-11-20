#include "ui_widgets/remote_engine_controls.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QStandardItemModel>
#include <QStringList>

#include "base/identity.h"
#include "speech/languages.h"
#include "ui_widgets/engine_field.h"

namespace fv {
namespace {

const QString kDeviceTip =
    QStringLiteral("Choose the hardware on the remote computer. Auto chooses an available device; "
                   "GPU uses its graphics card; CPU uses its processor.");
const QString kQuantTip = QStringLiteral("Choose Whisper's precision on the remote computer. "
                                         "Auto chooses a supported precision for its hardware.");
// OpenWhisper adds "Moonshine supports English only": OpenWhisper_CPP's hosts serve Parakeet alone.
const QString kLanguageTip =
    QStringLiteral("Choose the host engine's language for remote transcription. "
                   "Auto detects a supported language.");
const QString kAppliesToHost = QStringLiteral(" Changes apply to the host and connected clients.");
const FieldHelpLink kRemoteEngineLink{QStringLiteral(u"Open Settings → Remote engine"),
                                      QStringLiteral("remote_engine")};
const QString kSetupPrefix = QStringLiteral("setup:");

// _LABELS: the language and device labels the fields show; anything else shows as it is.
QString optionLabel(const QString& value) {
    const std::string key = value.toStdString();
    for (const auto& [code, label] : speech::language_labels()) {
        if (code == key) {
            return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
        }
    }
    return deviceLabel(value);
}

const Json& member(const Json& object, const char* key) {
    static const Json kNull;
    if (!object.is_object()) {
        return kNull;
    }
    const auto found = object.find(key);
    return found == object.end() ? kNull : *found;
}

QString text(const Json& value) {
    return value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString();
}

QStringList strings(const Json& value) {
    QStringList out;
    if (value.is_array()) {
        for (const Json& item : value) {
            if (item.is_string()) {
                out << QString::fromStdString(item.get<std::string>());
            }
        }
    }
    return out;
}

// _runtime_tip.
QString runtimeTip(const QString& host, const Json& runtime, const Json& engine) {
    if (member(runtime, "can_configure") == Json(true)) {
        return QStringLiteral("Runtime on %1. Changes apply to the host and connected clients.")
            .arg(host);
    }
    if (engine.is_object() && !engine.empty()) {
        return QStringLiteral("Update %1 on %2 to change its runtime from here.")
            .arg(QString::fromUtf8(identity().product), host);
    }
    return QStringLiteral("Connect to the host to see its runtime choices.");
}

} // namespace

RemoteEngineControls::RemoteEngineControls(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    deviceCombo_ = engineCombo({});
    computeCombo_ = engineCombo({});
    languageCombo_ = engineCombo({});
    const struct {
        QString key;
        QString caption;
        QComboBox* combo;
        QString tip;
    } fields[] = {
        {QStringLiteral("device"), QStringLiteral("Device"), deviceCombo_, kDeviceTip},
        {QStringLiteral("compute_type"), QStringLiteral("Quant"), computeCombo_, kQuantTip},
        {QStringLiteral("language"), QStringLiteral("Language"), languageCombo_, kLanguageTip},
    };
    for (const auto& item : fields) {
        QWidget* column =
            engineField(item.caption, item.combo, item.tip + kAppliesToHost, {kRemoteEngineLink},
                        [this](const QString& destination) { Q_EMIT helpRequested(destination); });
        fields_[item.key] = column;
        layout->addWidget(column, 1);
        connect(
            item.combo, &QComboBox::activated, this,
            [this, key = item.key, combo = item.combo](int index) { activate(key, combo, index); });
    }
    setState(std::nullopt);
}

QWidget* RemoteEngineControls::field(const QString& key) const {
    const auto found = fields_.find(key);
    return found == fields_.end() ? nullptr : found->second;
}

void RemoteEngineControls::setState(const std::optional<remote::RemoteModels>& state) {
    state_ = state;
    pending_ = false;
    const Json runtime = state && state->runtime ? *state->runtime : Json::object();
    const Json engine = state && state->engine ? *state->engine : Json::object();
    const Json& selected = member(runtime, "selected");
    QString family = text(member(engine, "family"));
    if (family.isEmpty()) {
        family = text(member(runtime, "family"));
    }
    // Whisper's precision means nothing to other families; their language does.
    const bool whisper = family.isEmpty() || family == QLatin1String("local_whisper");
    fields_[QStringLiteral("compute_type")]->setVisible(whisper);
    fields_[QStringLiteral("language")]->setVisible(!whisper);

    QString device = text(member(selected, "device"));
    if (device.isEmpty()) {
        device = text(member(engine, "device"));
    }
    const bool canConfigure = member(runtime, "can_configure") == Json(true);
    const bool reported = engine.is_object() && !engine.empty();
    QString host = state ? QString::fromStdString(state->host) : QString();
    if (host.isEmpty()) {
        host = QStringLiteral("the host");
    }
    const QString tipBase = runtimeTip(host, runtime, engine);

    const struct {
        QString key;
        QComboBox* combo;
        QStringList options;
    } rows[] = {
        {QStringLiteral("device"), deviceCombo_, strings(member(runtime, "devices"))},
        {QStringLiteral("compute_type"), computeCombo_,
         strings(member(member(runtime, "compute_types"), device.toUtf8().constData()))},
        {QStringLiteral("language"), languageCombo_, strings(member(runtime, "languages"))},
    };
    for (const auto& row : rows) {
        const std::string key = row.key.toStdString();
        QString value = text(member(selected, key.c_str()));
        if (value.isEmpty()) {
            value = text(member(engine, key.c_str()));
        }
        QComboBox* combo = row.combo;
        const bool blocked = combo->blockSignals(true);
        combo->clear();
        QStringList values = row.options;
        if (!value.isEmpty() && !values.contains(value)) {
            values << value;
        }
        for (const QString& item : values) {
            combo->addItem(optionLabel(item), item);
        }
        if (row.key == QLatin1String("device")) {
            const Json& dependencies = member(runtime, "dependencies");
            if (dependencies.is_array()) {
                for (const Json& dependency : dependencies) {
                    const QString needed = text(member(dependency, "device"));
                    if (member(dependency, "installable") == Json(true) && !needed.isEmpty() &&
                        !values.contains(needed)) {
                        combo->addItem(
                            QStringLiteral(u"%1 · Set up on host").arg(optionLabel(needed)),
                            kSetupPrefix + needed);
                    }
                }
            }
        }
        if (values.isEmpty()) {
            combo->addItem(reported ? QStringLiteral("Not reported")
                                    : QStringLiteral("Not connected"),
                           QVariant());
        } else if (!value.isEmpty()) {
            combo->setCurrentIndex(combo->findData(value));
        }
        combo->blockSignals(blocked);
        QString tip = tipBase;
        if (!value.isEmpty() && !row.options.contains(value) && canConfigure) {
            tip += QStringLiteral(" The saved choice is currently unavailable on the host.");
            if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
                if (QStandardItem* item = model->item(combo->findData(value))) {
                    item->setEnabled(false);
                }
            }
        }
        combo->setToolTip(tip);
        const bool editable =
            combo->count() > 1 || (!row.options.isEmpty() && !row.options.contains(value));
        editable_[combo] = canConfigure && editable;
    }
    syncEnabled();
}

void RemoteEngineControls::setLocked(bool locked) {
    locked_ = locked;
    syncEnabled();
}

void RemoteEngineControls::syncEnabled() {
    for (QComboBox* combo : {deviceCombo_, computeCombo_, languageCombo_}) {
        const bool editable = editable_[combo];
        combo->setEnabled(editable && !locked_ && !pending_);
    }
}

void RemoteEngineControls::activate(const QString& key, QComboBox* combo, int index) {
    const QVariant item = combo->itemData(index);
    const Json runtime = state_ && state_->runtime ? *state_->runtime : Json::object();
    if (!combo->isEnabled() || !item.isValid() || item.isNull()) {
        return;
    }
    const QString value = item.toString();
    if (value == text(member(member(runtime, "selected"), key.toStdString().c_str()))) {
        return;
    }
    if (value.startsWith(kSetupPrefix)) {
        setState(state_);
        Q_EMIT helpRequested(QStringLiteral("remote_models"));
        return;
    }
    pending_ = true;
    syncEnabled();
    Q_EMIT runtimeSelected(text(member(runtime, "family")), text(member(runtime, "model")),
                           QVariantMap{{key, value}});
}

} // namespace fv
