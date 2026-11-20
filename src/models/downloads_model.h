#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "models/catalog.h"
#include "models/hf_access.h"
#include "models/speech_model_cache.h"

// The Qt-free state of Settings > Downloads (ui_qt/dialogs/settings_downloads.py,
// ui_qt/widgets/model_row_widget.py and component_row_widget.py): row states, sort and filter,
// the selection bar, the download queue's messages, the rail value, tab titles and the storage
// summary. The page and the rows render what these return. FioriVoce's catalog is the speech
// models alone: the Whisper rows, custom models and the Hugging Face cache folder are not ported.
namespace fv::models {

// _current_cache_info: the cached copy, unless it lacks the revision the catalog pins. OpenWhisper
// passes model_revision(name), which knows only Whisper's pins, so a speech model's row passes
// none and its inventory entry stands.
std::optional<CachedModelInfo> current_cache_info(const std::optional<CachedModelInfo>& info,
                                                  std::string_view revision);

// What sorting, filtering and the selection read of one model row.
struct ModelRowFacts {
    std::string model_name;
    std::string repo_id;
    std::string backend;
    bool is_cached = false;
    // The cached size, else the bundled estimate in bytes; +infinity when neither is known.
    double sort_size_bytes = 0;
};

// The facts of `model_name`'s row given its (current) cache entry.
ModelRowFacts model_row_facts(std::string_view model_name,
                              const std::optional<CachedModelInfo>& info);

// The model rows' names in catalog order: the order rows are built and batches download in.
std::vector<std::string> model_row_names();

// The sort combo's data values.
namespace sort_mode {
inline constexpr std::string_view kRecommended = "recommended";
inline constexpr std::string_view kDownloaded = "downloaded";
inline constexpr std::string_view kSize = "size";
inline constexpr std::string_view kName = "name";
inline constexpr std::string_view kBackend = "backend";
} // namespace sort_mode

// The backend filter combo's data values in its order: "all", then each backend by its label,
// case-folded. backend_filter_index is a value's position there, -1 when absent (findData).
std::vector<std::string> backend_filter_values();
int backend_filter_index(std::string_view backend);

// _sort_key. Each mode's tuple fits this shape with the unused fields left at their defaults:
// "downloaded" (not cached, name), "size" (size, name), "name" (name), "backend" (filter index,
// not cached, size, name), anything else "recommended" (not cached, size, name). OpenWhisper's
// recommended order first puts the MLX models on a supported Mac; FioriVoce has none.
struct SortKey {
    int group = 0;
    bool not_cached = false;
    double size = 0;
    std::string name; // case-folded

