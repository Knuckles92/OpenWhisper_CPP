#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <QImage>

#include "walkthrough/shot_image.h"

namespace fv::ui_snap {

// A kept screenshot handed to the walkthrough core (walkthrough::ShotImage) over a QImage. The
// image is deep-copied on construction, as Python copies a QImage before queuing it, so the
// caller's later edits never reach the writer thread. Affinity: immutable, any thread (QImage
// encodes PNGs off the GUI thread).
class QImageShot final : public walkthrough::ShotImage {
public:
    explicit QImageShot(const QImage& image);

    int width() const override;
    int height() const override;
    double device_pixel_ratio() const override;
    // QImage::save(path, "PNG"); throws std::runtime_error when it reports a failure.
    void save_png(const std::filesystem::path& path) const override;
    std::vector<std::uint8_t> png_bytes() const override;

    const QImage& image() const { return image_; }

private:
    QImage image_;
};

} // namespace fv::ui_snap
