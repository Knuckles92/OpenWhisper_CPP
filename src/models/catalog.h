#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "models/gpu_probe.h"

// The built-in catalog of downloadable components (services/components.py:339-680): the
// NeMo-Speech.cpp runtimes, pinned with immutable URLs, sizes and SHA-256 digests, per platform.
// The catalog ships in the app and is never fetched. OpenWhisper's GPU Acceleration, meeting
// agents and other engines' runtimes are not in it.
namespace fv::models {

// Bumped when the app changes in a way that invalidates installed component payloads.
inline constexpr int kComponentApi = 1;

namespace platform {
inline constexpr std::string_view kWinAmd64 = "win_amd64";
inline constexpr std::string_view kLinuxX86_64 = "linux_x86_64";
inline constexpr std::string_view kLinuxAarch64 = "linux_aarch64";
inline constexpr std::string_view kDarwinArm64 = "darwin_arm64";
inline constexpr std::string_view kDarwinX86_64 = "darwin_x86_64";
} // namespace platform

namespace component_id {
inline constexpr std::string_view kAsrNvidiaCpu = "asr-nvidia-cpu";
inline constexpr std::string_view kAsrNvidiaCuda = "asr-nvidia-cuda";
inline constexpr std::string_view kAsrNvidiaVulkan = "asr-nvidia-vulkan";
inline constexpr std::string_view kAsrNvidiaMetal = "asr-nvidia-metal";
} // namespace component_id

// Lifecycle state of a component on this machine, as ComponentState's strings.
enum class ComponentState {
    NotInstalled,
    External,
    Installed,
    UpdateAvailable,
    Incompatible,
    Broken
};
// "not_installed", "external", "installed", "update_available", "incompatible", "broken".
std::string_view state_name(ComponentState);

// Coarse install phases for progress, reported as (phase, done bytes, total bytes).
namespace install_phase {
inline constexpr std::string_view kResolving = "resolving";
inline constexpr std::string_view kDownloading = "downloading";
inline constexpr std::string_view kVerifying = "verifying";
inline constexpr std::string_view kExtracting = "extracting";
inline constexpr std::string_view kFinalizing = "finalizing";
} // namespace install_phase

// An install failed for a reason worth showing the user verbatim.
class ComponentError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The user cancelled an install in flight.
class ComponentCanceled : public std::runtime_error {
public:
    ComponentCanceled() : std::runtime_error("") {}
};

// A change to a component this process has loaded, waiting under the components root's .pending
// folder for the next start (ComponentStore::settle_staged_changes).
enum class StagedChange {
    None,
    Install, // a new tree (an install or an update) replaces the loaded one
    Removal, // the loaded tree is removed
};

// Everything the UI needs to render one component row.
struct ComponentInfo {
    std::string component_id;
    std::string display_name;
    std::string summary;
    ComponentState state = ComponentState::NotInstalled;
    std::optional<Json> installed_version; // the manifest's "version" (a str, int or float)
    std::optional<Json> available_version; // the catalog's
    std::int64_t download_bytes = 0;       // the archives' sizes summed
    std::int64_t install_bytes = 0;        // the catalog's, or the size on disk once installed
    std::string reason;
    // A change staged for the next start. `state` still describes the tree this run uses, and
    // `reason` says the change waits for a restart.
    StagedChange staged = StagedChange::None;

    bool is_usable() const {
        return state == ComponentState::External || state == ComponentState::Installed ||
               state == ComponentState::UpdateAvailable;
    }
};

// What decisions about this computer read: its platform tag, which components are installed, its
// NVIDIA GPU and its macOS version. Tests build one; Host::current() describes the real host.
struct Host {
    std::optional<std::string> platform; // a platform tag, nullopt where none applies
    std::function<bool(std::string_view component_id)> is_installed; // empty: nothing is
    std::function<std::optional<NvidiaGpu>()> gpu;                   // empty: no GPU
    std::optional<std::vector<int>> macos_version;                   // off macOS: nullopt

    bool installed(std::string_view component_id) const {
        return is_installed && is_installed(component_id);
    }
    std::optional<NvidiaGpu> nvidia() const { return gpu ? gpu() : std::nullopt; }

    // This computer: platform_tag(), the GPU nvidia_gpu() reports, the macOS version, and
    // `is_installed` (a ComponentStore's, usually).
    static Host current(std::function<bool(std::string_view)> is_installed);
};

// current_platform_tag(platform, machine): `sys_platform` as sys.platform spells it ("win32",
// "linux", "darwin"), `machine` as the OS reports the architecture (case and surrounding space
// ignored). Anything without components is nullopt.
std::optional<std::string> platform_tag(std::string_view sys_platform, std::string_view machine);
// This computer's tag (on Windows the kernel's architecture, not the process's).
std::optional<std::string> platform_tag();

// The built-in catalog, {"schema": 2, "components": {id: {"platforms": {tag: entry}}}}. Shared
// and immutable: callers copy what they change.
const Json& builtin_catalog();

// catalog_entry_for_platform: a copy of `component_id`'s entry for `tag` in `catalog`, or nullopt
// when either is missing or not an object. An empty tag finds nothing.
std::optional<Json> catalog_entry(std::string_view component_id, std::string_view tag,
                                  const Json& catalog = builtin_catalog());
// The same for host.platform.
std::optional<Json> catalog_entry(std::string_view component_id, const Host& host,
                                  const Json& catalog = builtin_catalog());

// Whether an entry has production-ready immutable artifacts: not "published": false, at least one
// archive, install_bytes above 0, and every archive with a 64-character digest that is not all
// zeros, an https:// URL and a size above 0.
bool component_is_published(std::string_view component_id, std::string_view tag,
                            const Json& catalog = builtin_catalog());
bool component_is_published(const Json& entry);

// The components installable on host.platform, in display order: Windows x64 the CPU and CUDA
// runtimes; Linux x86_64 those plus the Vulkan one when it is installed or nvidia_gpu_runtime
// picks it; Apple Silicon the CPU and Metal runtimes; Intel Macs the CPU runtime; elsewhere none.
// Each must be published, and (for this host only) not need a newer macOS.
std::vector<std::string> available_component_ids(const Host& host);
// The same for an explicit tag: the catalog, not this Mac, decides (no macOS minimum).
std::vector<std::string> available_component_ids(std::string_view tag, const Host& host);

// Why a manifest (or catalog entry) cannot run here, or nullopt: "Built for a different version
// of OpenWhisper_CPP (component API {api}, this app uses 1)", "Built for {platform}, which this app
// cannot use", "Requires macOS {min} or later". OpenWhisper's python_abi check is not ported: no
// OpenWhisper_CPP component carries Python.
std::optional<std::string> check_compatibility(const Json& manifest, const Host& host);

} // namespace fv::models
