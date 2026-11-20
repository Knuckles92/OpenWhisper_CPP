#include "models/downloads_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "history/format_utils.h"
#include "models/model_details.h"
#include "models/speech_catalog.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::models {

namespace {

std::string noun_for(std::int64_t count) {
    return count == 1 ? "model" : "models";
}

// round(x * 100) as Python rounds: half to even.
long long percent_of(double fraction) {
    return static_cast<long long>(py::round_half_even(fraction * 100));
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            joined += separator;
        }
        joined += parts[i];
    }
    return joined;
}

} // namespace

std::optional<CachedModelInfo> current_cache_info(const std::optional<CachedModelInfo>& info,
                                                  std::string_view revision) {
    if (!info || revision.empty() ||
        std::ranges::find(info->revisions, revision) != info->revisions.end()) {
        return info;
    }
    return std::nullopt;
}

ModelRowFacts model_row_facts(std::string_view model_name,
                              const std::optional<CachedModelInfo>& info) {
    ModelRowFacts facts;
    facts.model_name = std::string(model_name);
    const ModelDetails* details = find_model_details(model_name);
    // resolve_model_repo passes an unknown name through unchanged.
    facts.repo_id = details != nullptr ? details->repository_id : std::string(model_name);
    facts.backend = std::string(backend_of(model_name));
    facts.is_cached = info.has_value();
    if (info) {
        facts.sort_size_bytes = static_cast<double>(info->size_on_disk);
    } else if (const auto size_mb = model_download_size_mb(model_name)) {
        facts.sort_size_bytes = static_cast<double>(*size_mb * 1'000'000);
    } else {
        facts.sort_size_bytes = std::numeric_limits<double>::infinity();
    }
    return facts;
}

std::vector<std::string> model_row_names() {
    std::vector<std::string> names;
    for (const SpeechModel& model : models()) {
        names.emplace_back(model.key);
    }
    return names;
}

std::vector<std::string> backend_filter_values() {
    std::vector<std::pair<std::string, std::string>> labelled;
    for (const SpeechBackend& backend : backends()) {
        labelled.emplace_back(py::casefold(backend.label), std::string(backend.key));
    }
    std::ranges::stable_sort(labelled, {}, &std::pair<std::string, std::string>::first);
    std::vector<std::string> values{"all"};
    for (auto& [label, key] : labelled) {
        values.push_back(std::move(key));
    }
    return values;
}

int backend_filter_index(std::string_view backend) {
    const std::vector<std::string> values = backend_filter_values();
    const auto found = std::ranges::find(values, backend);
    return found == values.end() ? -1 : static_cast<int>(found - values.begin());
}

bool SortKey::operator<(const SortKey& other) const {
    if (group != other.group) {
        return group < other.group;
    }
    if (not_cached != other.not_cached) {
        return !not_cached;
    }
    if (size != other.size) {
        return size < other.size;
    }
    return name < other.name;
}

SortKey sort_key(std::string_view mode, const ModelRowFacts& row) {
    SortKey key;
    key.name = py::casefold(row.model_name);
    if (mode == sort_mode::kDownloaded) {
        key.not_cached = !row.is_cached;
        return key;
    }
    if (mode == sort_mode::kSize) {
        key.size = row.sort_size_bytes;
        return key;
    }
    if (mode == sort_mode::kName) {
        return key;
    }
    key.not_cached = !row.is_cached;
    key.size = row.sort_size_bytes;
    if (mode == sort_mode::kBackend) {
        // Matches the alphabetical filter combo, recommended order inside.
        key.group = backend_filter_index(row.backend);
    }
    return key;
}

std::vector<ModelRowFacts> sorted_rows(std::vector<ModelRowFacts> rows, std::string_view mode) {
    std::ranges::stable_sort(rows, [mode](const ModelRowFacts& a, const ModelRowFacts& b) {
        return sort_key(mode, a) < sort_key(mode, b);
    });
    return rows;
}

bool matches_filter(std::string_view needle, const ModelRowFacts& row, const FuzzyMatch& fuzzy) {
    return fuzzy(needle, row.model_name) || fuzzy(needle, row.repo_id);
}