    bool operator<(const SortKey& other) const;
};
SortKey sort_key(std::string_view mode, const ModelRowFacts& row);
// The rows sorted as the page lays them out (a stable sort by sort_key).
std::vector<ModelRowFacts> sorted_rows(std::vector<ModelRowFacts> rows, std::string_view mode);

// uimath's fuzzy_match(query, target), passed in so fv_models does not depend on fv_uimath.
using FuzzyMatch = std::function<bool(std::string_view query, std::string_view target)>;
// ModelRowWidget.matches_filter: the needle against the model name or the repository id.
bool matches_filter(std::string_view needle, const ModelRowFacts& row, const FuzzyMatch& fuzzy);
// _apply_filter for one row: the search text (stripped, lower-cased; empty matches all), the
// backend filter value ("all" or a backend) and the status filter ("all", "downloaded",
// "not_downloaded").
bool row_visible(const ModelRowFacts& row, std::string_view search_text, std::string_view backend,
                 std::string_view status, const FuzzyMatch& fuzzy);

// _update_selection_bar.
struct SelectionBarInput {
    std::vector<double> selected_sizes; // sort_size_bytes of each ticked row
    bool any_missing = false;           // some row still needs a download
    bool busy = false;                  // a download runs or a batch is queued
    bool downloads_blocked = false;     // HF_HUB_OFFLINE
};
struct SelectionBar {
    std::string summary;
    std::string download_text;
    bool download_enabled = false;
    std::string download_tooltip;
    bool clear_enabled = false;
    bool select_all_enabled = false;
    bool download_all_enabled = false;
    bool download_all_visible = false;
};
SelectionBar selection_summary(const SelectionBarInput& input);
// While the inventory is still being read, a refresh then disables Select all, Download all and
// Download selected (_refresh_cached_model_state); a tick alone does not.
void lock_while_checking(SelectionBar& bar);

// _usage_for: "Dictation", "Meetings", both joined by " · ", or "".
std::string usage_text(std::string_view model_name, std::string_view dictation_model,
                       std::string_view meeting_model);

// _models_in_use: the dictation model (the loaded one when dictation is on Auto) and the meeting
// model, "" when meetings use remote speech. The meeting model is resolved by the caller (meeting
// speech settings are not part of fv_models). With a backend other than Parakeet selected,
// OpenWhisper took its Whisper model, "auto" unless one was saved; FioriVoce reads no Whisper
// setting, so that case is Auto.
struct ModelsInUse {
    std::string dictation;
    std::string meeting;
    std::optional<std::string> loaded;
};
ModelsInUse models_in_use(const Json& settings, std::string meeting_model,
                          std::optional<std::string> loaded_model);

// The label rail values and messages give a model: its catalog label, else the name.
std::string model_display_name(std::string_view model_name);

// The download in flight and the batch queue, with the messages each step shows (DownloadsPage's
// set_downloading, set_download_progress, finish_download, begin_batch and finish_batch). A
// message of nullopt leaves the message line as it is.
// Affinity: GUI thread (one instance per page).
class DownloadQueue {
public:
    struct Progress {
        std::optional<std::string> message;
        bool rail_changed = false; // the rounded percentage moved (progress_changed)
    };

    std::string set_downloading(std::string_view model_name);
    Progress set_download_progress(std::string_view model_name, std::int64_t done,
                                   std::int64_t total);
    std::optional<std::string> finish_download(std::string_view model_name, bool success);
    // Enters batch mode with the approved plan, keeping only names `is_row` knows.
    std::string begin_batch(std::span<const std::string> model_names,
                            const std::function<bool(std::string_view)>& is_row);
    // Closes the batch with a summary of what actually downloaded.
    std::string finish_batch(int completed, int planned);

