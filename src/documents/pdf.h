#pragma once

#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QSizeF>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "documents/painting.h"
#include "walkthrough/document.h"

// A walkthrough, or any Markdown, written as a PDF in the walkthrough's look
// (services/walkthrough/document.py:416-560). Affinity: any thread, once the QGuiApplication
// exists; safe to call from several threads at once, each call with its own document and writer.
namespace fv::documents {

// A written PDF and what went on each of its pages.
struct WrittenPdf {
    std::filesystem::path path;
    std::vector<PageRecord> pages;
};

// Writes folder/walkthrough.pdf: the sections laid out on pages, Letter where the system locale
// measures in US units, else A4, unless `pageSize` is given. A screenshot section's words,
// image and caption stay on one page. The file's own title, which viewers show in their title
// bar, is "<title or Walkthrough> · <when>" even when the page has no heading; its creator is
// the product. Throws walkthrough::DocumentError when the file cannot be written.
WrittenPdf writePdf(const walkthrough::Walkthrough& walkthrough,
                    const std::filesystem::path& folder,
                    const walkthrough::DocumentFormat& format = {},
                    const std::optional<QPageSize>& pageSize = std::nullopt);

// Writes `text`, any Markdown, as a PDF at `path` in the walkthrough's look, for a document
// someone else laid out, such as an agent's write-up. Its images are file names in `folder`;
// one that is missing is left out. Raw HTML is shown as text, never run. `look` sets the colours
// and font Markdown can't. Throws walkthrough::DocumentError when the file cannot be written.
WrittenPdf writeMarkdownPdf(std::string_view text, const std::filesystem::path& path,
                            const std::filesystem::path& folder, std::string_view title,
                            const std::optional<QPageSize>& pageSize = std::nullopt,
                            const walkthrough::Look& look = {});

// The documents' page: `pageSize`, else the locale's, inside the margins.
QPageLayout pageLayout(const std::optional<QPageSize>& pageSize = std::nullopt);
// Letter where the system locale measures in US units, A4 everywhere else.
QPageSize localPageSize();
// Letter for ImperialUSSystem, A4 for every other measurement system.
QPageSize pageSizeFor(QLocale::MeasurementSystem measurement);
// The paper a setting names; nullopt for Auto, which is the locale's.
std::optional<QPageSize> pageSizeFor(walkthrough::PaperSize paper);
// Layout units per inch of a QTextDocument without a paint device: it sizes point fonts at the
// screen's logical DPI (96 on Windows and Linux), which a new QImage carries too and, unlike
// the QScreen, belongs to no thread.
int layoutDpi();
// The page inside its margins, in whole layout units: whole units put page edges exactly where
// the layout's fixed-point arithmetic does, so a line moved to the next page measures as on it.
QSizeF textArea(const QPageLayout& layout, int dpi);

} // namespace fv::documents
