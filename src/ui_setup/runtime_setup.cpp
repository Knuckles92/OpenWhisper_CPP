#include "ui_setup/runtime_setup.h"

#include <utility>

#include <QFileDialog>
#include <QString>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/json.h"
#include "base/utf8.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_setup/runtime_setup_dialog.h"

namespace fv {
namespace {

std::optional<std::filesystem::path> fromPicker(const QString& chosen) {
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    return base_utf8::path_from_utf8(qt::toUtf8(chosen));
}

} // namespace

RuntimeSetup::RuntimeSetup(AppContext& context) : context_(&context) {}

RuntimeSetup::RuntimeSetup(RuntimeSetupPorts ports) : ports_(std::move(ports)) {}

bool RuntimeSetup::ensureEngine(QWidget* parent) {
    if (!ports_) {
        ports_ = portsFor(*context_);
    }
    // Each dialog gets its own copy: work it leaves running on io keeps that copy alive.
    auto ports = std::make_shared<RuntimeSetupPorts>(*ports_);
    if (runtimeLoaded_) {
        ports->runtime_loaded = runtimeLoaded_;
    }
    RuntimeSetupDialog dialog(std::move(ports), parent);
    dialog.exec();
    return dialog.usable();
}

void RuntimeSetup::setRuntimeLoaded(std::function<bool()> loaded) {
    runtimeLoaded_ = std::move(loaded);
}

RuntimeSetupPorts RuntimeSetup::portsFor(AppContext& context) {
    // The context outlives every dialog (it is built in Bootstrap before any window).
    const Paths* paths = &context.paths();
    SettingsStore* settings = &context.settings();

    RuntimeSetupPorts ports;
    ports.io = &context.io();
    ports.discover = [paths] {
        return models::discover_openwhisper(*paths);
    };
    ports.adopt_component = [paths](const std::string& id, const CancelToken& cancel) {
        models::adopt_component(*paths, id, cancel);
    };
    ports.adopt_model = [paths](const std::string& key, const CancelToken& cancel) {
        models::adopt_model(*paths, key, cancel);
    };
    ports.import_model_file = [paths](const std::filesystem::path& file,
                                      const CancelToken& cancel) {
        return models::import_model_file(*paths, file, cancel);
    };
    ports.resolve = [paths, settings] {
        return models::resolve_parakeet(*settings->snapshot(), *paths);
    };
    ports.save_runtime_dir = [settings](const std::filesystem::path& folder) {
        settings->save_setting(keys::fv_runtime_dir.key(), Json(base_utf8::path_to_utf8(folder)));
    };
    ports.pick_runtime_folder = [](QWidget* parent) {
        return fromPicker(QFileDialog::getExistingDirectory(
            parent, QStringLiteral("Choose a NeMo-Speech.cpp folder")));
    };
    ports.pick_model_file = [](QWidget* parent) {
        return fromPicker(
            QFileDialog::getOpenFileName(parent, QStringLiteral("Choose a model file"), QString(),
                                         QStringLiteral("Model files (*.gguf)")));
    };
    return ports;
}

} // namespace fv
