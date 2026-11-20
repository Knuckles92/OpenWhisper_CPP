#include "models/coordinator.h"

#include <format>

#include "base/identity.h"
#include "base/log.h"
#include "models/detail/files.h"
#include "pyport/json.h"

namespace fv::models {

namespace {

constexpr log::Logger kLog("services.components");

// services.format_utils.format_size_bytes: decimal units, GB to two places, MB and KB to none.
std::string format_size_bytes(std::int64_t size_bytes) {
    if (size_bytes >= 1'000'000'000) {
        return std::format("{:.2f} GB", static_cast<double>(size_bytes) / 1'000'000'000);
    }
    if (size_bytes >= 1'000'000) {
        return std::format("{:.0f} MB", static_cast<double>(size_bytes) / 1'000'000);
    }
    if (size_bytes >= 1'000) {
        return std::format("{:.0f} KB", static_cast<double>(size_bytes) / 1'000);
    }
    return std::format("{} B", size_bytes);
}

std::int64_t entry_int(const std::optional<Json>& entry, std::string_view key) {
    if (!entry) {
        return 0;
    }
    const Json* value = detail::member(*entry, key);
    return value == nullptr ? 0 : detail::to_int(*value);
}

} // namespace

ComponentCoordinator::ComponentCoordinator(ComponentStore& store)
    : ComponentCoordinator(store, Options{}) {}

ComponentCoordinator::ComponentCoordinator(ComponentStore& store, Options options)
    : store_(store), options_(std::move(options)) {}

std::optional<CancelSource>
ComponentCoordinator::begin_install(std::string_view component_id,
                                    std::optional<CancelSource> cancel) {
    std::lock_guard lock(mutex_);
    if (active_.contains(component_id)) {
        kLog.debug("Install for '{}' already in flight", component_id);
        return std::nullopt;
    }
    active_.emplace(component_id);
    CancelSource source = cancel ? *cancel : CancelSource();
    cancel_flags_.insert_or_assign(std::string(component_id), source);
    return source;
}

void ComponentCoordinator::end_install(std::string_view component_id) {
    std::lock_guard lock(mutex_);
    if (const auto it = active_.find(component_id); it != active_.end()) {
        active_.erase(it);
    }
    if (const auto it = cancel_flags_.find(component_id); it != cancel_flags_.end()) {
        cancel_flags_.erase(it);
    }
}

bool ComponentCoordinator::is_installing(std::string_view component_id) const {
    std::lock_guard lock(mutex_);
    return active_.contains(component_id);
}

bool ComponentCoordinator::is_any_installing() const {
    std::lock_guard lock(mutex_);
    return !active_.empty();
}

void ComponentCoordinator::cancel_install(std::string_view component_id) {
    std::optional<CancelSource> source;
    {
        std::lock_guard lock(mutex_);
        if (const auto it = cancel_flags_.find(component_id); it != cancel_flags_.end()) {
            source = it->second;
        }
    }
    if (source) {
        kLog.info("Cancelling install of '{}'", component_id);
        source->cancel();
    }
}

void ComponentCoordinator::cancel_all() {
    std::vector<CancelSource> sources;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, source] : cancel_flags_) {
            sources.push_back(source);
        }
    }
    for (auto& source : sources) {
        source.cancel();
    }
}

const Json& ComponentCoordinator::fetch_catalog() const {
    return options_.catalog != nullptr ? *options_.catalog : builtin_catalog();
}

std::optional<Json> ComponentCoordinator::catalog_entry(std::string_view component_id) const {
    const Json& catalog = fetch_catalog();
    if (!detail::truthy(catalog)) {
        return std::nullopt;
    }
    return models::catalog_entry(component_id, store_.host(), catalog);
}

ComponentInfo ComponentCoordinator::describe(std::string_view component_id) const {
    ComponentInfo info;
    info.component_id = std::string(component_id);
    if (options_.copy_for) {
        std::tie(info.display_name, info.summary) = options_.copy_for(component_id);
    } else {
        info.display_name = std::string(component_id);
    }
    const auto entry = catalog_entry(component_id);
    if (entry && !entry->empty()) {
        if (const Json* version = detail::member(*entry, "version")) {
            info.available_version.emplace(*version);
        }
        if (const Json* archives = detail::member(*entry, "archives");
            archives != nullptr && archives->is_array()) {
            for (const Json& archive : *archives) {
                const Json* size = detail::member(archive, "size_bytes");
                info.download_bytes += size == nullptr ? 0 : detail::to_int(*size);
            }
        }
    }
    if (info.available_version && info.available_version->is_null()) {
        info.available_version.reset();
    }

    const auto manifest = store_.read_manifest(component_id);
    const bool present = store_.is_installed(component_id);

    if (present && !manifest) {
        info.state = ComponentState::Broken;
        info.install_bytes = entry_int(entry, "install_bytes");
        info.reason = "The component manifest is missing or invalid.";
        return info;
    }
    if (!present) {
        // A directory without the sentinel is a partial install.
        std::error_code error;
        const bool partial =
            std::filesystem::is_directory(store_.component_dir(component_id), error);
        info.state = partial ? ComponentState::Broken : ComponentState::NotInstalled;
        info.install_bytes = entry_int(entry, "install_bytes");
        info.reason = partial ? "The previous installation did not finish." : "";
        return info;
    }

    if (const Json* version = detail::member(*manifest, "version");
        version != nullptr && !version->is_null()) {
        info.installed_version.emplace(*version);
    }
    const auto incompatible = check_compatibility(*manifest, store_.host());
    // The catalog is a release pin, so any version difference means the shipped payload changed.
    const bool outdated = info.available_version && detail::truthy(*info.available_version) &&
                          info.installed_version && detail::truthy(*info.installed_version) &&
                          !py::py_equal(*info.available_version, *info.installed_version);
    if (incompatible) {
        info.state = ComponentState::Incompatible;
        info.reason = *incompatible;
    } else if (outdated) {
        // Say what the user gets, since an update is their choice.
        const std::int64_t reclaimed =
            store_.installed_size_bytes(component_id) - entry_int(entry, "install_bytes");
        info.state = ComponentState::UpdateAvailable;
        info.reason = reclaimed > 0 ? std::format("Update frees {} of disk space.",
                                                  format_size_bytes(reclaimed))
                                    : "";
    } else {
        info.state = ComponentState::Installed;
    }
    info.install_bytes = store_.installed_size_bytes(component_id);
    // Only a loaded (so installed) component has a change staged.
    info.staged = store_.staged_change(component_id);
    if (info.staged == StagedChange::Removal) {
        info.reason =
            std::format("Restart {} to finish removing the component.", identity().product);
    } else if (info.staged == StagedChange::Install) {
        info.reason =
            std::format("Restart {} to finish installing the component.", identity().product);
    }
    return info;
}

std::vector<ComponentInfo> ComponentCoordinator::list_components() const {
    std::vector<ComponentInfo> rows;
    for (const auto& id : available_component_ids(store_.host())) {
        rows.push_back(describe(id));
    }
    return rows;
}

ChangeOutcome ComponentCoordinator::install(std::string_view component_id,
                                            const InstallProgress& progress,
                                            const CancelToken& cancel) {
    const auto entry = catalog_entry(component_id);
    if (!entry) {
        throw ComponentError(
            "Could not reach the download server. Check your internet connection and try again.");
    }
    return store_.install(component_id, *entry, progress, cancel);
}

ChangeOutcome ComponentCoordinator::uninstall(std::string_view component_id) {
    return store_.uninstall(component_id);
}

void ComponentCoordinator::prune_orphans() {
    store_.prune_orphans();
}

} // namespace fv::models
