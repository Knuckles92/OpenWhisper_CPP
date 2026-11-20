#include "ui_downloads/hf_policy_field.h"

#include <exception>
#include <string_view>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QSignalBlocker>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "models/hf_access_policy.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_widgets/no_wheel.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

} // namespace

HfPolicyField::HfPolicyField(Persist persist, QWidget* parent)
    : QObject(), persist_(std::move(persist)), combo_(new ElidingComboBox(parent)) {
    setParent(combo_);
    combo_->setObjectName(QStringLiteral("hfPolicyCombo"));
    combo_->addItem(QStringLiteral("Ask before downloading"),
                    qt::toQString(models::hf_access_policy::kAsk));
    combo_->addItem(QStringLiteral("Always allow downloads"),
                    qt::toQString(models::hf_access_policy::kAlways));
    combo_->addItem(QStringLiteral("Never connect (fully offline)"),
                    qt::toQString(models::hf_access_policy::kNever));
    combo_->setMinimumHeight(34);
    combo_->setMaximumWidth(300);
    combo_->setToolTip(QStringLiteral(
        "Models already on this computer always load locally without any network checks. "
        "Hugging Face is only contacted to download a missing model, and only when this policy "
        "or a one-time approval allows it. An external HF_HUB_OFFLINE=1 environment variable "
        "disables downloads entirely."));
    connect(combo_, &QComboBox::currentIndexChanged, this, &HfPolicyField::onChanged);
}

void HfPolicyField::load(SettingsStore& settings) {
    const QString policy = qt::toQString(models::load_hf_access_policy(settings));
    const int index = combo_->findData(policy);
    if (index >= 0) {
        // The window's loading flag would refuse the save; the field's own flag keeps a page
        // built without a window from saving what it just read.
        loading_ = true;
        combo_->setCurrentIndex(index);
        loading_ = false;
    }
}

QString HfPolicyField::policy() const {
    return combo_->currentData().toString();
}

void HfPolicyField::onChanged() {
    if (loading_) {
        return;
    }
    const QString value = policy();
    Json updates = Json::object();
    updates[std::string(keys::hf_access_policy.key())] = qt::toUtf8(value);
    if (!persist_ || !persist_(updates, {std::string(keys::hf_hub_offline.key())})) {
        return;
    }
    Q_EMIT policyChanged(value);
}

HfPolicyField::Persist HfPolicyField::storeWriter(SettingsStore& settings) {
    return [&settings](const Json& updates, std::vector<std::string> drops) {
        try {
            const std::vector<std::string_view> remove(drops.begin(), drops.end());
            settings.update(updates, remove);
            return true;
        } catch (const std::exception& error) {
            kLog.error_with(error, "Couldn't save the download policy");
            return false;
        }
    };
}

} // namespace fv
