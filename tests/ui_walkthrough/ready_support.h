#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#include <QColor>
#include <QImage>
#include <QString>

#include "base/utf8.h"
#include "testkit/testkit.h"
#include "walkthrough/session.h"

// What the ready card's tests share: a finished walkthrough's folder in a temporary data folder,
// with its PDF, an optional Word document and an optional first screenshot.
namespace fvtest {

// A WalkthroughResult whose files are under a temporary folder of its own.
class ReadyFixture {
public:
    ReadyFixture() : folder_(root_.path() / "captures" / "2026-10-21 14-32-05") {
        std::filesystem::create_directories(folder_);
    }

    const std::filesystem::path& folder() const { return folder_; }

    // The document as the card is shown it: a PDF that exists, three screenshots over 1:12.
    fv::walkthrough::WalkthroughResult result(bool withWord = false, bool withShot = false) const {
        fv::walkthrough::WalkthroughResult out;
        out.folder = folder_;
        out.pdf = folder_ / "walkthrough.pdf";
        out.markdown = folder_ / "walkthrough.md";
        out.shots = 3;
        out.duration_s = 72;
        touch(out.pdf);
        if (withWord) {
            touch(out.docx());
        }
        if (withShot) {
            out.first_shot = folder_ / "shot-1.png";
            // Wider than the thumbnail's shape, so the crop is on the sides; its middle is red and
            // its sides are blue.
            QImage picture(300, 100, QImage::Format_RGB32);
            picture.fill(QColor(0x20, 0x40, 0xc0));
            for (int y = 0; y < picture.height(); ++y) {
                for (int x = 100; x < 200; ++x) {
                    picture.setPixelColor(x, y, QColor(0xc0, 0x20, 0x20));
                }
            }
            picture.save(QString::fromStdString(fv::path_to_utf8(*out.first_shot)), "PNG");
        }
        return out;
    }

private:
    static void touch(const std::filesystem::path& file) {
        std::ofstream(file, std::ios::binary) << "x";
    }

    fv::testkit::TempDataRoot root_;
    std::filesystem::path folder_;
};

} // namespace fvtest
