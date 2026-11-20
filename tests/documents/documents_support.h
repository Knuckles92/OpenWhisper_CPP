#pragma once

#include <QColor>
#include <QImage>
#include <QPageSize>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "documents/painting.h"
#include "documents/pdf.h"
#include "support/pdfium_reader.h"
#include "walkthrough/document.h"

namespace fv::documents::test {

// A new folder under the system temporary folder, removed with what it holds.
class TempFolder {
public:
    TempFolder() {
        std::random_device seed;
        const auto base = std::filesystem::temp_directory_path();
        for (;;) {
            path_ = base / ("fv-documents-test-" + std::to_string(seed()));
            if (std::filesystem::create_directory(path_)) {
                break;
            }
        }
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(std::string_view name) const {
        return path_ / path_from_utf8(name);
    }

private:
    std::filesystem::path path_;
};

// A PNG of one colour.
inline std::filesystem::path png(const std::filesystem::path& path, int width, int height,
                                 std::string_view color = "#3060c0") {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(QString::fromUtf8(color.data(), static_cast<qsizetype>(color.size()))));
    REQUIRE(image.save(QString::fromUtf8(path_to_utf8(path))));
    return path;
}

inline std::chrono::local_seconds local(int year, unsigned month, unsigned day, int hour,
                                        int minute, int second) {
    return std::chrono::local_days{std::chrono::year{year} / month / day} +
           std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
}

// A Tuesday afternoon.
inline std::chrono::zoned_seconds started() {
    return walkthrough::local_moment(std::chrono::locate_zone("America/Los_Angeles"),
                                     local(2026, 10, 20, 14, 32, 5));
}

inline walkthrough::Section section(std::string text,
                                    std::optional<std::filesystem::path> shot = std::nullopt,
                                    std::optional<double> kept_at_s = std::nullopt) {
    walkthrough::Section value;
    value.text = std::move(text);
    value.shot = std::move(shot);
    value.kept_at_s = kept_at_s;
    return value;
}

inline walkthrough::Walkthrough walkthroughOf(double duration_s,
                                              std::vector<walkthrough::Section> sections) {
    return {started(), duration_s, std::move(sections)};
}

inline std::string aboutShotless() {
    return "Spoken notes with screenshots, transcribed by " + std::string(identity().product) + ".";
}
inline std::string about() {
    return aboutShotless() + " Each screenshot comes right after the words that describe it.";
}

inline QPageSize letter() {
    return QPageSize(QPageSize::Letter);
}

inline std::string utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// Python's " ".join(text.split()): runs of whitespace, line and paragraph separators included,
// become one space, and the ends are trimmed.
inline std::string flat(std::string_view text) {
    return utf8(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()))
                    .replace(QChar(u' '), QChar(u' '))
                    .replace(QChar(u' '), QChar(u' '))
                    .simplified());
}

// A page's painted lines as one text: images dropped, a list marker's tab a space, lines
// joined by `separator`.
inline std::string recordText(const PageRecord& page, std::string_view separator = " ") {
    std::string text;
    for (std::size_t index = 0; index < page.lines.size(); ++index) {
        QString line = page.lines[index];
        line.remove(QChar(u'￼'));
        line.replace(QChar(u'\t'), QChar(u' '));
        if (index) {
            text += separator;
        }
        text += utf8(line);
    }
    return text;
}

// Each page's painted text, flattened as _pages() flattens PDFium's.
inline std::vector<std::string> recordPages(const WrittenPdf& pdf) {
    std::vector<std::string> pages;
    for (const PageRecord& page : pdf.pages) {
        pages.push_back(flat(recordText(page)));
    }
    return pages;
}

// Each page's text as PDFium reads it, flattened (_pages), or nothing without PDFium.
inline std::optional<std::vector<std::string>> pdfiumPages(const std::filesystem::path& pdf) {
    PdfiumReader* reader = PdfiumReader::get();
    if (reader == nullptr) {
        return std::nullopt;
    }
    std::vector<std::string> pages;
    for (const std::string& text : reader->pageTexts(pdf)) {
        pages.push_back(flat(text));
    }
    return pages;
}

// The page texts a test checks: the painter's record always, and PDFium's reading of the file
// when OPENWHISPER_CPP_TEST_PDFIUM names the library.
inline std::vector<std::pair<std::string, std::vector<std::string>>>
readings(const WrittenPdf& pdf) {
    std::vector<std::pair<std::string, std::vector<std::string>>> all;
    all.emplace_back("painted lines", recordPages(pdf));
    if (auto pages = pdfiumPages(pdf.path)) {
        all.emplace_back("PDFium", std::move(*pages));
    }
    return all;
}

inline bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

inline std::size_t count(std::string_view text, std::string_view part) {
    std::size_t found = 0;
    for (std::size_t at = text.find(part); at != std::string_view::npos;
         at = text.find(part, at + part.size())) {
        ++found;
    }
    return found;
}

// The widest row of `color` on `page`, in points (_widest), every fourth row.
inline int widest(const std::filesystem::path& pdf, int page, std::string_view color) {
    const QImage image = pdfiumOrSkip().render(pdf, page);
    const QColor target(QString::fromUtf8(color.data(), static_cast<qsizetype>(color.size())));
    int widest = 0;
    for (int y = 0; y < image.height(); y += 4) {
        int row = 0;
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == target) {
                ++row;
            }
        }
        widest = std::max(widest, row);
    }
    return widest;
}

// The log records written while it lives.
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<Sink>()) { log::add_sink(sink_); }
    ~LogCapture() { log::remove_sink(sink_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::string text() const {
        std::lock_guard lock(sink_->mutex);
        return sink_->text;
    }

private:
    struct Sink final : log::Sink {
        void write(const log::Record& record) override {
            std::lock_guard lock(mutex);
            text.append(record.message);
            text += '\n';
        }
        mutable std::mutex mutex;
        std::string text;
    };
    std::shared_ptr<Sink> sink_;
};

} // namespace fv::documents::test
