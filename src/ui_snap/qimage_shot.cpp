#include "ui_snap/qimage_shot.h"

#include <stdexcept>

#include <QBuffer>
#include <QByteArray>
#include <QString>

namespace fv::ui_snap {

QImageShot::QImageShot(const QImage& image) : image_(image.copy()) {}

int QImageShot::width() const {
    return image_.width();
}

int QImageShot::height() const {
    return image_.height();
}

double QImageShot::device_pixel_ratio() const {
    return image_.devicePixelRatio();
}

void QImageShot::save_png(const std::filesystem::path& path) const {
    const QString file = QString::fromStdU16String(path.u16string());
    if (!image_.save(file, "PNG")) {
        throw std::runtime_error("Could not write the screenshot as a PNG");
    }
}

std::vector<std::uint8_t> QImageShot::png_bytes() const {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image_.save(&buffer, "PNG")) {
        throw std::runtime_error("Could not encode the screenshot as a PNG");
    }
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

} // namespace fv::ui_snap
