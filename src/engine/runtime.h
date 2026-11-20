#pragma once

#include <filesystem>

namespace fv {

// The file name of the NeMo-Speech.cpp ASR library on this platform.
const char* runtime_library_name();

// Finds the ASR library inside an unpacked NeMo-Speech.cpp release. Windows
// releases keep it in bin/, Linux and macOS ones in lib/; a release unpacked
// with its top folder keeps either under nemo-speech/. Throws EngineError
// naming `runtime_dir` when none is there.
std::filesystem::path find_runtime_library(const std::filesystem::path& runtime_dir);

// Whether the release in `runtime_dir` is a GPU build: the runtime library's folder holds a GPU
// backend of ggml (CUDA on Windows and Linux, Vulkan on Linux, Metal on macOS). False when the
// folder has no runtime library.
bool runtime_has_gpu_backend(const std::filesystem::path& runtime_dir);

} // namespace fv
