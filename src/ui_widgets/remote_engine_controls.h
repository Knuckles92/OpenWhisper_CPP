#pragma once

#include <map>
#include <optional>

#include <QVariantMap>
#include <QWidget>

#include "base/json.h"
#include "remote/link.h"

class QComboBox;

namespace fv {

// The Remote backend's Device, Quant and Language fields, filled only from what the paired host
// reports (ui_qt/widgets/remote_engine_controls.py RemoteEngineControls). A choice is sent to the
// host as runtime_selected(family, model, {key: value}) and the fields lock until the next state
// arrives. A device the host could set up shows as "<device> · Set up on host" and opens the
// host's model management instead. Affinity: GUI thread.
class RemoteEngineControls : public QWidget {
    Q_OBJECT
public:
    explicit RemoteEngineControls(QWidget* parent = nullptr);

    // What the host reports (its runtime and engine); nullopt or no runtime before it connects.
    void setState(const std::optional<remote::RemoteModels>& state);
    // Locked while the engine reloads, a recording runs or the host is not connected.
    void setLocked(bool locked);

    QComboBox* deviceCombo() const { return deviceCombo_; }
    QComboBox* computeCombo() const { return computeCombo_; }
    QComboBox* languageCombo() const { return languageCombo_; }
    // The captioned column of one field: "device", "compute_type" or "language".
    QWidget* field(const QString& key) const;

Q_SIGNALS:
    // family, model and the one setting that changed ({key: value}).
    void runtimeSelected(const QString& family, const QString& model, const QVariantMap& changes);
    void helpRequested(const QString& destination);

private:
    void activate(const QString& key, QComboBox* combo, int index);
    void syncEnabled();

    std::optional<remote::RemoteModels> state_;
    bool locked_ = false;
    bool pending_ = false;
    QComboBox* deviceCombo_ = nullptr;
    QComboBox* computeCombo_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    std::map<QString, QWidget*> fields_;
    // Whether the host lets this computer change each field now.
    std::map<QComboBox*, bool> editable_;
};

} // namespace fv
