// QImageShot: a kept screenshot handed to the walkthrough core.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>

#include <QColor>
#include <QImage>
#include <QString>

#include "testkit/testkit.h"
#include "ui_snap/qimage_shot.h"
#include "walkthrough/shot_image.h"

using fv::ui_snap::QImageShot;

namespace {

QImage shot(QColor color, double dpr = 2.0) {
    QImage image(80, 50, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    image.setDevicePixelRatio(dpr);
    return image;
}

} // namespace

TEST_CASE("a shot reports the image's device size and ratio", "[qimage_shot]") {
    const QImageShot image(shot(Qt::red));
    CHECK(image.width() == 80);
    CHECK(image.height() == 50);
    CHECK(image.device_pixel_ratio() == 2.0);
}

TEST_CASE("a shot keeps its own copy of the pixels", "[qimage_shot]") {
    QImage source = shot(Qt::red);
    const QImageShot image(source);

    source.fill(Qt::blue);

    CHECK(image.image().pixelColor(10, 10) == QColor(Qt::red));
}

TEST_CASE("a shot writes and encodes a PNG, also off the GUI thread", "[qimage_shot]") {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path path = root.path() / u8"shot-1 é.png";
    const fv::walkthrough::ShotImagePtr image = std::make_shared<QImageShot>(shot(Qt::green));

    std::thread writer([&] { image->save_png(path); });
    writer.join();

    QImage written;
    REQUIRE(written.load(QString::fromStdU16String(path.u16string()), "PNG"));
    CHECK(written.size() == QSize(80, 50));
    CHECK(written.pixelColor(5, 5) == QColor(Qt::green));

    const std::vector<std::uint8_t> bytes = image->png_bytes();
    REQUIRE(bytes.size() > 8);
    CHECK(bytes[1] == 'P');
    QImage decoded;
    REQUIRE(decoded.loadFromData(bytes.data(), static_cast<int>(bytes.size()), "PNG"));
    CHECK(decoded.pixelColor(5, 5) == QColor(Qt::green));
}

TEST_CASE("a shot that cannot be written says so", "[qimage_shot]") {
    fv::testkit::TempDataRoot root;
    const QImageShot image(shot(Qt::red));
    CHECK_THROWS_AS(image.save_png(root.path() / "missing" / "shot-1.png"), std::runtime_error);
}
