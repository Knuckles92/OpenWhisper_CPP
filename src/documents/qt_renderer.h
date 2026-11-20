#pragma once

#include <filesystem>
#include <string_view>

#include "walkthrough/renderer.h"

// The walkthrough documents rendered with Qt Gui, installed by the composition with
// walkthrough::set_document_renderer.
namespace fv::documents {

// Affinity: thread-safe; every call builds its own document and writer on the calling thread,
// which may be any thread once the QGuiApplication exists. Errors are walkthrough::DocumentError.
class QtDocumentRenderer final : public walkthrough::DocumentRenderer {
public:
    std::filesystem::path write_pdf(const walkthrough::Walkthrough& walkthrough,
                                    const std::filesystem::path& folder,
                                    const walkthrough::DocumentFormat& format,
                                    walkthrough::PaperSize paper) override;
    std::filesystem::path write_docx(const walkthrough::Walkthrough& walkthrough,
                                     const std::filesystem::path& folder,
                                     const walkthrough::DocumentFormat& format,
                                     walkthrough::PaperSize paper) override;
    void write_markdown_pdf(std::string_view text, const std::filesystem::path& target,
                            const std::filesystem::path& folder, std::string_view title,
                            const walkthrough::Look& look, walkthrough::PaperSize paper) override;
    void write_markdown_docx(std::string_view text, const std::filesystem::path& target,
                             const std::filesystem::path& folder, std::string_view title,
                             const walkthrough::Look& look, walkthrough::PaperSize paper) override;
};

} // namespace fv::documents