bool row_visible(const ModelRowFacts& row, std::string_view search_text, std::string_view backend,
                 std::string_view status, const FuzzyMatch& fuzzy) {
    const std::string needle = py::lower(py::strip(search_text));
    bool visible = needle.empty() || matches_filter(needle, row, fuzzy);
    if (backend != "all") {
        visible = visible && row.backend == backend;
    }
    if (status == "downloaded") {
        visible = visible && row.is_cached;
    } else if (status == "not_downloaded") {
        visible = visible && !row.is_cached;
    }
    return visible;
}

SelectionBar selection_summary(const SelectionBarInput& input) {
    const auto count = static_cast<std::int64_t>(input.selected_sizes.size());
    std::int64_t total_bytes = 0;
    bool all_known = true;
    for (const double size : input.selected_sizes) {
        if (std::isfinite(size)) {
            total_bytes += static_cast<std::int64_t>(size);
        } else {
            all_known = false;
        }
    }
    const std::string estimate =
        all_known ? "~" + format_size_bytes(total_bytes) + " to download" : "download size unknown";
    const bool any_selectable = input.any_missing && !input.busy && !input.downloads_blocked;

    SelectionBar bar;
    // Nothing ticked: say what the bar is for, or the disabled button reads as broken.
    if (count > 0) {
        bar.summary = std::to_string(count) + " selected · " + estimate;
    } else if (any_selectable) {
        bar.summary = "Tick models to download several at once";
    }
    bar.download_text = count > 0
                            ? "Download " + std::to_string(count) + " " + noun_for(count) + "…"
                            : "Download selected";
    const bool selectable = count > 0 && !input.busy && !input.downloads_blocked;
    bar.download_enabled = selectable;
    if (!selectable) {
        bar.download_tooltip = input.downloads_blocked ? "Downloads are disabled by HF_HUB_OFFLINE"
                               : input.busy            ? "Wait for the current download to finish"
                                                       : "Tick the models to download first";
    }
    bar.clear_enabled = count > 0 && !input.busy;
    bar.select_all_enabled = any_selectable;
    bar.download_all_enabled = any_selectable;
    bar.download_all_visible = input.any_missing;
    return bar;
}

void lock_while_checking(SelectionBar& bar) {
    bar.select_all_enabled = false;
    bar.download_all_enabled = false;
    bar.download_enabled = false;
}

std::string usage_text(std::string_view model_name, std::string_view dictation_model,
                       std::string_view meeting_model) {
    std::vector<std::string> uses;
    if (model_name == dictation_model) {
        uses.emplace_back("Dictation");
    }
    if (model_name == meeting_model) {
        uses.emplace_back("Meetings");
    }
    return join(uses, " · ");
}

ModelsInUse models_in_use(const Json& settings, std::string meeting_model,
                          std::optional<std::string> loaded_model) {
    std::string active = "auto";
    const std::string selected_backend = keys::selected_model.read(settings);
    if (backend(selected_backend) != nullptr) {
        active = selected_model(settings, selected_backend);
    }
    ModelsInUse in_use;
    in_use.dictation =
        active == "auto" && loaded_model && !loaded_model->empty() ? *loaded_model : active;
    in_use.meeting = std::move(meeting_model);
    in_use.loaded = std::move(loaded_model);
    return in_use;
}

std::string model_display_name(std::string_view model_name) {
    if (const SpeechModel* found = model(model_name)) {
        return std::string(found->label);
    }
    return std::string(model_name);
}

std::string DownloadQueue::set_downloading(std::string_view model_name) {
    downloading_ = std::string(model_name);
    fraction_.reset();
    if (!queue_.empty()) {
        const int position = done_ + 1;
        const int total = done_ + static_cast<int>(queue_.size());
        return "Downloading model " + std::to_string(position) + " of " + std::to_string(total) +
               ": " + std::string(model_name) + "…";
    }
    return "Downloading \"" + std::string(model_name) + "\"…";
}

DownloadQueue::Progress DownloadQueue::set_download_progress(std::string_view model_name,
                                                             std::int64_t done,
                                                             std::int64_t total) {
    Progress progress;
    if (total <= 0) {
        return progress;
    }
    progress.message = "Downloading \"" + std::string(model_name) + "\"… " +
                       format_size_bytes(done) + " of " + format_size_bytes(total);
    const std::optional<double> previous = fraction_;
    fraction_ =
        std::min(1.0, std::max(0.0, static_cast<double>(done) / static_cast<double>(total)));
    progress.rail_changed = !previous || percent_of(*previous) != percent_of(*fraction_);
    return progress;
}

