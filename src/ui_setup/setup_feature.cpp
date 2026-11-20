#include "ui_setup/setup_feature.h"

#include <memory>
#include <utility>

#include "appcore/guided_setup_state.h"
#include "appkit/app_context.h"
#include "ui_settings/settings_backup.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_setup/openwhisper_import.h"
#include "ui_setup/runtime_setup.h"

namespace fv {

namespace {

// The Backup page's "Import from OpenWhisper…" over this feature's import: ui_settings, which
// this module depends on, cannot name OpenWhisperImport, so it asks for BackupImportOffer.
// Holds the import, so the page's offer never outlives it. Affinity: GUI thread.
class BackupImportAdapter final : public BackupImportOffer {
public:
    explicit BackupImportAdapter(std::shared_ptr<OpenWhisperImport> import)
        : import_(std::move(import)) {
        QObject::connect(import_.get(), &OpenWhisperImport::availabilityChanged, this,
                         &BackupImportOffer::availabilityChanged);
    }

    bool available() const override { return import_->available(); }
    void refresh() override { import_->refresh(); }
    void open(QWidget* parent) override { import_->open(parent); }

private:
    std::shared_ptr<OpenWhisperImport> import_;
};

SettingsStore* findSettings(AppContext& context) {
    try {
        return &context.settings();
    } catch (const ServiceUnavailable&) {
        return nullptr;
    }
}

} // namespace

void SetupFeature::provide(AppContext& context) {
    context.provide(std::make_shared<RuntimeSetup>(context));
    // A context without settings (a test of another service) has no setup to offer.
    if (findSettings(context) == nullptr) {
        return;
    }
    appcore::guided_setup_state::initialize(context.settings());
    // Before GuidedSetup, whose Welcome page asks it whether to offer the import. It looks for
    // OpenWhisper's data only when a window asks it to. A context without the executors (a test
    // of another service) has no import.
    try {
        auto import = std::make_shared<OpenWhisperImport>(context);
        context.provide(import);
        context.provide<BackupImportOffer>(std::make_shared<BackupImportAdapter>(import));
    } catch (const ServiceUnavailable&) {
    }
    context.provide(std::make_shared<GuidedSetup>(context));
}

void SetupFeature::install(AppContext& context) {
    if (auto* setup = context.find<GuidedSetup>()) {
        setup->install(context);
    }
}

} // namespace fv
