#include "meeting_intel/pdf_text.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include "base/log.h"
#include "meeting_intel/detail/pdfium_api.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::meeting_intel {

namespace fs = std::filesystem;

namespace {

constexpr log::Logger logger("meeting.context_folder");

struct Pdfium {
    std::mutex mutex; // every PDFium call, the path and the libraries below
    std::optional<fs::path> path;
    std::map<fs::path, detail::PdfiumApi> loaded;
    std::set<fs::path> failed; // paths whose load failure was already logged
    bool missing_logged = false;
};

Pdfium& pdfium() {
    static Pdfium* state = new Pdfium(); // never destroyed: the libraries stay loaded
    return *state;
}

// The API for the configured path, loading and initializing it once; nullptr when none.
const detail::PdfiumApi* api_locked(Pdfium& state) {
    if (!state.path) {
        if (!state.missing_logged) {
            state.missing_logged = true;
            logger.warning("PDF text needs PDFium, which is not set up; PDFs in the knowledge "
                           "folder are skipped");
        }
        return nullptr;
    }
    std::error_code error;
    fs::path absolute = fs::absolute(*state.path, error);
    if (error) {
        absolute = *state.path;
    }
    if (const auto found = state.loaded.find(absolute); found != state.loaded.end()) {
        return &found->second;
    }
    if (state.failed.contains(absolute)) {
        return nullptr;
    }
    auto api = detail::load_pdfium(absolute);
    if (!api) {
        state.failed.insert(absolute);
        logger.warning("{} PDFs in the knowledge folder are skipped", api.error());
        return nullptr;
    }
    api->init_library();
    return &state.loaded.emplace(absolute, *api).first->second;
}

std::string from_utf16(const unsigned short* text, std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) {
        char32_t unit = text[index];
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < count && text[index + 1] >= 0xDC00 &&
            text[index + 1] <= 0xDFFF) {
            unit = 0x10000 + ((unit - 0xD800) << 10) + (text[index + 1] - 0xDC00);
            ++index;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            unit = 0xFFFD;
        }
        py::append_utf8(out, unit);
    }
    return out;
}

std::string without_carriage_returns(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            continue;
        }
        out += text[index];
    }
    return out;
}

class Document {
public:
    Document(const detail::PdfiumApi& api, detail::FpdfDocument document)
        : api_(api), document_(document) {}
    ~Document() { api_.close_document(document_); }
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    std::string page_text(int index) const {
        detail::FpdfPage page = api_.load_page(document_, index);
        if (page == nullptr) {
            throw PdfError("PDFium could not load a page");
        }
        detail::FpdfTextPage text_page = api_.text_load_page(page);
        if (text_page == nullptr) {
            api_.close_page(page);
            throw PdfError("PDFium could not read a page's text");
        }
        std::string text;
        const int chars = api_.text_count_chars(text_page);
        if (chars > 0) {
            // Generated characters can make the text longer than the character count.
            std::vector<unsigned short> buffer(static_cast<std::size_t>(chars) * 2 + 2);
            const int written = api_.text_get_text(text_page, 0, chars, buffer.data());
            if (written > 1) {
                text = from_utf16(buffer.data(), static_cast<std::size_t>(written - 1));
            }
        }
        api_.text_close_page(text_page);
        api_.close_page(page);
        return without_carriage_returns(std::move(text));
    }

    int page_count() const { return api_.get_page_count(document_); }

private:
    const detail::PdfiumApi& api_;
    detail::FpdfDocument document_;
};

} // namespace

void set_pdfium_path(std::optional<fs::path> library) {
    Pdfium& state = pdfium();
    std::lock_guard lock(state.mutex);
    state.path = std::move(library);
}

std::optional<fs::path> pdfium_path() {
    Pdfium& state = pdfium();
    std::lock_guard lock(state.mutex);
    return state.path;
}

std::optional<std::string> pdf_text(const fs::path& pdf, int max_pages) {
    std::ifstream in(pdf, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                "could not open the PDF");
    }
    Pdfium& state = pdfium();
    // Looked up before the file is read: an empty text is not cached, so without PDFium every
    // search would otherwise read each PDF of the folder (up to 8 MiB apiece) again. A loaded
    // library is never removed from `state.loaded`, so the pointer stays valid while unlocked.
    const detail::PdfiumApi* api = nullptr;
    {
        std::lock_guard lock(state.mutex);
        api = api_locked(state);
    }
    if (api == nullptr) {
        return std::nullopt;
    }
    // PDFium reads from memory: its path loader takes a narrow path, which cannot name every
    // file on Windows. The bytes must outlive the document.
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};
    std::lock_guard lock(state.mutex);
    // An empty password opens a PDF encrypted for its owner only, as pypdf's decrypt("") does.
    detail::FpdfDocument handle = api->load_mem_document64(bytes.data(), bytes.size(), "");
    if (handle == nullptr) {
        if (api->get_last_error() == detail::kFpdfErrPassword) {
            return std::string();
        }
        throw PdfError("PDFium could not open the PDF");
    }
    const Document document(*api, handle);
    std::vector<std::string> pages;
    const int count = document.page_count();
    for (int index = 0; index < count && index < max_pages; ++index) {
        std::string text = document.page_text(index);
        if (!py::strip(text).empty()) {
            pages.push_back(std::move(text));
        }
    }
    std::string joined;
    for (std::size_t index = 0; index < pages.size(); ++index) {
        joined += (index == 0 ? "" : "\n") + pages[index];
    }
    return joined;
}

} // namespace fv::meeting_intel