    const std::optional<std::string>& downloading_model() const { return downloading_; }
    const std::optional<double>& download_fraction() const { return fraction_; }
    const std::vector<std::string>& batch_queue() const { return queue_; }
    int batch_done() const { return done_; }
    int batch_failed() const { return failed_; }
    // _download_slot_busy: a download runs or a batch waits.
    bool slot_busy() const { return downloading_.has_value() || !queue_.empty(); }
    // A row's "Queued" state: waiting in the batch and not the one downloading.
    bool is_queued(std::string_view model_name) const;

private:
    std::optional<std::string> downloading_;
    std::optional<double> fraction_;
    std::vector<std::string> queue_;
    int done_ = 0;
    int failed_ = 0;
};

// show_delete_result's message.
std::string delete_result_message(std::string_view model_name, bool success,
                                  std::string_view error);

// storage_summary: downloaded count, catalog size, bytes, and bytes per backend label in row
// order. The sizes are the inventory's, as listed (no revision check).
struct StorageSummary {
    int downloaded = 0;
    int total = 0;
    std::int64_t bytes = 0;
    std::vector<std::pair<std::string, std::int64_t>> by_backend;
};
StorageSummary storage_summary(const CachedModelsSnapshot::Inventory& inventory);
StorageSummary storage_summary(const SpeechModelCache& cache);

// rail_value: the download in flight ("Downloading <label> · 40%" or "Downloading <label>…"),
// "Checking downloaded models…" while the inventory loads, else "<n> of <total> · <size>".
std::string downloads_rail_value(const DownloadQueue& queue, bool checking,
                                 const StorageSummary& summary);
std::string downloads_rail_value(const std::optional<std::string>& downloading,
                                 std::optional<double> fraction, bool checking,
                                 const StorageSummary& summary);

// The models tab title (_show_cache_totals): "Speech models" while checking, else
// "Speech models · <n> of <total> downloaded".
std::string models_tab_title(bool checking, int downloaded, int total);
// The stats line beside it: "Checking downloaded models… · <folder>" or
// "<n> of <total> speech models · <size> used · <folder>".
std::string models_stats_text(bool checking, int downloaded, int total, std::int64_t bytes,
                              std::string_view folder);
// refresh_components' tab title: "Components · <usable> of <n> installed" plus
// " · <k> update(s)"; nullopt without components (the title is left alone).
std::optional<std::string> components_tab_title(std::span<const ComponentInfo> infos);

// ModelRowWidget.update_state.
struct ModelRowInput {
    std::optional<CachedModelInfo> info; // the current cache entry, nullopt when not downloaded
    bool is_active = false;
    bool is_loaded = false; // the engine has it loaded: its files are mapped, so no Delete
    bool downloading = false;
    bool downloads_blocked = false;
    bool download_slot_busy = false;
    bool queued = false;
    bool selection_enabled = true;
};
struct ModelRowState {
    bool active_style = false; // the "active" property
    std::string size_text;
    bool size_muted = false;
    double sort_size_bytes = 0;
    std::string badge_text;
    std::string badge_tone; // "downloading", "queued", "active", "downloaded", "idle"
    bool badge_visible = false;
    bool progress_visible = false; // hidden progress also resets to 0
    bool checkbox_visible = false;
    bool checkbox_enabled = false;
    bool download_visible = false;
    bool download_enabled = false;
    std::string download_tooltip;
    bool set_active_visible = false;
    bool set_active_enabled = false;
    bool delete_visible = false;
    bool delete_enabled = false;
    std::string delete_tooltip;
};
ModelRowState model_row_state(std::string_view model_name, const ModelRowInput& input);

// _model_summary: the languages, plus " / Live meetings" for a streaming model.
std::string model_row_summary(std::string_view model_name);
// The row's name label (speech_model_picker_label: the catalog label off Apple Silicon).
std::string model_row_label(std::string_view model_name);

// ModelRowWidget.set_progress and ComponentRowWidget.set_progress. A size text of nullopt leaves
// the size label as it is.
struct RowProgress {
    bool indeterminate = false; // total unknown: a busy bar
    int percent = 0;
    std::optional<std::string> size_text;
};
RowProgress model_row_progress(std::int64_t done, std::int64_t total);

// ComponentRowWidget.update_state. Optional fields are left as they are while an install runs.
enum class RemoveGuard {
    Keep,    // an install is running: Remove is hidden and untouched
    Start,   // an install just ended: disable Remove for kRemoveGuardMs
    Release, // enable Remove unless the guard is still running
};
// Once an install ends, Remove takes the place of Install/Cancel under the pointer; it stays
// disabled this long so the second click of a double-click cannot land on it.
inline constexpr int kRemoveGuardMs = 600;
struct ComponentRowState {
    std::string name;
    std::string summary; // the summary and the reason joined by "  —  "
    std::string badge_text;
    std::string badge_tone; // "downloading", "idle", "installed", "warning"
    std::optional<std::string> size_text;
    bool progress_visible = false; // hidden progress also resets to 0
    // Install doubles as Cancel while installing; an installed or external component only hides
    // it, keeping its last text, enable and tooltip.
    std::optional<bool> install_visible;
    std::optional<std::string> install_text;
    std::optional<bool> install_enabled;
    std::optional<std::string> install_tooltip;
    bool remove_visible = false;
    RemoveGuard remove_guard = RemoveGuard::Keep;
};
ComponentRowState component_row_state(const ComponentInfo& info, bool installing,
                                      bool was_installing);
// The badge text of an install phase ("Downloading…"; "Working…" for an unknown one), with the
// "downloading" tone. Only applies while installing.
std::string component_phase_badge(std::string_view phase);
RowProgress component_row_progress(std::string_view phase, std::int64_t done, std::int64_t total);

} // namespace fv::models
