#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/json.h"
#include "models/catalog.h"
#include "models/gpu_probe.h"

// The copy of the download and runtime prompts (ui_qt/dialogs/hf_consent_dialog.py _title_text and
// _body_text, required_runtime_dialog.py, settings_downloads.py BatchDownloadDialog and the delete
// and remove confirmations). The dialogs (fv_ui_downloads) lay it out; deciding when they show is
// the controller's.
// Affinity: thread-safe (pure functions).
namespace fv::models {

// --- Download consent (HuggingFaceConsentDialog) ---

inline constexpr std::string_view kConsentWindowTitle = "Download Speech Model";

// 'Download "<model>" (about 714 MB)?', or 'Download "<model>" model?' without a bundled size.
std::string consent_title(std::string_view model_name);

// The runtimes a model still needs: missing_runtime for its selected device, and the GPU runtime
// offered beside it (gpu_runtime_offer), asked only when a runtime is missing.
struct RuntimeNeeds {
    std::optional<std::string> runtime;
    std::optional<std::string> gpu_runtime;
};
RuntimeNeeds runtime_needs(std::string_view model_name, const Json& settings, const Host& host);

// The notice: where the download comes from, publisher, licence, verification and version, the
// security notice, the runtime the model also needs, and the HF_HUB_OFFLINE or "Never connect"
// note. Paragraphs are separated by a blank line.
std::string consent_body(std::string_view model_name, std::string_view policy, bool env_blocked,
                         const RuntimeNeeds& needs);

// The note under the notice.
std::string_view consent_storage_note();

// --- Required runtime (RequiredRuntimeDialog) and the first-use GPU choice (GpuRuntimeDialog) ---

inline constexpr std::string_view kRequiredRuntimeWindowTitle = "Required speech runtime";
inline constexpr std::string_view kGpuRuntimeWindowTitle = "Choose a speech runtime";

// A runtime's archive bytes summed and its installed bytes, for this host's platform (0 and 0
// without an entry).
struct RuntimeSizes {
    std::int64_t download = 0;
    std::int64_t on_disk = 0;
};
RuntimeSizes runtime_sizes(std::string_view component_id, const Host& host);

// "<runtime> is required".
std::string required_runtime_title(std::string_view component_id);
// The explanation, with the runtime's download size.
std::string required_runtime_body(std::string_view model_name, std::string_view component_id,
                                  std::int64_t download_bytes);

// "Run Parakeet on the RTX 2060?" ("this GPU" without a GPU).
std::string gpu_runtime_title(std::string_view model_name, const std::optional<NvidiaGpu>& gpu);
// Both choices with their sizes; sizes and the card's short name keep non-breaking spaces.
std::string gpu_runtime_body(std::string_view model_name, std::string_view gpu_component,
                             std::string_view cpu_component, const std::optional<NvidiaGpu>& gpu,
                             const RuntimeSizes& gpu_sizes, const RuntimeSizes& cpu_sizes);

// --- Batch download (BatchDownloadDialog) ---

inline constexpr std::string_view kBatchWindowTitle = "Download models";

// "Download 2 models?"
std::string batch_download_title(std::size_t count);
// One line per model: "<name>  ·  ~714 MB" ("size unknown" without an estimate).
std::string batch_model_list(std::span<const std::string> model_names);
// "Estimated total: ~1.43 GB", or "Estimated total: unknown" when a model has no estimate.
std::string batch_total_text(std::span<const std::string> model_names);
// "Download 2 models…"
std::string batch_download_button(std::size_t count);
std::string_view batch_note();

// --- Confirmations ---

struct Confirmation {
    std::string title;
    std::string text;
};
// Deleting a downloaded model's files.
Confirmation delete_model_confirmation(std::string_view model_name);
// Removing an installed component.
Confirmation remove_component_confirmation(std::string_view display_name,
                                           std::int64_t install_bytes);

} // namespace fv::models
