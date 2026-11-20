#pragma once

#include <QPageSize>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>

#include "documents/composer.h"
#include "walkthrough/document.h"

// A walkthrough or its write-up as a Word document (services/walkthrough/word.py). The composers
// that lay out the PDF lay out the Word document, so it has the same words, screenshots, fonts,
// colours and spacing. Word does the pagination: each screenshot section is kept on one page, as
// in the PDF, and headings stay with what follows them. Affinity: as pdf.h; like the PDF it is
// made with QImage and QTextDocument only.
namespace fv::documents {

// Border widths are in eighths of a point: the screenshots' hairline, rounded, at least 2.
inline constexpr int kHairline = std::max(2, static_cast<int>(kFrameWidthPt * 8 + 0.5));
inline constexpr int kEmuPerPt = 12700;

// Writes folder/walkthrough.docx, the walkthrough as its PDF shows it, on `pageSize` or the
// locale's paper; the path written. Its title property is the PDF's ("<title or Walkthrough> ·
// <when>"). Throws walkthrough::DocumentError("Could not write <path>: <why>") when the file
// cannot be written.
std::filesystem::path writeDocx(const walkthrough::Walkthrough& walkthrough,
                                const std::filesystem::path& folder,
                                const walkthrough::DocumentFormat& format = {},
                                const std::optional<QPageSize>& pageSize = std::nullopt);

// Writes `text`, any Markdown, as a Word document at `path` in the walkthrough's look: the
// counterpart of writeMarkdownPdf, with the same arguments. Returns `path`.
std::filesystem::path writeMarkdownDocx(std::string_view text, const std::filesystem::path& path,
                                        const std::filesystem::path& folder, std::string_view title,
                                        const std::optional<QPageSize>& pageSize = std::nullopt,
                                        const walkthrough::Look& look = {});

} // namespace fv::documents
