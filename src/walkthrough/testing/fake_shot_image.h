#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "walkthrough/shot_image.h"

namespace fv::walkthrough {

// A screenshot of one solid colour, saved as a real 8-bit RGB PNG (stored, uncompressed deflate,
// so no zlib is needed). Told to, its save writes the first half of the file and then fails, as
// a full disk does. pixel() reads such a PNG back.
// Affinity: immutable apart from the save counter; any thread.
class FakeShotImage final : public ShotImage {
public:
    enum class Save { Works, FailsHalfWritten };

    FakeShotImage(int width, int height, std::uint32_t rgb, Save save = Save::Works,
                  double device_pixel_ratio = 1.0)
        : width_(width), height_(height), rgb_(rgb), save_(save), ratio_(device_pixel_ratio) {}

    int width() const override { return width_; }
    int height() const override { return height_; }
    double device_pixel_ratio() const override { return ratio_; }

    void save_png(const std::filesystem::path& path) const override {
        saves_.fetch_add(1);
        const auto bytes = png_bytes();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Could not open the screenshot file");
        }
        const std::size_t count = save_ == Save::Works ? bytes.size() : bytes.size() / 2;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(count));
        out.close();
        if (save_ != Save::Works) {
            throw std::runtime_error("No space left on the device");
        }
        if (!out) {
            throw std::runtime_error("Could not write the screenshot file");
        }
    }

    std::vector<std::uint8_t> png_bytes() const override {
        std::vector<std::uint8_t> raw;
        for (int y = 0; y < height_; ++y) {
            raw.push_back(0); // filter: none
            for (int x = 0; x < width_; ++x) {
                raw.push_back(static_cast<std::uint8_t>(rgb_ >> 16));
                raw.push_back(static_cast<std::uint8_t>(rgb_ >> 8));
                raw.push_back(static_cast<std::uint8_t>(rgb_));
            }
        }
        std::vector<std::uint8_t> zlib{0x78, 0x01};
        std::size_t at = 0;
        do {
            const std::size_t length = std::min<std::size_t>(65535, raw.size() - at);
            const bool last = at + length == raw.size();
            zlib.push_back(last ? 1 : 0);
            zlib.push_back(static_cast<std::uint8_t>(length));
            zlib.push_back(static_cast<std::uint8_t>(length >> 8));
            zlib.push_back(static_cast<std::uint8_t>(~length));
            zlib.push_back(static_cast<std::uint8_t>(~length >> 8));
            zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                        raw.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
        } while (at < raw.size());
        put32(zlib, adler32(raw));

        std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        std::vector<std::uint8_t> header;
        put32(header, static_cast<std::uint32_t>(width_));
        put32(header, static_cast<std::uint32_t>(height_));
        header.insert(header.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
        chunk(png, "IHDR", header);
        chunk(png, "IDAT", zlib);
        chunk(png, "IEND", {});
        return png;
    }

    // How many times save_png ran.
    int saves() const { return saves_.load(); }

    // The colour at (x, y) of a PNG this class wrote, as 0xRRGGBB; nullopt for anything else.
    static std::optional<std::uint32_t> pixel(const std::vector<std::uint8_t>& png, int x, int y) {
        if (png.size() < 33 || png[12] != 'I' || png[13] != 'H') {
            return std::nullopt;
        }
        const auto read32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(png[at]) << 24 |
                   static_cast<std::uint32_t>(png[at + 1]) << 16 |
                   static_cast<std::uint32_t>(png[at + 2]) << 8 | png[at + 3];
        };
        const auto width = static_cast<int>(read32(16));
        std::vector<std::uint8_t> raw;
        std::size_t at = 8;
        while (at + 8 <= png.size()) {
            const std::uint32_t length = read32(at);
            const std::string type(png.begin() + static_cast<std::ptrdiff_t>(at + 4),
                                   png.begin() + static_cast<std::ptrdiff_t>(at + 8));
            if (at + 12 + length > png.size()) {
                return std::nullopt;
            }
            if (type == "IDAT") {
                std::size_t block = at + 8 + 2;
                while (block + 5 <= at + 8 + length) {
                    const bool last = (png[block] & 1) != 0;
                    const std::size_t size = png[block + 1] | png[block + 2] << 8;
                    raw.insert(raw.end(), png.begin() + static_cast<std::ptrdiff_t>(block + 5),
                               png.begin() + static_cast<std::ptrdiff_t>(block + 5 + size));
                    block += 5 + size;
                    if (last) {
                        break;
                    }
                }
            }
            at += 12 + length;
        }
        const std::size_t offset = static_cast<std::size_t>(y) * (1 + 3 * width) + 1 + 3 * x;
        if (offset + 3 > raw.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(raw[offset]) << 16 |
               static_cast<std::uint32_t>(raw[offset + 1]) << 8 | raw[offset + 2];
    }

private:
    static void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 24));
        out.push_back(static_cast<std::uint8_t>(value >> 16));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }

    static std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (const auto byte : data) {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        return b << 16 | a;
    }

    static std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
        std::uint32_t crc = 0xFFFFFFFFU;
        for (const auto byte : data) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            }
        }
        return ~crc;
    }

    static void chunk(std::vector<std::uint8_t>& png, const char* type,
                      const std::vector<std::uint8_t>& data) {
        put32(png, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        png.insert(png.end(), body.begin(), body.end());
        put32(png, crc32(body));
    }

    int width_;
    int height_;
    std::uint32_t rgb_;
    Save save_;
    double ratio_;
    mutable std::atomic<int> saves_{0};
};

} // namespace fv::walkthrough
