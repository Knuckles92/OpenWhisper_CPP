#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

// PDF text for the knowledge folder through PDFium, which OpenWhisper's pypdf stands for here.
// PDFium is loaded by full path the first time a PDF is read and never unloaded; it is not
// thread-safe, so every call into it is serialized by one mutex.
// Affinity: thread-safe; blocks while another thread reads a PDF.
namespace fv::meeting_intel {

// A PDF PDFium could not open (other than for a password), or a page it could not read.
class PdfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Where the PDFium library is (models::locate_pdfium's answer); nullopt: PDFs are skipped. The
// composition sets it at startup. A library loaded from an earlier path stays loaded.
void set_pdfium_path(std::optional<std::filesystem::path> library);
std::optional<std::filesystem::path> pdfium_path();

// _extract_pdf: the text of each of the first `max_pages` pages that has any non-blank text,
// joined by "\n" (PDFium's "\r\n" line ends read as "\n", as pypdf writes them); "" for a PDF
// that needs a password. nullopt when no PDFium is set or it cannot be loaded (logged once per
// path). Throws PdfError for a file PDFium cannot read, std::system_error when it cannot be
// opened.
std::optional<std::string> pdf_text(const std::filesystem::path& pdf, int max_pages);

} // namespace fv::meeting_intel
