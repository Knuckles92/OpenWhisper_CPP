#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "base/env.h"
#include "base/json.h"
#include "models/component_store.h"

// Where ONNX Runtime (which runs the speaker model) is found. OpenWhisper installs it as a Python
// package; FioriVoce loads its CPU build by path, from a downloadable component or a file the
// user names. Loading it is fv_diarize's.
namespace fv::models {

inline constexpr std::string_view kOnnxRuntimeComponent = "onnxruntime-cpu";

// The runtime library's file name on this platform.
#if defined(_WIN32)
inline constexpr std::string_view kOnnxRuntimeLibrary = "onnxruntime.dll";
#elif defined(__APPLE__)
inline constexpr std::string_view kOnnxRuntimeLibrary = "libonnxruntime.dylib";
#else
inline constexpr std::string_view kOnnxRuntimeLibrary = "libonnxruntime.so";
#endif

// The onnxruntime-cpu component's catalog, in builtin_catalog()'s shape ({"schema": 2,
// "components": {"onnxruntime-cpu": {"platforms": {tag: entry}}}}), for catalog_entry() and
// component_is_published(). Its Windows x64 entry is "published": false with no archive until
// the release archive's URL, size and SHA-256 are pinned, so nothing offers to install it.
// Shared and immutable.
const Json& onnxruntime_catalog();

// The ONNX Runtime library to load, or nullopt: FIORIVOCE_ONNXRUNTIME when it names an existing
// file (anything else is logged and skipped), then the installed onnxruntime-cpu component's
// library (in the release archive's lib/ folder, or at the top of the component).
std::optional<std::filesystem::path> locate_onnxruntime(const EnvReader& env,
                                                        const ComponentStore& store);

} // namespace fv::models
