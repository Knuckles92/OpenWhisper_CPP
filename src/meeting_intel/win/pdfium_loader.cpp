#include "meeting_intel/detail/pdfium_api.h"

#include <string>
#include <type_traits>

#include "base/utf8.h"

#include <windows.h>

namespace fv::meeting_intel::detail {

namespace {

std::string last_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message(buffer ? buffer : L"", length);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ' || message.back() == L'.')) {
        message.pop_back();
    }
    std::string text = path_to_utf8(std::filesystem::path(message));
    return text.empty() ? "error " + std::to_string(code) : text;
}

} // namespace

std::expected<PdfiumApi, std::string> load_pdfium(const std::filesystem::path& absolute) {
    // Dependencies come from the library's own folder and System32 only, never the working
    // directory or PATH. The error mode keeps Windows from answering a damaged or
    // wrong-architecture DLL with a dialog of its own; the load just fails.
    DWORD previous_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_mode);
    HMODULE module = LoadLibraryExW(
        absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD code = module == nullptr ? GetLastError() : 0;
    SetThreadErrorMode(previous_mode, nullptr);
    if (module == nullptr) {
        return std::unexpected("Could not load PDFium from " + path_to_utf8(absolute) + ": " +
                               last_error_text(code) + ".");
    }
    // Never freed: a loaded library stays loaded until the process ends.
    PdfiumApi api;
    std::string missing;
    const auto resolve = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
            reinterpret_cast<void*>(GetProcAddress(module, name)));
        if (function == nullptr && missing.empty()) {
            missing = name;
        }
    };
    resolve(api.init_library, "FPDF_InitLibrary");
    resolve(api.load_mem_document64, "FPDF_LoadMemDocument64");
    resolve(api.close_document, "FPDF_CloseDocument");
    resolve(api.get_page_count, "FPDF_GetPageCount");
    resolve(api.load_page, "FPDF_LoadPage");
    resolve(api.close_page, "FPDF_ClosePage");
    resolve(api.text_load_page, "FPDFText_LoadPage");
    resolve(api.text_close_page, "FPDFText_ClosePage");
    resolve(api.text_count_chars, "FPDFText_CountChars");
    resolve(api.text_get_text, "FPDFText_GetText");
    resolve(api.get_last_error, "FPDF_GetLastError");
    if (!missing.empty()) {
        return std::unexpected(path_to_utf8(absolute) + " is not PDFium: it has no " + missing +
                               ".");
    }
    return api;
}

} // namespace fv::meeting_intel::detail