std::optional<std::string> DownloadQueue::finish_download(std::string_view model_name,
                                                          bool success) {
    if (downloading_ == model_name) {
        downloading_.reset();
        fraction_.reset();
    }
    if (const auto queued = std::ranges::find(queue_, model_name); queued != queue_.end()) {
        queue_.erase(queued);
        ++done_;
        if (!success) {
            ++failed_;
        }
        if (queue_.empty()) {
            // finish_batch reports the closing summary for the queue.
            return std::nullopt;
        }
        return std::string();
    }
    if (success) {
        return std::string();
    }
    return "Download of \"" + std::string(model_name) + "\" failed";
}

std::string DownloadQueue::begin_batch(std::span<const std::string> model_names,
                                       const std::function<bool(std::string_view)>& is_row) {
    queue_.clear();
    for (const std::string& name : model_names) {
        if (!is_row || is_row(name)) {
            queue_.push_back(name);
        }
    }
    done_ = 0;
    failed_ = 0;
    return "Downloading " + std::to_string(queue_.size()) + " models…";
}

std::string DownloadQueue::finish_batch(int completed, int planned) {
    const int failed = failed_;
    queue_.clear();
    failed_ = 0;
    if (completed < planned && failed == 0) {
        return "Stopped — downloaded " + std::to_string(completed) + " of " +
               std::to_string(planned) + " models";
    }
    if (failed != 0 || completed < planned) {
        return "Downloaded " + std::to_string(completed) + " of " + std::to_string(planned) +
               " models — " + std::to_string(failed) + " failed";
    }
    return "Downloaded " + std::to_string(planned) + " " + noun_for(planned);
}

bool DownloadQueue::is_queued(std::string_view model_name) const {
    return std::ranges::find(queue_, model_name) != queue_.end() && downloading_ != model_name;
}

std::string delete_result_message(std::string_view model_name, bool success,
                                  std::string_view error) {
    if (success) {
        return "Deleted \"" + std::string(model_name) + "\"";
    }
    return "Could not delete: " + std::string(error);
}

StorageSummary storage_summary(const CachedModelsSnapshot::Inventory& inventory) {
    StorageSummary summary;
    for (const SpeechModel& model : models()) {
        ++summary.total;
        const ModelDetails* details = find_model_details(model.key);
        if (details == nullptr) {
            continue;
        }
        const auto found = inventory.find(details->repository_id);
        if (found == inventory.end()) {
            continue;
        }
        const std::int64_t size = found->second.size_on_disk;
        ++summary.downloaded;
        summary.bytes += size;
        const SpeechBackend* owner = backend(model.backend);
        const std::string label = owner != nullptr ? std::string(owner->label) : "Whisper";
        const auto slot = std::ranges::find(summary.by_backend, label,
                                            &std::pair<std::string, std::int64_t>::first);
        if (slot == summary.by_backend.end()) {
            summary.by_backend.emplace_back(label, size);
        } else {
            slot->second += size;
        }
    }
    return summary;
}

StorageSummary storage_summary(const SpeechModelCache& cache) {
    CachedModelsSnapshot::Inventory inventory;
    for (CachedModelInfo& info : cache.inventory()) {
        std::string repo = info.repo_id;
        inventory.insert_or_assign(std::move(repo), std::move(info));
    }
    return storage_summary(inventory);
}

std::string downloads_rail_value(const DownloadQueue& queue, bool checking,
                                 const StorageSummary& summary) {
    return downloads_rail_value(queue.downloading_model(), queue.download_fraction(), checking,
                                summary);
}

std::string downloads_rail_value(const std::optional<std::string>& downloading,
                                 std::optional<double> fraction, bool checking,
                                 const StorageSummary& summary) {
    if (downloading && !downloading->empty()) {
        const std::string name = model_display_name(*downloading);
        if (fraction) {
            return "Downloading " + name + " · " + std::to_string(percent_of(*fraction)) + "%";
        }
        return "Downloading " + name + "…";
    }
    if (checking) {
        return "Checking downloaded models…";
    }
    return std::to_string(summary.downloaded) + " of " + std::to_string(summary.total) + " · " +
           format_size_bytes(summary.bytes);
}

