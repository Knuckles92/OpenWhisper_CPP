#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "base/json.h"

namespace fv {

class ElidingComboBox;
class SettingsStore;

// "When a model is missing from this computer" (settings_dialog.py _build_downloads_page's
// hfPolicyCombo and _on_hf_policy_changed): Ask before downloading, Always allow downloads or
// Never connect, with the tooltip that says cached models never touch the network. A choice is
// saved together with the removal of the legacy hf_hub_offline key, then reported through
// policyChanged. The field belongs to its combo and goes with it.
// Affinity: GUI thread.
class HfPolicyField : public QObject {
    Q_OBJECT
public:
    // Saves {"hf_access_policy": policy} without `drops` (the Settings window's persistMany);
    // false when nothing was saved (the window is loading, or the save failed and was shown).
    using Persist = std::function<bool(const Json& updates, std::vector<std::string> drops)>;

    explicit HfPolicyField(Persist persist, QWidget* parent = nullptr);

    ElidingComboBox* combo() const { return combo_; }

    // Selects the saved policy (load_hf_access_policy, which migrates a legacy value), without
    // saving it again.
    void load(SettingsStore& settings);
    // The policy shown ("ask", "always" or "never").
    QString policy() const;

    // A Persist that saves straight to the store, for a page with no Settings window around it.
    static Persist storeWriter(SettingsStore& settings);

Q_SIGNALS:
    // A new policy was saved (Settings' on_hf_policy_changed).
    void policyChanged(const QString& policy);

private:
    void onChanged();

    Persist persist_;
    ElidingComboBox* combo_;
    bool loading_ = false;
};

} // namespace fv
