#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>

// The PDFium entry points the knowledge folder calls, declared here so no PDFium header is
// vendored or linked (fpdfview.h and fpdf_text.h of pdfium-binaries; FPDF_CALLCONV is the default
// convention on x64). Handles are opaque.
namespace fv::meeting_intel::detail {

using FpdfDocument = void*;
using FpdfPage = void*;
using FpdfTextPage = void*;

// FPDF_GetLastError's codes.
inline constexpr unsigned long kFpdfErrPassword = 4;

struct PdfiumApi {
    void (*init_library)() = nullptr; // FPDF_InitLibrary
    FpdfDocument (*load_mem_document64)(const void*, std::size_t, const char*) = nullptr;
    void (*close_document)(FpdfDocument) = nullptr;                          // FPDF_CloseDocument
    int (*get_page_count)(FpdfDocument) = nullptr;                           // FPDF_GetPageCount
    FpdfPage (*load_page)(FpdfDocument, int) = nullptr;                      // FPDF_LoadPage
    void (*close_page)(FpdfPage) = nullptr;                                  // FPDF_ClosePage
    FpdfTextPage (*text_load_page)(FpdfPage) = nullptr;                      // FPDFText_LoadPage
    void (*text_close_page)(FpdfTextPage) = nullptr;                         // FPDFText_ClosePage
    int (*text_count_chars)(FpdfTextPage) = nullptr;                         // FPDFText_CountChars
    int (*text_get_text)(FpdfTextPage, int, int, unsigned short*) = nullptr; // FPDFText_GetText
    unsigned long (*get_last_error)() = nullptr;                             // FPDF_GetLastError
};

// Loads the library at `absolute` by full path (its dependencies from its own folder and System32
// only) and resolves every entry point. The library is never unloaded. The error is a sentence.
// Implemented in win/ and posix/.
std::expected<PdfiumApi, std::string> load_pdfium(const std::filesystem::path& absolute);

} // namespace fv::meeting_intel::detail
