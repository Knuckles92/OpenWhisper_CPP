#pragma once

#include <QImage>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Reads a written PDF back with PDFium, as OpenWhisper's tests/pdf_reader.py does through
// pypdfium2. The library is loaded by path from FIORIVOCE_TEST_PDFIUM (pypdfium2's pdfium.dll,
// read-only) and stays loaded until the process exits; the dozen FPDF_* entry points it calls are
// declared here, so no PDFium header is vendored. Affinity: any one thread at a time (calls are
// serialized, as PDFium is not thread-safe).
namespace fv::documents::test {

class PdfiumReader {
public:
    // The reader, or nullptr when FIORIVOCE_TEST_PDFIUM is unset. Throws std::runtime_error when
    // the variable names a library that cannot be loaded or lacks an entry point.
    static PdfiumReader* get();

    // Each page's text, in reading order (get_text_range), as UTF-8.
    std::vector<std::string> pageTexts(const std::filesystem::path& pdf) const;
    // The document information keys pypdfium2's get_metadata_dict reads: Title, Author, Subject,
    // Keywords, Creator, Producer, CreationDate and ModDate, each "" when absent.
    std::map<std::string, std::string> metadata(const std::filesystem::path& pdf) const;
    // The page's width and height in points.
    std::pair<double, double> pageSize(const std::filesystem::path& pdf, int index) const;
    // The page at `scale` pixels per point, transparent where nothing is painted (ARGB32).
    QImage render(const std::filesystem::path& pdf, int index, double scale = 1.0) const;

    struct Api;
    ~PdfiumReader();
    PdfiumReader(const PdfiumReader&) = delete;
    PdfiumReader& operator=(const PdfiumReader&) = delete;

private:
    explicit PdfiumReader(std::unique_ptr<Api> api);

    std::unique_ptr<Api> api_;
};

// The reader, or SKIP the running test with a message when FIORIVOCE_TEST_PDFIUM is unset.
PdfiumReader& pdfiumOrSkip();

} // namespace fv::documents::test
