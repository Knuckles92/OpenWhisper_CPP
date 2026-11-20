#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "walkthrough/document.h"

// The PDF and Word renderers, which need Qt Gui, behind an interface: fv_documents implements it
// (documents::QtDocumentRenderer) and the composition installs it, so this library, startup
// recovery and their tests never load Qt (services/walkthrough/library.py:121-123 keeps the Qt
// renderer out of recovery the same way).
namespace fv::walkthrough {

// A document could not be rendered or written; what() says why.
class DocumentError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Affinity: thread-safe; called on worker threads only (the renderer needs the
// QGuiApplication to exist, never the GUI thread).
class DocumentRenderer {
public:
    virtual ~DocumentRenderer() = default;
    // folder/walkthrough.pdf and folder/walkthrough.docx (document.py write_pdf, word.py
    // write_docx); the path written.
    virtual std::filesystem::path write_pdf(const Walkthrough& walkthrough,
                                            const std::filesystem::path& folder,
                                            const DocumentFormat& format, PaperSize paper) = 0;
    virtual std::filesystem::path write_docx(const Walkthrough& walkthrough,
                                             const std::filesystem::path& folder,
                                             const DocumentFormat& format, PaperSize paper) = 0;
    // Any Markdown (a write-up) as a PDF or Word document at `target`, its images read from
    // `folder` (document.py write_markdown_pdf, word.py write_markdown_docx).
    virtual void write_markdown_pdf(std::string_view text, const std::filesystem::path& target,
                                    const std::filesystem::path& folder, std::string_view title,
                                    const Look& look, PaperSize paper) = 0;
    virtual void write_markdown_docx(std::string_view text, const std::filesystem::path& target,
                                     const std::filesystem::path& folder, std::string_view title,
                                     const Look& look, PaperSize paper) = 0;
};

// Installs the renderer every caller of document_renderer() gets; nullptr removes it.
void set_document_renderer(std::shared_ptr<DocumentRenderer> renderer);
// The installed renderer. Throws std::logic_error when none is installed.
DocumentRenderer& document_renderer();

} // namespace fv::walkthrough