std::string models_tab_title(bool checking, int downloaded, int total) {
    if (checking) {
        return "Speech models";
    }
    return "Speech models · " + std::to_string(downloaded) + " of " + std::to_string(total) +
           " downloaded";
}

std::string models_stats_text(bool checking, int downloaded, int total, std::int64_t bytes,
                              std::string_view folder) {
    if (checking) {
        return "Checking downloaded models… · " + std::string(folder);
    }
    return std::to_string(downloaded) + " of " + std::to_string(total) + " speech models · " +
           format_size_bytes(bytes) + " used · " + std::string(folder);
}

std::optional<std::string> components_tab_title(std::span<const ComponentInfo> infos) {
    if (infos.empty()) {
        return std::nullopt;
    }
    const auto usable = std::ranges::count_if(infos, &ComponentInfo::is_usable);
    const auto updates = std::ranges::count_if(infos, [](const ComponentInfo& info) {
        return info.state == ComponentState::UpdateAvailable;
    });
    std::string title = "Components · " + std::to_string(usable) + " of " +
                        std::to_string(infos.size()) + " installed";
    if (updates > 0) {
        title += " · " + std::to_string(updates) + " update" + (updates > 1 ? "s" : "");
    }
    return title;
}

ModelRowState model_row_state(std::string_view model_name, const ModelRowInput& input) {
    ModelRowState state;
    const bool cached = input.info.has_value();
    state.active_style = input.is_active && cached;

    if (cached) {
        state.size_text = format_size_bytes(input.info->size_on_disk);
        state.size_muted = false;
        state.sort_size_bytes = static_cast<double>(input.info->size_on_disk);
    } else {
        state.size_text = format_download_size(model_name).value_or("size unknown");
        state.size_muted = true;
        const auto size_mb = model_download_size_mb(model_name);
        state.sort_size_bytes = size_mb ? static_cast<double>(*size_mb * 1'000'000)
                                        : std::numeric_limits<double>::infinity();
    }

    state.progress_visible = input.downloading;
    if (input.downloading) {
        state.badge_text = "Downloading…";
        state.badge_tone = "downloading";
    } else if (input.queued) {
        state.badge_text = "Queued";
        state.badge_tone = "queued";
    } else if (input.is_active && cached) {
        state.badge_text = "Active";
        state.badge_tone = "active";
    } else if (cached) {
        state.badge_text = "Downloaded";
        state.badge_tone = "downloaded";
    } else {
        state.badge_text = "Not downloaded";
        state.badge_tone = "idle";
    }
    // The Download button already says a model is not here; the chip only adds something for
    // the other states.
    state.badge_visible = input.downloading || input.queued || cached;

    state.checkbox_visible = !cached;
    state.checkbox_enabled = input.selection_enabled && !input.downloading;
    state.download_visible = !cached;
    state.download_enabled =
        !input.downloading && !input.downloads_blocked && !input.download_slot_busy;
    if (input.downloads_blocked) {
        state.download_tooltip = "Downloads are disabled by HF_HUB_OFFLINE";
    } else if (input.download_slot_busy && !input.downloading) {
        state.download_tooltip = "Another download is in progress";
    }

    state.set_active_visible = cached && !input.is_active;
    state.set_active_enabled = !input.downloading;

    state.delete_visible = cached;
    state.delete_enabled = !input.is_loaded && !input.downloading;
    if (input.is_loaded) {
        state.delete_tooltip = "In use — switch models first";
    }
    return state;
}

std::string model_row_summary(std::string_view model_name) {
    if (const SpeechModel* found = model(model_name)) {
        return std::string(found->languages) + (found->streaming ? " / Live meetings" : "");
    }
    // A name outside the catalog reads as OpenWhisper read a Whisper name.
    std::string summary = model_name.ends_with(".en") ? "English only" : "Multilingual";
    if (model_name.starts_with("distil-")) {
        summary += " / Distilled";
    }
    return summary;
}

std::string model_row_label(std::string_view model_name) {
    return model_display_name(model_name);
}

