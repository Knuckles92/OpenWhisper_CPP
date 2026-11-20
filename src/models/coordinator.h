#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "models/catalog.h"
#include "models/component_store.h"

// Owns the catalog and serializes installs (services/components.py ComponentCoordinator):
// installs run on a worker while dialogs run on the GUI thread. A second request for a component
// already installing is refused rather than starting a duplicate download.
namespace fv::models {

// Affinity: thread-safe; install() and uninstall() block (never on the GUI thread).
class ComponentCoordinator {
public:
    // A component's display name and row summary; the id and "" when it has no copy.
    using CopyLookup = std::function<std::pair<std::string, std::string>(std::string_view id)>;

    struct Options {
        // The catalog describe() and install() read. nullptr: builtin_catalog(). Tests point it
        // at their own document, or at a null Json for "no catalog".
        const Json* catalog = nullptr;
        // Component copy (the Downloads page's component_details, M2); empty: id and "".
        CopyLookup copy_for;
    };

    explicit ComponentCoordinator(ComponentStore& store);
    ComponentCoordinator(ComponentStore& store, Options options);

    // Claims an install and returns its cancel source, or nullopt while one is in flight.
    // `cancel` joins an existing source (so a caller's own cancel reaches it).
    std::optional<CancelSource> begin_install(std::string_view component_id,
                                              std::optional<CancelSource> cancel = std::nullopt);
    void end_install(std::string_view component_id);
    bool is_installing(std::string_view component_id) const;
    bool is_any_installing() const;
    void cancel_install(std::string_view component_id);
    // Cancels every install in flight; called before the executors shut down, since a running
    // multi-gigabyte download would otherwise hold shutdown open for minutes.
    void cancel_all();

    // The shared read-only catalog (no copy).
    const Json& fetch_catalog() const;
    // A copy of the entry for this host's platform, or nullopt.
    std::optional<Json> catalog_entry(std::string_view component_id) const;

    // A component's state for the UI. What is on disk decides whether it is installed; the
    // catalog only supplies the version and sizes to compare with, so a component without a
    // catalog entry still reads as installed. Reasons: "The component manifest is missing or
    // invalid.", "The previous installation did not finish.", the compatibility text, and
    // "Update frees {size} of disk space." (only when the update reclaims space).
    ComponentInfo describe(std::string_view component_id) const;

    // describe() for every component installable here; empty where none is, so the UI can hide
    // the section.
    std::vector<ComponentInfo> list_components() const;

    // Installs the catalog entry of `component_id` for this platform (ComponentStore::install).
    // Without an entry it throws ComponentError "Could not reach the download server. Check your
    // internet connection and try again." (services/runtime/components.py's text, kept from
    // when the catalog was fetched).
    ChangeOutcome install(std::string_view component_id, const InstallProgress& progress,
                          const CancelToken& cancel);
    ChangeOutcome uninstall(std::string_view component_id);
    void prune_orphans();

    ComponentStore& store() const { return store_; }

private:
    ComponentStore& store_;
    Options options_;
    mutable std::mutex mutex_;
    std::set<std::string, std::less<>> active_;
    std::map<std::string, CancelSource, std::less<>> cancel_flags_;
};

} // namespace fv::models
