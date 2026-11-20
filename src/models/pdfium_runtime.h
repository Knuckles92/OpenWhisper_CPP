#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "base/env.h"
#include "base/json.h"
#include "models/component_store.h"

// Where PDFium (which reads the knowledge folder's PDFs) is found. OpenWhisper reads PDFs with
// pypdf, a Python package; OpenWhisper_CPP loads PDFium by path, from a downloadable component or a
// file the user names. Loading it is fv_meeting_intel's.
namespace fv::models {

inline constexpr std::string_view kPdfiumComponent = "pdfium";

// The library's file name on this platform.
#if defined(_WIN32)
inline constexpr std::string_view kPdfiumLibrary = "pdfium.dll";
#elif defined(__APPLE__)
inline constexpr std::string_view kPdfiumLibrary = "libpdfium.dylib";
#else
inline constexpr std::string_view kPdfiumLibrary = "libpdfium.so";
#endif

// The pdfium component's catalog, in builtin_catalog()'s shape ({"schema": 2, "components":
// {"pdfium": {"platforms": {tag: entry}}}}), for catalog_entry() and component_is_published().
// Its Windows x64 entry is "published": false with no archive until a pdfium-binaries release's
// URL, size and SHA-256 are pinned, so nothing offers to install it. Shared and immutable.
const Json& pdfium_catalog();

// The PDFium library to load, or nullopt: OPENWHISPER_CPP_PDFIUM when it names an existing file
// (anything else is logged and skipped), then the installed pdfium component's library (in the
// release archive's bin/ or lib/ folder, or at the top of the component).
std::optional<std::filesystem::path> locate_pdfium(const EnvReader& env,
                                                   const ComponentStore& store);

} // namespace fv::models
