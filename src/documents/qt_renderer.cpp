#include "documents/qt_renderer.h"

#include "documents/docx.h"
#include "documents/pdf.h"

namespace fv::documents {

std::filesystem::path QtDocumentRenderer::write_pdf(const walkthrough::Walkthrough& walkthrough,
                                                    const std::filesystem::path& folder,
                                                    const walkthrough::DocumentFormat& format,
                                                    walkthrough::PaperSize paper) {
    return writePdf(walkthrough, folder, format, pageSizeFor(paper)).path;
}

std::filesystem::path QtDocumentRenderer::write_docx(const walkthrough::Walkthrough& walkthrough,
                                                     const std::filesystem::path& folder,
                                                     const walkthrough::DocumentFormat& format,
                                                     walkthrough::PaperSize paper) {
    return writeDocx(walkthrough, folder, format, pageSizeFor(paper));
}

void QtDocumentRenderer::write_markdown_pdf(std::string_view text,
                                            const std::filesystem::path& target,
                                            const std::filesystem::path& folder,
                                            std::string_view title, const walkthrough::Look& look,
                                            walkthrough::PaperSize paper) {
    writeMarkdownPdf(text, target, folder, title, pageSizeFor(paper), look);
}

void QtDocumentRenderer::write_markdown_docx(std::string_view text,
                                             const std::filesystem::path& target,
                                             const std::filesystem::path& folder,
                                             std::string_view title, const walkthrough::Look& look,
                                             walkthrough::PaperSize paper) {
    writeMarkdownDocx(text, target, folder, title, pageSizeFor(paper), look);
}

} // namespace fv::documents