RowProgress model_row_progress(std::int64_t done, std::int64_t total) {
    RowProgress progress;
    if (total <= 0) {
        progress.indeterminate = true;
        return progress;
    }
    progress.percent =
        static_cast<int>(static_cast<double>(done) * 100 / static_cast<double>(total));
    progress.size_text = format_size_bytes(done) + " of " + format_size_bytes(total);
    return progress;
}

ComponentRowState component_row_state(const ComponentInfo& info, bool installing,
                                      bool was_installing) {
    ComponentRowState state;
    state.name = info.display_name;
    state.summary = info.summary;
    if (!info.reason.empty()) {
        state.summary = info.summary.empty() ? info.reason : info.summary + "  —  " + info.reason;
    }

    if (installing) {
        state.badge_text = "Downloading…";
        state.badge_tone = "downloading";
        state.install_text = std::string("Cancel");
        state.install_enabled = true;
        state.remove_visible = false;
        state.progress_visible = true;
        state.remove_guard = RemoveGuard::Keep;
        return state;
    }

    state.progress_visible = false;
    state.remove_guard = was_installing ? RemoveGuard::Start : RemoveGuard::Release;

    switch (info.state) {
    case ComponentState::NotInstalled:
        state.badge_text = "Not installed";
        state.badge_tone = "idle";
        break;
    case ComponentState::External:
        state.badge_text = "Available";
        state.badge_tone = "installed";
        break;
    case ComponentState::Installed:
        state.badge_text = "Installed";
        state.badge_tone = "installed";
        break;
    case ComponentState::UpdateAvailable:
        state.badge_text = "Update available";
        state.badge_tone = "warning";
        break;
    case ComponentState::Incompatible:
        state.badge_text = "Update required";
        state.badge_tone = "warning";
        break;
    case ComponentState::Broken:
        state.badge_text = "Damaged";
        state.badge_tone = "warning";
        break;
    }

    if (info.staged != StagedChange::None) {
        // The change is made; only a restart finishes it, so the row offers nothing more.
        state.badge_text = "Restart needed";
        state.badge_tone = "warning";
        state.size_text = format_size_bytes(info.install_bytes);
        state.install_visible = false;
        state.remove_visible = false;
        return state;
    }
    if (info.state == ComponentState::External || info.state == ComponentState::Installed) {
        state.size_text = info.state == ComponentState::External
                              ? std::string("Existing setup")
                              : format_size_bytes(info.install_bytes);
        state.install_visible = false;
        state.remove_visible = info.state == ComponentState::Installed;
        return state;
    }
    state.size_text = info.download_bytes != 0
                          ? format_size_bytes(info.download_bytes) + " download"
                          : std::string();
    state.install_visible = true;
    switch (info.state) {
    case ComponentState::UpdateAvailable:
        state.install_text = std::string("Update");
        break;
    case ComponentState::Incompatible:
        state.install_text = std::string("Reinstall");
        break;
    case ComponentState::Broken:
        state.install_text = std::string("Repair");
        break;
    default:
        state.install_text = std::string("Install");
        break;
    }
    // The catalog ships with the app, so a component always has a size to download.
    state.install_enabled = true;
    state.install_tooltip = std::string();
    // A pending update is an offer, not an obligation: the component can still be removed.
    state.remove_visible = info.state == ComponentState::Broken ||
                           info.state == ComponentState::Incompatible ||
                           info.state == ComponentState::UpdateAvailable;
    return state;
}

std::string component_phase_badge(std::string_view phase) {
    if (phase == install_phase::kResolving) {
        return "Preparing…";
    }
    if (phase == install_phase::kDownloading) {
        return "Downloading…";
    }
    if (phase == install_phase::kVerifying) {
        return "Verifying…";
    }
    if (phase == install_phase::kExtracting) {
        return "Installing…";
    }
    if (phase == install_phase::kFinalizing) {
        return "Finishing…";
    }
    return "Working…";
}

RowProgress component_row_progress(std::string_view phase, std::int64_t done, std::int64_t total) {
    RowProgress progress;
    if (total <= 0) {
        progress.indeterminate = true;
        progress.size_text = std::string();
        return progress;
    }
    progress.percent =
        static_cast<int>(static_cast<double>(done) * 100 / static_cast<double>(total));
    progress.size_text = phase == install_phase::kDownloading
                             ? format_size_bytes(done) + " of " + format_size_bytes(total)
                             : std::string();
    return progress;
}

} // namespace fv::models
