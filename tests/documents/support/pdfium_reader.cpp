#include "support/pdfium_reader.h"

#include <QLibrary>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>

#include "base/utf8.h"
#include "testkit/testkit.h"

namespace fv::documents::test {

namespace {

// PDFium's handles, opaque here.
using Document = void*;
using Page = void*;
using TextPage = void*;
using Bitmap = void*;

// FPDF_LIBRARY_CONFIG version 2, as pypdfium2 passes it.
struct LibraryConfig {
    int version;
    const char** userFontPaths;
    void* isolate;
    unsigned int v8EmbedderSlot;
};

// FPDF_ANNOT: draw annotations, pypdfium2's default.
constexpr int kRenderAnnotations = 0x01;

std::mutex& calls() {
    static std::mutex mutex;
    return mutex;
}

std::string fromUtf16(const unsigned short* text, std::size_t count) {
    QString value;
    value.reserve(static_cast<qsizetype>(count));
    for (std::size_t index = 0; index < count; ++index) {
        value.append(QChar(static_cast<char16_t>(text[index])));
    }
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

struct PdfiumReader::Api {
    QLibrary library;
    void (*initLibraryWithConfig)(const LibraryConfig*) = nullptr;
    Document (*loadMemDocument64)(const void*, std::size_t, const char*) = nullptr;
    void (*closeDocument)(Document) = nullptr;
    int (*getPageCount)(Document) = nullptr;
    Page (*loadPage)(Document, int) = nullptr;
    void (*closePage)(Page) = nullptr;
    float (*getPageWidthF)(Page) = nullptr;
    float (*getPageHeightF)(Page) = nullptr;
    unsigned long (*getMetaText)(Document, const char*, void*, unsigned long) = nullptr;
    TextPage (*textLoadPage)(Page) = nullptr;
    void (*textClosePage)(TextPage) = nullptr;
    int (*textCountChars)(TextPage) = nullptr;
    int (*textGetText)(TextPage, int, int, unsigned short*) = nullptr;
    Bitmap (*bitmapCreate)(int, int, int) = nullptr;
    int (*bitmapFillRect)(Bitmap, int, int, int, int, unsigned long) = nullptr;
    void (*renderPageBitmap)(Bitmap, Page, int, int, int, int, int, int) = nullptr;
    void* (*bitmapGetBuffer)(Bitmap) = nullptr;
    int (*bitmapGetStride)(Bitmap) = nullptr;
    void (*bitmapDestroy)(Bitmap) = nullptr;

    template <class F> void resolve(F& function, const char* name) {
        function = reinterpret_cast<F>(library.resolve(name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("PDFium has no ") + name + ": " +
                                     library.errorString().toStdString());
        }
    }
};

namespace {

// A loaded document and the bytes it reads from, which PDFium needs until it is closed.
class Open {
public:
    Open(const PdfiumReader::Api& api, const std::filesystem::path& pdf) : api_(api) {
        std::ifstream in(pdf, std::ios::binary);
        bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!in && !in.eof()) {
            throw std::runtime_error("Could not read " + path_to_utf8(pdf));
        }
        doc_ = api_.loadMemDocument64(bytes_.data(), bytes_.size(), nullptr);
        if (doc_ == nullptr) {
            throw std::runtime_error("PDFium could not open " + path_to_utf8(pdf));
        }
    }
    ~Open() { api_.closeDocument(doc_); }
    Open(const Open&) = delete;
    Open& operator=(const Open&) = delete;

    Document doc() const { return doc_; }

private:
    const PdfiumReader::Api& api_;
    std::vector<char> bytes_;
    Document doc_ = nullptr;
};

class OpenPage {
public:
    OpenPage(const PdfiumReader::Api& api, Document doc, int index) : api_(api) {
        page_ = api_.loadPage(doc, index);
        if (page_ == nullptr) {
            throw std::runtime_error("PDFium could not load page " + std::to_string(index));
        }
    }
    ~OpenPage() { api_.closePage(page_); }
    OpenPage(const OpenPage&) = delete;
    OpenPage& operator=(const OpenPage&) = delete;

    Page page() const { return page_; }

private:
    const PdfiumReader::Api& api_;
    Page page_ = nullptr;
};

} // namespace

PdfiumReader* PdfiumReader::get() {
    static std::once_flag once;
    static PdfiumReader* reader = nullptr;
    static std::string failure;
    std::call_once(once, [] {
        const auto path = testkit::get_env("OPENWHISPER_CPP_TEST_PDFIUM");
        if (!path || path->empty()) {
            return;
        }
        auto api = std::make_unique<Api>();
        api->library.setFileName(QString::fromUtf8(path->c_str()));
        if (!api->library.load()) {
            failure = "Could not load " + *path + ": " + api->library.errorString().toStdString();
            return;
        }
        try {
            api->resolve(api->initLibraryWithConfig, "FPDF_InitLibraryWithConfig");
            api->resolve(api->loadMemDocument64, "FPDF_LoadMemDocument64");
            api->resolve(api->closeDocument, "FPDF_CloseDocument");
            api->resolve(api->getPageCount, "FPDF_GetPageCount");
            api->resolve(api->loadPage, "FPDF_LoadPage");
            api->resolve(api->closePage, "FPDF_ClosePage");
            api->resolve(api->getPageWidthF, "FPDF_GetPageWidthF");
            api->resolve(api->getPageHeightF, "FPDF_GetPageHeightF");
            api->resolve(api->getMetaText, "FPDF_GetMetaText");
            api->resolve(api->textLoadPage, "FPDFText_LoadPage");
            api->resolve(api->textClosePage, "FPDFText_ClosePage");
            api->resolve(api->textCountChars, "FPDFText_CountChars");
            api->resolve(api->textGetText, "FPDFText_GetText");
            api->resolve(api->bitmapCreate, "FPDFBitmap_Create");
            api->resolve(api->bitmapFillRect, "FPDFBitmap_FillRect");
            api->resolve(api->renderPageBitmap, "FPDF_RenderPageBitmap");
            api->resolve(api->bitmapGetBuffer, "FPDFBitmap_GetBuffer");
            api->resolve(api->bitmapGetStride, "FPDFBitmap_GetStride");
            api->resolve(api->bitmapDestroy, "FPDFBitmap_Destroy");
        } catch (const std::runtime_error& error) {
            failure = error.what();
            return;
        }
        const LibraryConfig config{2, nullptr, nullptr, 0};
        api->initLibraryWithConfig(&config);
        // Never destroyed: the library stays loaded and initialized until the process exits.
        reader = new PdfiumReader(std::move(api));
    });
    if (!failure.empty()) {
        throw std::runtime_error(failure);
    }
    return reader;
}

PdfiumReader::PdfiumReader(std::unique_ptr<Api> api) : api_(std::move(api)) {}

PdfiumReader::~PdfiumReader() = default;

std::vector<std::string> PdfiumReader::pageTexts(const std::filesystem::path& pdf) const {
    std::lock_guard lock(calls());
    const Open open(*api_, pdf);
    std::vector<std::string> texts;
    const int count = api_->getPageCount(open.doc());
    for (int index = 0; index < count; ++index) {
        const OpenPage page(*api_, open.doc(), index);
        TextPage text = api_->textLoadPage(page.page());
        if (text == nullptr) {
            throw std::runtime_error("PDFium could not read the text of page " +
                                     std::to_string(index));
        }
        const int chars = api_->textCountChars(text);
        std::string value;
        if (chars > 0) {
            // Generated characters can make the text longer than the character count.
            std::vector<unsigned short> buffer(static_cast<std::size_t>(chars) * 2 + 2);
            const int written = api_->textGetText(text, 0, chars, buffer.data());
            if (written > 1) {
                value = fromUtf16(buffer.data(), static_cast<std::size_t>(written - 1));
            }
        }
        api_->textClosePage(text);
        texts.push_back(std::move(value));
    }
    return texts;
}

std::map<std::string, std::string> PdfiumReader::metadata(const std::filesystem::path& pdf) const {
    std::lock_guard lock(calls());
    const Open open(*api_, pdf);
    std::map<std::string, std::string> values;
    for (const char* key : {"Title", "Author", "Subject", "Keywords", "Creator", "Producer",
                            "CreationDate", "ModDate"}) {
        const unsigned long size = api_->getMetaText(open.doc(), key, nullptr, 0);
        std::string value;
        if (size > 2) {
            std::vector<unsigned short> buffer(size / 2);
            api_->getMetaText(open.doc(), key, buffer.data(), size);
            value = fromUtf16(buffer.data(), buffer.size() - 1);
        }
        values[key] = std::move(value);
    }
    return values;
}

std::pair<double, double> PdfiumReader::pageSize(const std::filesystem::path& pdf,
                                                 int index) const {
    std::lock_guard lock(calls());
    const Open open(*api_, pdf);
    const OpenPage page(*api_, open.doc(), index);
    return {api_->getPageWidthF(page.page()), api_->getPageHeightF(page.page())};
}

QImage PdfiumReader::render(const std::filesystem::path& pdf, int index, double scale) const {
    std::lock_guard lock(calls());
    const Open open(*api_, pdf);
    const OpenPage page(*api_, open.doc(), index);
    const int width = static_cast<int>(std::ceil(api_->getPageWidthF(page.page()) * scale));
    const int height = static_cast<int>(std::ceil(api_->getPageHeightF(page.page()) * scale));
    Bitmap bitmap = api_->bitmapCreate(width, height, 1);
    if (bitmap == nullptr) {
        throw std::runtime_error("PDFium could not make a bitmap");
    }
    api_->bitmapFillRect(bitmap, 0, 0, width, height, 0x00000000UL);
    api_->renderPageBitmap(bitmap, page.page(), 0, 0, width, height, 0, kRenderAnnotations);
    // PDFium's BGRA is Qt's ARGB32 on a little-endian machine.
    const QImage image(static_cast<const uchar*>(api_->bitmapGetBuffer(bitmap)), width, height,
                       api_->bitmapGetStride(bitmap), QImage::Format_ARGB32);
    QImage copy = image.copy();
    api_->bitmapDestroy(bitmap);
    return copy;
}

PdfiumReader& pdfiumOrSkip() {
    PdfiumReader* reader = PdfiumReader::get();
    if (reader == nullptr) {
        SKIP("OPENWHISPER_CPP_TEST_PDFIUM is not set: the PDF is not read back with PDFium");
    }
    return *reader;
}

} // namespace fv::documents::test
