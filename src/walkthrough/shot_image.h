#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace fv::walkthrough {

// A kept screenshot, opaque to the core: fv_ui_snap implements it over QImage
// (ui_snap::QImageShot), the tests with testing/fake_shot_image.h. Holding the image
// immutable is what Python's QImage.copy() before queuing guarantees.
// Affinity: immutable, any thread.
class ShotImage {
public:
    virtual ~ShotImage() = default;
    virtual int width() const = 0; // device pixels
    virtual int height() const = 0;
    virtual double device_pixel_ratio() const = 0;
    // Writes the image as a PNG at `path`. Throws std::runtime_error when it cannot; a partial
    // file may be left behind, which the caller removes.
    virtual void save_png(const std::filesystem::path& path) const = 0;
    // The same PNG in memory, for clipboard images.
    virtual std::vector<std::uint8_t> png_bytes() const = 0;
};

using ShotImagePtr = std::shared_ptr<const ShotImage>;

} // namespace fv::walkthrough
