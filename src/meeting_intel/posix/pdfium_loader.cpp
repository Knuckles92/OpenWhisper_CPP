#include "meeting_intel/detail/pdfium_api.h"

#include <string>
#include <type_traits>

#include <dlfcn.h>

#include "base/utf8.h"

namespace fv::meeting_intel::detail {

std::expected<PdfiumApi, std::string> load_pdfium(const std::filesystem::path& absolute) {
    void* library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* reason = dlerror();
        return std::unexpected("Could not load PDFium from " + path_to_utf8(absolute) + ": " +
                               (reason != nullptr ? reason : "unknown error") + ".");
    }
    // Never closed: a loaded library stays loaded until the process ends.
    PdfiumApi api;
    std::string missing;
    const auto resolve = [&](auto& function, const char* name) {
        function =
            reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
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
